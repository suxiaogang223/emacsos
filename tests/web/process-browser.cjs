// Run the real ring-3 probe kernel in the pinned browser emulator.
const { chromium } = require(process.env.EMACSOS_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const url = process.argv[2] || 'http://127.0.0.1:18082/';
const output = path.resolve(process.env.EMACSOS_WEB_REPORT || 'build/web-process');
fs.mkdirSync(output, { recursive: true });
const disk = fs.readFileSync(path.join(root, 'build/process-probe/system.img'));
const native = JSON.parse(fs.readFileSync(path.join(root, 'build/process-probe/results.json')));
const report = { passed: false, checks: [], url,
    scope: 'Actual ring-3 ELF probe kernel in pinned QEMU/Wasm; substituted probe SYSTEM, not Emacs application API or scheduling acceptance.' };
let browser, page;
async function main() {
    report.parent = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim();
    report.dirty = Boolean(execFileSync('git', ['status', '--porcelain'], { cwd: root, encoding: 'utf8' }).trim());
    assert.equal(report.dirty, false, 'Process browser acceptance requires a clean checkout');
    assert.equal(native.parent, report.parent);
    assert.equal(native.dirty, false);
    assert.equal(native.passed, true);
    report.imageSha256 = crypto.createHash('sha256').update(disk).digest('hex');
    assert.equal(report.imageSha256, native.image_sha256);
    report.workers = native.workers;
    report.assets = JSON.parse(fs.readFileSync(path.join(root, 'web/assets.json')));
    browser = await chromium.launch({ headless: true,
        ...(process.env.EMACSOS_BROWSER ? { executablePath: process.env.EMACSOS_BROWSER } : {}),
        args: ['--no-sandbox'] });
    page = await browser.newPage();
    await page.addInitScript(() => {
        const fetch = window.fetch.bind(window);
        window.fetch = async (...args) => {
            const response = await fetch(...args);
            if (new URL(response.url).pathname.endsWith('/emacsos.img') && response.ok) {
                window.__probeHash = response.clone().arrayBuffer().then(async bytes => {
                    const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
                    return Array.from(hash, n => n.toString(16).padStart(2, '0')).join('');
                });
            }
            return response;
        };
    });
    await page.route('**/emacsos.img', route => route.fulfill({ status: 200,
        contentType: 'application/octet-stream', body: disk }));
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
    await page.goto(url);
    await page.click('#boot');
    await page.waitForFunction(() => window.__serial?.includes('PROCESS-PROBE-PASS') ||
        window.__serial?.includes('PANIC:'), null, { timeout: 120000 });
    const serial = await page.evaluate(() => window.__serial);
    fs.writeFileSync(path.join(output, 'serial.log'), serial);
    assert(!serial.includes('PANIC:'), serial);
    assert(serial.includes('PROCESS-RING3-CONTEXT-COPY-PASS'));
    report.checks.push('Real ring-3 SYSCALL/int80, private mappings, TLS/FPU/SSE and full user-copy checks');
    assert(serial.includes('PROCESS-FAULT-BOUNDARY-PASS'));
    report.checks.push('User faults, invalid returns and privilege attempts leave the manager alive');
    assert(serial.includes('PROCESS-LIFECYCLE-ROLLBACK-PASS'));
    report.checks.push('PID generations, quotas, allocation rollback, bounded output and reclamation');
    assert.equal(await page.evaluate(() => window.__probeHash), report.imageSha256);
    report.checks.push('Actual fetched probe bytes match the native probe identity');
    await page.screenshot({ path: path.join(output, 'process.png'), fullPage: true });
    report.passed = true;
}
main().catch(async error => {
    report.error = String(error.stack || error);
    if (page && !page.isClosed()) {
        try { fs.writeFileSync(path.join(output, 'serial.log'), await page.evaluate(() => window.__serial || '')); }
        catch { /* The recorded timeout/error remains authoritative. */ }
    }
    process.exitCode = 1;
}).finally(async () => {
    if (browser) await browser.close();
    fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
    console.log(JSON.stringify({ passed: report.passed, checks: report.checks, error: report.error }));
});
