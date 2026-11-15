// Real guest network traffic through the local web server's automatic bridge.
const { chromium } = require(process.env.EMACSOS_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const url = process.argv[2] || 'http://127.0.0.1:18082/';
const output = path.resolve(process.env.EMACSOS_WEB_REPORT || 'build/web-network');
const payloadPrefix = `browser-network-${Date.now()}-`;
const payload = payloadPrefix + 'x'.repeat(2048);
const guestPayload = `(concat ${JSON.stringify(payloadPrefix)} (make-string 2048 ?x))`;
const checks = [], errors = [], sockets = new Set();
const report = { passed: false, url, checks, errors,
    scope: 'Actual pinned QEMU/Wasm guest TCP to a local numeric-address fixture, file save, completed DATA checkpoint and restart. No DNS or TLS claim.' };
fs.mkdirSync(output, { recursive: true });
let browser, page;
let received = Buffer.alloc(0);
const fixture = net.createServer(socket => {
    sockets.add(socket);
    socket.on('error', () => {});
    socket.on('close', () => sockets.delete(socket));
    socket.on('data', bytes => {
        received = Buffer.concat([received, bytes]);
        if (received.length >= Buffer.byteLength(payload)) {
            socket.write(received.subarray(0, 13));
            setTimeout(() => { if (!socket.destroyed) socket.end(received.subarray(13)); }, 500);
        }
    });
});

async function main() {
    await new Promise((resolve, reject) => { fixture.once('error', reject); fixture.listen(0, '127.0.0.1', resolve); });
    const port = fixture.address().port;
    report.fixturePort = port;
    report.webSource = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
    report.webSourceDirty = Boolean(execFileSync('git', ['status', '--porcelain'], { encoding: 'utf8' }).trim());
    assert.equal(report.webSourceDirty, false, 'Browser acceptance requires a clean source checkout');
    browser = await chromium.launch({ headless: true,
        ...(process.env.EMACSOS_BROWSER ? { executablePath: process.env.EMACSOS_BROWSER } : {}),
        args: ['--no-sandbox'] });
    page = await browser.newPage({ viewport: { width: 1440, height: 1100 } });
    page.on('pageerror', error => errors.push(String(error)));
    await page.addInitScript(() => {
        const fetch = window.fetch.bind(window);
        window.fetch = async (...args) => {
            const response = await fetch(...args);
            if (new URL(response.url).pathname.endsWith('/emacsos.img') && response.ok) {
                window.__imageHash = response.clone().arrayBuffer().then(async bytes => {
                    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
                    return Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('');
                });
            }
            return response;
        };
    });
    await page.route('**/vendor/xterm.js', async route => {
        const response = await route.fetch();
        await route.fulfill({ response, body: await response.text() + `
window.__serial = '';
window.Terminal = new Proxy(window.Terminal, { construct(Target, args) {
    const terminal = Reflect.construct(Target, args), write = terminal.write.bind(terminal), decoder = new TextDecoder();
    terminal.write = (bytes, ...rest) => {
        window.__serial += typeof bytes === 'string' ? bytes : decoder.decode(bytes, {stream:true});
        return write(bytes, ...rest);
    };
    return terminal;
}});` });
    });
    async function waitState(value) {
        await page.waitForFunction(value => document.documentElement.dataset.machineState === value, value, { timeout: 60000 });
    }
    async function evaluate(form, marker) {
        const start = await page.evaluate(() => window.__serial.length);
        await page.click('#eval-key');
        await page.keyboard.type(form, { delay: 1 });
        await page.keyboard.press('Enter');
        await page.waitForFunction(({ marker, start }) => window.__serial.slice(start).includes(marker),
            { marker, start }, { timeout: 45000 });
    }
    await page.goto(url);
    report.build = await page.evaluate(async () => (await fetch('./build.json')).json());
    assert.equal(report.build.dirty, false, 'The image must come from a clean build');
    assert.equal(report.build.parent, report.webSource, 'The build must match the tested source commit');
    await page.click('#boot');
    await waitState('running');
    await page.waitForFunction(() => document.documentElement.dataset.networkState === 'connected');
    report.imageSha256 = await page.evaluate(() => window.__imageHash);
    assert.equal(report.imageSha256, report.build.components['build/image/emacsos.img'],
        'The actual fetched SYSTEM bytes must match the build manifest');
    checks.push('Automatic same-origin network link and actual Emacs boot');
    // Timer progress occurs while the fixture intentionally delays the final bytes.
    const setup = `(progn (setq eos-net-payload ${guestPayload} eos-net-reply "" eos-net-ticks 0) (setq eos-net-timer (run-at-time 0 0.05 (lambda () (setq eos-net-ticks (1+ eos-net-ticks))))) (setq eos-net-process (make-network-process :name "browser-network" :host "10.0.2.2" :service ${port} :family 'ipv4 :nowait t :coding 'binary :noquery t :sentinel (lambda (p _e) (when (eq (process-status p) 'open) (process-send-string p (substring eos-net-payload 0 17)) (process-send-string p (substring eos-net-payload 17)))) :filter (lambda (p bytes) (setq eos-net-reply (concat eos-net-reply bytes)) (when (= (length eos-net-reply) (length eos-net-payload)) (cancel-timer eos-net-timer) (if (and (equal eos-net-reply eos-net-payload) (> eos-net-ticks 0)) (progn (with-temp-file "/home/user/projects/browser-network.txt" (insert eos-net-reply)) (message (concat "WEB-NET-ECHO-" "SAVE-PASS"))) (error "Network payload or timer mismatch")) (delete-process p))))) (message "WEB-NET-STARTED"))`;
    await evaluate(setup, 'WEB-NET-ECHO-SAVE-PASS');
    assert.equal(received.toString(), payload);
    checks.push('Guest nowait TCP, fragmented echo, timer progress and DATA save');
    await page.click('#disk-save');
    await page.waitForFunction(() => document.documentElement.dataset.storageState === 'saved', null, { timeout: 30000 });
    checks.push('Completed browser DATA checkpoint after network save');
    page.on('dialog', dialog => dialog.accept());
    await Promise.all([page.waitForNavigation({ waitUntil: 'domcontentloaded' }), page.click('#reset')]);
    await page.click('#boot');
    await waitState('running');
    await evaluate(`(message "WEB-NET-RETAINED:%s" (equal (with-temp-buffer (insert-file-contents "/home/user/projects/browser-network.txt") (buffer-string)) ${guestPayload}))`, 'WEB-NET-RETAINED:t');
    checks.push('Restart restores exact network-received file');
    assert.deepEqual(errors, []);
    fs.writeFileSync(path.join(output, 'serial.txt'), await page.evaluate(() => window.__serial));
    await page.screenshot({ path: path.join(output, 'network.png'), fullPage: true });
    report.passed = true;
}

main().catch(async error => {
    report.failure = String(error.stack || error);
    process.exitCode = 1;
    if (page && !page.isClosed()) {
        try { fs.writeFileSync(path.join(output, 'serial.txt'), await page.evaluate(() => window.__serial || '')); } catch {}
        try { await page.screenshot({ path: path.join(output, 'failure.png'), fullPage: true }); } catch {}
    }
})
    .finally(async () => {
        report.fixtureReceivedBytes = received.length;
        if (browser) await browser.close();
        for (const socket of sockets) socket.destroy();
        await new Promise(resolve => fixture.close(resolve));
        fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
        console.log(JSON.stringify(report));
    });
