// End-to-end DATA acceptance: every machine below runs the real Wasm guest.
const { chromium } = require(process.env.EMACSOS_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const url = process.argv[2] || 'http://127.0.0.1:18082/';
const output = path.resolve(process.env.EMACSOS_DISK_REPORT || 'build/web-disk');
fs.mkdirSync(output, {recursive:true});
const report = {passed:false, checks:[], errors:[], boots:[], scope:'Actual QEMU/Wasm DATA guest acceptance; no native interoperability claim'};
report.driverSource = execFileSync('git',['rev-parse','HEAD'],{encoding:'utf8'}).trim();
report.driverSourceDirty = Boolean(execFileSync('git',['status','--porcelain'],{encoding:'utf8'}).trim());
const webRoot = process.env.EMACSOS_WEB_ROOT || process.cwd();
report.webSource = execFileSync('git',['-C',webRoot,'rev-parse','HEAD'],{encoding:'utf8'}).trim();
report.webSourceDirty = Boolean(execFileSync('git',['-C',webRoot,'status','--porcelain'],{encoding:'utf8'}).trim());
report.webSourceScope = 'Local checkout selected for this server; not a server source attestation';
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const log = message => {report.checks.push(message);console.log('PASS:',message);};
let current;
async function instrumentation(page) {
    page.on('pageerror', error => report.errors.push(String(error)));
    await page.addInitScript(() => {
        const fetch = window.fetch.bind(window);
        window.fetch = async (...args) => {
            const response = await fetch(...args);
            const name = new URL(response.url).pathname;
            if ((name.endsWith('/emacsos.img') || name.endsWith('/data-empty.img')) && response.ok) {
                window[name.endsWith('/data-empty.img') ? '__templateHash' : '__imageHash'] = response.clone().arrayBuffer().then(async bytes =>
                    Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',bytes)),byte=>byte.toString(16).padStart(2,'0')).join(''));
            }
            return response;
        };
    });
    await page.route('**/vendor/xterm.js', async route => {
        const response = await route.fetch();
        await route.fulfill({response,body:await response.text()+`
window.__serial='';window.Terminal=new Proxy(window.Terminal,{construct(Target,args){
 const terminal=Reflect.construct(Target,args);const write=terminal.write.bind(terminal);const decoder=new TextDecoder();
 terminal.write=(bytes,...rest)=>{window.__serial+=typeof bytes==='string'?bytes:decoder.decode(bytes,{stream:true});return write(bytes,...rest);};return terminal;}});`});
    });
}
async function state(page,value) {
    await page.waitForFunction(value=>document.documentElement.dataset.machineState===value,value,{timeout:120000});
}
async function pageFor(context) {
    current=await context.newPage();
    await instrumentation(current);
    await current.goto(url);
    await state(current,'off');
    return current;
}
async function boot(page, storage='saved') {
    const begin=Date.now();
    await page.click('#boot');
    await state(page,'running');
    await page.waitForFunction(value=>document.documentElement.dataset.storageState===value,storage,{timeout:60000});
    if(storage==='session-only')await page.waitForFunction(()=>!document.getElementById('disk-export').disabled,null,{timeout:30000});
    const sha256=await page.evaluate(()=>window.__imageHash);
    assert.equal(sha256,report.build.components['build/image/emacsos.img']);
    report.boots.push({seconds:(Date.now()-begin)/1000,imageSha256:sha256,storage});
}
async function evaluate(page,form,marker) {
    const start=await page.evaluate(()=>window.__serial.length);
    await page.click('#eval-key');
    await page.keyboard.type(form,{delay:1});
    await page.keyboard.press('Enter');
    await page.waitForFunction(({start,marker})=>window.__serial.slice(start).includes(marker),{start,marker},{timeout:45000});
}
async function metadata(page) {
    return page.evaluate(async()=>await new Promise((resolve,reject)=>{
        const open=indexedDB.open('emacsos-data',1);
        open.onerror=()=>reject(open.error);
        open.onsuccess=()=>{
            const db=open.result;const tx=db.transaction('disks');const get=tx.objectStore('disks').get('default');
            tx.oncomplete=()=>{const record=get.result;db.close();resolve(record?{sha256:record.sha256,generation:record.generation,session:record.session,savedAt:record.savedAt}:null);};
            tx.onabort=()=>{db.close();reject(tx.error);};
        };
    }));
}
async function waitCheckpoint(page,previous) {
    const deadline=Date.now()+30000;
    while(Date.now()<deadline){
        const next=await metadata(page);
        if(next&&next.sha256!==previous.sha256)return next;
        await new Promise(resolve=>setTimeout(resolve,500));
    }
    throw new Error('Automatic DATA checkpoint did not retain new bytes');
}
async function verifySource(page) {
    await evaluate(page,'(progn (when (boundp \'browser-retained-number) (error "Unexpected automatic Lisp execution")) (load-file "/work/browser-retained.el") (unless (and (= (logand (file-modes "/work/browser-retained.el") 511) 416) (equal (file-symlink-p "/work/browser-dir/source.el") "../browser-retained.el")) (error "Transferred metadata differs")) (unless (and (with-temp-buffer (insert-file-contents "/home/user/browser-note.txt") (equal (buffer-string) "persistent home")) (with-temp-buffer (insert-file-contents "/tmp/browser-temp.txt") (equal (buffer-string) "ordinary temporary directory"))) (error "DATA directories did not survive")) (message "DISK-RESTORED-%d" browser-retained-number))','DISK-RESTORED-73');
}
(async()=>{
    const browser=await chromium.launch({headless:true,
        ...(process.env.EMACSOS_BROWSER?{executablePath:process.env.EMACSOS_BROWSER}:{}),args:['--no-sandbox']});
    try{
        const sourceContext=await browser.newContext({acceptDownloads:true});
        let page=await pageFor(sourceContext);
        report.build=await(await page.request.get(new URL('build.json',url).href)).json();
        report.assets=await(await page.request.get(new URL('assets.json',url).href)).json();
        await boot(page);
        report.templateSha256=await page.evaluate(()=>window.__templateHash);
        const initial=await metadata(page);
        await evaluate(page,'(progn (find-file "/work/browser-retained.el") (insert "(setq browser-retained-number 73)\\n") (save-buffer) (kill-buffer) (set-file-modes "/work/browser-retained.el" 416) (make-directory "/work/browser-dir") (make-symbolic-link "../browser-retained.el" "/work/browser-dir/source.el") (with-temp-file "/home/user/browser-note.txt" (insert "persistent home")) (with-temp-file "/tmp/browser-temp.txt" (insert "ordinary temporary directory")) (message (concat "DISK-" "CREATED")))','DISK-CREATED');
        report.automaticCheckpoint=await waitCheckpoint(page,initial);
        log('Automatic first DATA disk retains ordinary saved source and metadata without a manual mount or checkpoint');
        const contender=await pageFor(sourceContext);
        await contender.click('#boot');await state(contender,'failed');
        assert.match(await contender.locator('#error').innerText(),/already open in another tab/);
        await contender.close();current=page;
        log('A second tab is refused before it can run a competing retained-disk writer');
        await page.close();
        page=await pageFor(sourceContext);await boot(page);await verifySource(page);
        log('Closing and reopening restores saved source with modes/symlinks and no automatic Lisp execution');
        const downloadReady=page.waitForEvent('download');
        await page.click('#disk-export');const download=await downloadReady;
        const exported=path.join(output,'emacsos-data.img');await download.saveAs(exported);
        assert.equal(fs.statSync(exported).size,64*1024*1024);
        report.exportSha256=hash(fs.readFileSync(exported));
        report.importSha256=report.exportSha256;
        assert.equal((await metadata(page)).sha256,report.exportSha256);
        await page.screenshot({path:path.join(output,'retained.png'),fullPage:true});
        await sourceContext.close();
        log('EXPORT downloads a fresh 64 MiB frozen copy matching its completed browser checkpoint');

        const importedContext=await browser.newContext({acceptDownloads:true});
        page=await pageFor(importedContext);
        await page.locator('#disk-file').setInputFiles(exported);
        await page.waitForFunction(()=>document.getElementById('data-status').textContent.startsWith('Import staged.'));
        await boot(page);await verifySource(page);
        log('A fresh browser context imports the portable image and reads its saved source/metadata');
        await page.click('#disk-save');
        await page.waitForFunction(()=>document.documentElement.dataset.storageState==='saved'&&!document.getElementById('disk-save').disabled);
        const retained=await metadata(page);
        await page.close();
        page=await pageFor(importedContext);
        const corrupt=path.join(output,'corrupt-data.img');fs.writeFileSync(corrupt,Buffer.alloc(64*1024*1024));
        report.corruptImportSha256=hash(fs.readFileSync(corrupt));
        await page.locator('#disk-file').setInputFiles(corrupt);
        await page.waitForFunction(()=>document.getElementById('data-status').textContent.startsWith('Import staged.'));
        await boot(page,'error');
        assert.match(await page.locator('#data-status').innerText(),/mount rejected/);
        assert.equal((await metadata(page)).sha256,retained.sha256);
        assert(await page.locator('#disk-export').isDisabled());
        await evaluate(page,'(progn (dolist (directory (list "/work/" "/home/user/" "/tmp/")) (with-temp-buffer (insert "unsaved recovery content") (let (observed) (condition-case condition (write-region (point-min) (point-max) (concat directory "must-not-be-volatile.txt")) (file-error (setq observed condition))) (unless (and observed (buffer-modified-p) (string-match-p "Read-only file system" (error-message-string observed))) (error "Recovery allowed a writable fallback"))))) (message (concat "DISK-" "READONLY-RECOVERY")))','DISK-READONLY-RECOVERY');
        await page.screenshot({path:path.join(output,'rejected-import.png'),fullPage:true});
        await page.close();
        page=await pageFor(importedContext);await boot(page);await verifySource(page);
        await importedContext.close();
        log('A corrupt import enters recovery without formatting or replacing the previously retained disk');

        const sessionContext=await browser.newContext({acceptDownloads:true});
        page=await sessionContext.newPage();current=page;await instrumentation(page);
        await page.addInitScript(()=>Object.defineProperty(window,'indexedDB',{get(){throw new DOMException('Storage disabled for test','SecurityError');}}));
        await page.goto(url);await boot(page,'session-only');
        await evaluate(page,'(progn (with-temp-file "/work/session-only.txt" (insert "export still works")) (message (concat "SESSION-" "WROTE")))','SESSION-WROTE');
        const sessionDownload=page.waitForEvent('download');await page.click('#disk-export');
        await(await sessionDownload).saveAs(path.join(output,'session-only-data.img'));
        page.once('dialog',dialog=>dialog.accept());await page.click('#reset');
        await page.waitForFunction(()=>document.getElementById('data-status').textContent.includes('retention is unavailable'));
        await state(page,'running');
        assert(await page.locator('#disk-export').isEnabled());
        await sessionContext.close();
        log('Unavailable IndexedDB remains session-only with usable export and refuses a silent destructive restart');
        assert.deepEqual(report.errors,[]);report.passed=true;
    }catch(error){
        report.failure=String(error);
        if(current&&!current.isClosed()){
            report.serialTail=await current.evaluate(()=>(window.__serial||'').slice(-8000)).catch(()=>null);
            await current.screenshot({path:path.join(output,'failure.png'),fullPage:true}).catch(()=>{});
        }
        throw error;
    }finally{
        fs.writeFileSync(path.join(output,'report.json'),JSON.stringify(report,null,2)+'\n');
        await browser.close();
    }
})().catch(error=>{console.error(error);process.exitCode=1;});
