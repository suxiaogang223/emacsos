// Browser storage state-machine tests use real IndexedDB/Web Locks and a model
// control peer. Actual guest persistence is exercised separately in browser.cjs.
const { chromium } = require(process.env.EMACSOS_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const url = process.argv[2] || 'http://127.0.0.1:18082/';
(async () => {
    const browser = await chromium.launch({ headless: true,
        ...(process.env.EMACSOS_BROWSER ? { executablePath: process.env.EMACSOS_BROWSER } : {}),
        args: ['--no-sandbox'] });
    const page = await browser.newPage();
    const errors = [];
    const downloads = [];
    page.on('download', download => { downloads.push(download.suggestedFilename()); download.cancel(); });
    page.on('pageerror', error => errors.push(String(error)));
    try {
        await page.goto(url);
        const checks = await page.evaluate(async () => {
            const { BrowserDisk, ControlChannel, DATA_SIZE } = await import('./storage.js');
            const checks = [];
            function expect(condition, message) { if (!condition) throw new Error(message); }
            async function rejects(operation, marker) {
                try { await operation(); } catch (error) { expect(error.message.includes(marker), error.message); return; }
                throw new Error('Expected failure: ' + marker);
            }
            const channel = new ControlChannel();
            const request = channel.request('STATUS', 1000);
            await Promise.resolve();
            const wire = new TextDecoder().decode(new Uint8Array(channel.input));
            expect(/^EOS1 [0-9a-f]{16} 0000000000000001 STATUS\n$/.test(wire), 'Request framing');
            const deliver = value => [...new TextEncoder().encode(value)].forEach(byte => channel.receive(byte));
            deliver(`EOS1 ffffffffffffffff 0000000000000001 STATUS 0000000000000007 0 1 0\n`);
            deliver(`EOS1 ${channel.session} 0000000000000000 STATUS 0000000000000007 0 1 0\n`);
            expect(channel.pending.size === 1, 'Stale session/request consumed acknowledgement');
            deliver(`EOS1 ${channel.session} 0000000000000001 STATUS 0000000000000007 0 1 0\n`);
            expect((await request).generation === '0000000000000007', 'Reply parsing');
            checks.push('Control framing rejects stale sessions and unmatched requests');

            const first = new BrowserDisk(() => {});
            first.bytes = new Uint8Array(DATA_SIZE); first.bytes[0] = 41;
            first.diskId = crypto.randomUUID();
            await first.prepare();
            const second = new BrowserDisk(() => {});
            await rejects(() => second.ownership(), 'already open');
            second.close();
            checks.push('Web Locks refuse a second retained-disk writer');
            let generation = 1n, frozen = false, requests = [];
            const volume = first.bytes;
            first.machine = { FS: { readFile: () => volume } };
            first.mounted = true;
            first.control.request = async command => {
                requests.push(command);
                if (command === 'FREEZE') frozen = true;
                if (command === 'THAW') frozen = false;
                return { type: command === 'FREEZE' ? 'FROZEN' : command === 'THAW' ? 'THAWED' : 'STATUS',
                    generation: generation.toString(16).padStart(16, '0'), errno: 0, mounted: true, frozen };
            };
            await first.checkpoint();
            expect(requests[0] === 'FREEZE' && requests[1] === 'THAW' && !frozen, 'Copy/thaw ordering');
            expect(first.state === 'saved' && first.savedGeneration === '0000000000000001', 'Checkpoint completion');
            async function stored(db) {
                return await new Promise((resolve, reject) => {
                    const tx = db.transaction('disks'); const get = tx.objectStore('disks').get('default');
                    tx.oncomplete = () => resolve(get.result); tx.onabort = () => reject(tx.error);
                });
            }
            const saved = await stored(first.db);
            expect(new Uint8Array(saved.bytes)[0] === 41, 'Checkpoint bytes');
            volume[0] = 42; generation++;
            expect(new Uint8Array(saved.bytes)[0] === 41, 'Independent image ownership');
            checks.push('Freeze/copy/thaw precedes a completed atomic IndexedDB checkpoint');

            const actualDB = first.db;
            first.db = { transaction(...args) {
                const tx = actualDB.transaction(...args);
                const actualStore = tx.objectStore.bind(tx);
                tx.objectStore = name => {
                    const store = actualStore(name);
                    return { get: store.get.bind(store), put(record, key) { store.put(record, key); tx.abort(); } };
                };
                return tx;
            }};
            await rejects(() => first.checkpoint(), 'not committed');
            await first.checkpoint({exportImage:true});
            expect(first.state === 'error', 'Export concealed retention failure');
            first.db = actualDB;
            expect(new Uint8Array((await stored(actualDB)).bytes)[0] === 41, 'Aborted transaction replaced prior checkpoint');
            expect(!frozen, 'Failed retention left guest frozen');
            checks.push('Aborted retention preserves the previous image and resumes guest writes');

            requests = [];
            await Promise.all([first.checkpoint(), first.checkpoint()]);
            expect(requests.join(',') === 'FREEZE,THAW,STATUS,FREEZE,THAW,STATUS', 'Concurrent checkpoints interleaved');
            checks.push('Concurrent checkpoint requests serialize without overlapping barriers');
            const peer = first.control.request;

            generation++;
            const retainedGeneration = generation.toString(16).padStart(16, '0');
            const restartBarriers = [];
            first.db = {transaction(...args) {
                const tx = actualDB.transaction(...args);
                tx.addEventListener('complete', () => restartBarriers.push(frozen));
                const actualStore = tx.objectStore.bind(tx);
                tx.objectStore = name => {
                    const store = actualStore(name);
                    return {get:store.get.bind(store),put(record,key) {
                        restartBarriers.push(frozen); return store.put(record,key);
                    }};
                };
                return tx;
            }};
            await first.checkpoint({restart:true});
            first.db = actualDB;
            expect(restartBarriers.length === 2 && restartBarriers.every(Boolean) && frozen,
                'Restart thawed before retention/reload');
            expect(first.state === 'saved' && !first.pending, 'Retained restart remained pending');
            await first.recover();
            expect(!frozen, 'Explicit restart cancellation did not resume');
            checks.push('Restart holds DATA frozen through transaction completion and returns a saved barrier');
            generation++;
            first.db = {transaction(...args) {
                const tx = actualDB.transaction(...args);
                const actualStore = tx.objectStore.bind(tx);
                tx.objectStore = name => {
                    const store = actualStore(name);
                    return {get:store.get.bind(store),put(record,key) {
                        expect(frozen, 'Failed restart had already thawed');
                        store.put(record,key); tx.abort();
                    }};
                };
                return tx;
            }};
            await rejects(() => first.checkpoint({restart:true}), 'not committed');
            expect(!frozen && !first.unknown, 'Failed restart did not confirm guest resume');
            expect((await stored(actualDB)).generation === retainedGeneration, 'Failed restart replaced checkpoint');
            first.control.request = async command => {
                if (command === 'THAW') throw new Error('Simulated lost thaw acknowledgement');
                return peer(command);
            };
            await rejects(() => first.checkpoint({restart:true}), 'not committed');
            expect(frozen && first.unknown, 'Lost restart thaw was labeled writable');
            first.db = actualDB; first.control.request = peer;
            await first.recover();
            expect(!frozen && !first.unknown, 'Failed restart state did not recover');
            checks.push('Failed restart preserves old DATA and confirms thaw or exposes unresolved frozen state');
            first.control.request = async command => {
                if (command === 'FREEZE' || command === 'STATUS') throw new Error('Simulated lost acknowledgement');
                return peer(command);
            };
            await rejects(() => first.checkpoint(), 'Simulated lost acknowledgement');
            expect(first.unknown, 'Lost freeze acknowledged as writable');
            await rejects(() => first.checkpoint(), 'unresolved');
            first.control.request = peer;
            await first.recover();
            expect(!first.unknown && !first.frozen, 'State recovery did not confirm thaw');
            checks.push('Unresolved freeze blocks storage actions until confirmed recovery');

            await rejects(() => first.import(new File([new Uint8Array(12)], 'bad.img')), '64 MiB');
            const prior = await stored(actualDB);
            volume[0] = 43; generation++;
            let enter, release;
            const entered = new Promise(resolve => { enter = resolve; });
            const barrier = new Promise(resolve => { release = resolve; });
            first.control.request = async command => {
                if (command === 'FREEZE') { enter(); await barrier; }
                return peer(command);
            };
            const late = first.checkpoint();
            await entered;
            const closing = first.close();
            release();
            await rejects(() => late, 'session has ended');
            await closing;
            checks.push('An ended session cannot promote a delayed checkpoint');
            const restored = new BrowserDisk(() => {});
            await restored.prepare();
            expect(restored.source === 'restored' && restored.bytes[0] === 42, 'Automatic restore');
            restored.control.request = async () => ({ type:'STATUS', generation:'0000000000000000', errno:84, mounted:false, frozen:false });
            await restored.import(new File([new Uint8Array(DATA_SIZE)], 'corrupt.img'));
            await restored.attach({ FS:{} });
            const after = await stored(restored.db);
            expect(restored.state === 'error' && after.sha256 === prior.sha256, 'Rejected import replaced retained disk');
            const corrupt = { ...after, sha256: '0'.repeat(64) };
            await new Promise((resolve, reject) => {
                const tx = restored.db.transaction('disks', 'readwrite'); tx.objectStore('disks').put(corrupt, 'default');
                tx.oncomplete = resolve; tx.onabort = () => reject(tx.error);
            });
            await restored.close();
            const invalid = new BrowserDisk(() => {});
            await rejects(() => invalid.prepare(), 'failed validation');
            expect((await stored(invalid.db)).sha256 === corrupt.sha256, 'Invalid retained image was replaced');
            await invalid.close();
            checks.push('Restore validates integrity; corrupt/unmounted imports never replace retained data');

            const unavailable = new BrowserDisk(() => {});
            unavailable.sessionOnly = true; unavailable.bytes = new Uint8Array(DATA_SIZE); unavailable.diskId = crypto.randomUUID();
            await unavailable.prepare(); unavailable.machine = { FS: {readFile: () => volume} }; unavailable.mounted = true;
            unavailable.control.request = peer;
            await unavailable.checkpoint();
            expect(unavailable.state === 'session-only', 'Unavailable storage reported saved');
            await rejects(() => unavailable.checkpoint({restart:true}), 'retention is unavailable');
            expect(!frozen, 'Failed session-only restart left guest frozen');
            await unavailable.poll();
            expect(unavailable.state === 'session-only', 'Status polling concealed unavailable retention');
            await unavailable.close();
            checks.push('Unavailable retention remains session-only and cannot silently restart');

            const faulted = new BrowserDisk(() => {});
            faulted.bytes = new Uint8Array(DATA_SIZE); faulted.diskId = crypto.randomUUID();
            await faulted.prepare();
            faulted.machine = {FS:{readFile:()=>volume}};faulted.mounted=true;
            faulted.control.request = async command => {
                if(command==='FREEZE')frozen=true;
                if(command==='THAW')frozen=false;
                return {type:command==='STATUS'?'STATUS':command==='FREEZE'?'FROZEN':'THAWED',
                    generation:'0000000000000009',errno:command==='STATUS'?5:0,mounted:true,frozen};
            };
            await faulted.checkpoint();
            expect(faulted.state==='error'&&faulted.healthError===5&&faulted.pending,
                'Mounted STATUS fault was labeled saved');
            expect((await stored(faulted.db)).generation==='0000000000000009',
                'Consistent pre-fault checkpoint was not retained');
            await rejects(()=>faulted.checkpoint(),'storage fault');
            faulted.applyStatus({type:'THAWED',generation:'0000000000000009',errno:0,mounted:true,frozen:false});
            expect(faulted.healthError===5,'Successful THAW hid a latched media fault');
            await faulted.close();
            checks.push('Mounted media faults stay visible and disable snapshots without hiding completed checkpoint identity');
            return checks;
        });
        assert.deepEqual(errors, []);
        assert.deepEqual(downloads, ['emacsos-data.img']);
        for (const check of checks) console.log('PASS:', check);
        const output = path.resolve(process.env.EMACSOS_STORAGE_REPORT || 'build/web-storage');
        fs.mkdirSync(output, {recursive:true});
        fs.writeFileSync(path.join(output, 'state-machine.json'), JSON.stringify({passed:true,
            scope:'Real IndexedDB/Web Locks with a modeled guest control peer, not guest persistence acceptance',checks},null,2)+'\n');
    } finally { await browser.close(); }
})().catch(error => { console.error(error); process.exitCode = 1; });
