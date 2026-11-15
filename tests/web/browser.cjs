// Real-browser checks against a fresh browser-owned machine, never a shared guest.
const { chromium } = require(process.env.EMACSOS_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const url = process.argv[2] || 'http://127.0.0.1:18082/';
const output = path.resolve(process.env.EMACSOS_WEB_REPORT || 'build/web-browser');
fs.mkdirSync(output, { recursive: true });
const checks = [];
const errors = [];
const record = { passed: false, url, checks, errors,
    scope: 'Browser presentation checks; Mac platform is simulated. This is not the full native MVP acceptance.' };
try {
    record.webSource = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
    record.webSourceDirty = Boolean(execFileSync('git', ['status', '--porcelain'], { encoding: 'utf8' }).trim());
} catch { record.webSource = 'unavailable'; }

async function main() {
    const browser = await chromium.launch({ headless: true,
        ...(process.env.EMACSOS_BROWSER ? { executablePath: process.env.EMACSOS_BROWSER } : {}),
        args: ['--no-sandbox'] });
    const page = await browser.newPage({ viewport: { width: 1440, height: 1100 } });
    page.on('pageerror', error => errors.push(String(error)));
    await page.addInitScript(() => Object.defineProperty(navigator, 'platform', { get: () => 'MacIntel' }));
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
    // Capture actual serial writes without a production debug API or fake guest.
    await page.route('**/vendor/xterm.js', async route => {
        const response = await route.fetch();
        const source = await response.text();
        await route.fulfill({ response, body: source + `
window.__serial = '';
window.Terminal = new Proxy(window.Terminal, { construct(Target, args) {
    const terminal = Reflect.construct(Target, args);
    const write = terminal.write.bind(terminal);
    const decoder = new TextDecoder();
    terminal.write = (bytes, ...rest) => {
        window.__serial += typeof bytes === 'string' ? bytes : decoder.decode(bytes, {stream:true});
        return write(bytes, ...rest);
    };
    return terminal;
}});` });
    });
    let firstImage = true;
    await page.route('**/emacsos.img', async route => {
        if (firstImage) {
            firstImage = false;
            await route.fulfill({ status: 404, body: 'Missing test image' });
            return;
        }
        // Let the large image stream normally; passing it through the DevTools
        // fulfillment channel can exceed Chromium's protocol message limit.
        await route.continue();
    });
    const serialLength = () => page.evaluate(() => window.__serial?.length || 0);
    async function waitSerial(marker, start = 0, timeout = 30000) {
        await page.waitForFunction(({ marker, start }) => (window.__serial || '').slice(start).includes(marker),
            { marker, start }, { timeout });
    }
    async function evaluate(form, marker) {
        const start = await serialLength();
        await page.click('#eval-key');
        await page.keyboard.type(form, { delay: 2 });
        await page.keyboard.press('Enter');
        await waitSerial(marker, start);
    }
    async function state(value, target = page) {
        await target.waitForFunction(value => document.documentElement.dataset.machineState === value, value,
            { timeout: 120000 });
    }
    const check = message => { checks.push(message); console.log('PASS:', message); };
    try {
        const response = await page.goto(url);
        assert.equal(response.headers()['cross-origin-opener-policy'], 'same-origin');
        assert.equal(response.headers()['cross-origin-embedder-policy'], 'require-corp');
        assert(await page.evaluate(() => isSecureContext && crossOriginIsolated));
        record.guestBuild = await (await page.request.get(new URL('build.json', url).href)).json();
        record.assets = await (await page.request.get(new URL('assets.json', url).href)).json();
        await state('off');
        assert(await page.locator('#reset').isDisabled());
        assert(await page.locator('#command-key').isDisabled());
        assert.equal(await page.locator('input[type=file]').count(), 1);
        assert(await page.locator('#disk-file').isHidden());
        assert(await page.locator('#disk-import').isEnabled());
        assert(await page.locator('#disk-export').isDisabled());
        check('Isolated browser, initial controls and staged DATA import');
        for (const width of [1440, 1100, 1024, 768, 390]) {
            await page.setViewportSize({ width, height: 1100 });
            assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `Page overflow at ${width}`);
        }
        await page.screenshot({ path: path.join(output, 'mobile.png'), fullPage: true });
        await page.setViewportSize({ width: 1440, height: 1100 });
        await page.screenshot({ path: path.join(output, 'off.png'), fullPage: true });
        check('Responsive outer page at five desktop/tablet/mobile widths');

        await page.click('#boot');
        await state('failed');
        assert(await page.locator('#error').isVisible());
        await page.waitForFunction(() => !document.getElementById('boot').disabled);
        assert(await page.locator('#boot svg').count());
        check('Missing boot image is visible and permits a pre-initialization retry');
        const begin = Date.now();
        await page.click('#boot');
        await state('running');
        record.bootSeconds = (Date.now() - begin) / 1000;
        record.imageSha256 = await page.evaluate(() => window.__imageHash);
        assert.equal(record.imageSha256, record.guestBuild.components['build/image/emacsos.img']);
        assert(await page.locator('#boot').isDisabled());
        assert(await page.locator('#command-key').isEnabled());
        check('Current manifest-matched system boots in QEMU/Wasm');
        await page.waitForFunction(() => document.documentElement.dataset.storageState === 'saved', null, {timeout:60000});
        check('Guest-validated DATA receives an initial local checkpoint');
        await evaluate('(progn (unless (and (equal (getenv "HOME") "/home/user") (equal (expand-file-name "~") "/home/user") (equal default-directory "/home/user/") (file-directory-p "~/projects") (not (file-exists-p "/work"))) (error "Unexpected home or legacy workspace")) (message (concat "HOME-" "READY")))', 'HOME-READY');
        check('Default HOME and initial directory use /home/user with a projects directory and no /work alias');
        await page.waitForTimeout(250);
        await page.screenshot({ path: path.join(output, 'running.png'), fullPage: true });

        await page.locator('.xterm-screen').click({ position: { x: 100, y: 100 } });
        let start = await serialLength();
        await page.keyboard.press('Alt+x');
        await page.keyboard.type('emacs-version', { delay: 5 });
        await page.keyboard.press('Enter');
        await waitSerial('GNU Emacs 30.2', start);
        check('Mac Option-x invokes M-x in the actual guest (simulated Mac platform)');
        start = await serialLength();
        await page.click('#command-key');
        await waitSerial('M-x', start);
        await page.click('#cancel-key');
        await waitSerial('Quit', start);
        check('COMMAND and CANCEL buttons send real Emacs keys and restore focus');

        await evaluate('(progn (switch-to-buffer "*Browser keyboard test*") (erase-buffer) (message (concat "KEY-" "READY")))', 'KEY-READY');
        await page.keyboard.type('abc');
        await page.keyboard.press('Control+a');
        await page.keyboard.type('Z');
        await evaluate('(message "BROWSER-LINE-%s" (buffer-string))', 'BROWSER-LINE-Zabc');
        check('Ctrl-a reaches beginning-of-line without emulator/host interception');
        await evaluate('(progn (erase-buffer) (insert "(+ 20 22)") (message (concat "EVAL-" "READY")))', 'EVAL-READY');
        start = await serialLength();
        await page.keyboard.press('Control+x');
        await page.keyboard.press('Control+e');
        await waitSerial('42', start);
        check('Ctrl-x Ctrl-e evaluates the expression before point');

        await page.keyboard.press('Control+x');
        await page.keyboard.press('Control+f');
        await page.keyboard.type('/home/user/projects/browser-proof.el', { delay: 3 });
        await page.keyboard.press('Enter');
        await page.waitForTimeout(300);
        await page.keyboard.type('(setq browser-proof 42)\n', { delay: 3 });
        await page.keyboard.press('Control+x');
        await page.keyboard.press('Control+s');
        await evaluate('(progn (kill-buffer) (load-file "/home/user/projects/browser-proof.el") (message "BROWSER-SAVED-%d" browser-proof))', 'BROWSER-SAVED-42');
        check('Ctrl-x Ctrl-f / Ctrl-x Ctrl-s save source that the guest loads');
        await evaluate('(progn (require \'emacsos-counter) (emacsos-counter-start) (emacsos-counter-start) (message "BROWSER-RUNS-%d" (length (emacsos-app-list))))', 'BROWSER-RUNS-2');
        start = await serialLength();
        await page.click('#command-key');
        await page.keyboard.type('emacsos-processes', { delay: 5 });
        await page.keyboard.press('Enter');
        await waitSerial('*Processes*', start);
        await evaluate('(message "MANAGER-NAME-%s" (buffer-name))', 'MANAGER-NAME-*Processes*');
        await page.waitForTimeout(250);
        await page.screenshot({ path: path.join(output, 'applications.png'), fullPage: true });
        check('Bundled application manager opens in the browser guest');

        page.once('dialog', dialog => dialog.dismiss());
        await page.click('#reset');
        await state('running');
        await evaluate('(message "RESTART-CANCELED-%d" browser-proof)', 'RESTART-CANCELED-42');
        check('Canceling RESTART preserves the running machine and its RAM');
        await page.click('#fullscreen');
        await page.waitForFunction(() => document.fullscreenElement?.id === 'monitor');
        await page.evaluate(() => document.exitFullscreen());
        check('Fullscreen targets the monitor');
        await page.click('#eval-key');
        await page.keyboard.type('(kill-emacs 0)', { delay: 3 });
        await page.keyboard.press('Enter');
        await state('exited');
        assert(await page.locator('#command-key').isDisabled());
        check('Guest exit is reported and disables input');
        page.once('dialog', dialog => dialog.accept());
        await page.click('#reset');
        await state('off');
        assert.equal(await serialLength(), 0);
        check('Confirmed RESTART retains DATA before reloading a fresh powered-off page');
        await page.click('#boot');
        await state('running');
        await page.waitForFunction(() => document.documentElement.dataset.storageState === 'saved', null, {timeout:60000});
        await evaluate('(progn (when (boundp \'browser-proof) (error "Imported Lisp ran automatically")) (load-file "/home/user/projects/browser-proof.el") (message "BROWSER-RESTORED-%d" browser-proof))', 'BROWSER-RESTORED-42');
        check('Restart restores saved source without automatically executing it');
        page.once('dialog', dialog => dialog.accept());
        await page.click('#disk-discard');
        await state('off');
        assert.deepEqual(errors, []);

        const blocked = await browser.newPage();
        await blocked.addInitScript(() => Object.defineProperty(window, 'crossOriginIsolated', { value: false }));
        await blocked.goto(url);
        await blocked.click('#boot');
        await state('failed', blocked);
        assert.match(await blocked.locator('#error').innerText(), /HTTPS or localhost/);
        assert(await blocked.locator('#boot').isEnabled());
        await blocked.close();
        check('Unsuitable browser context fails visibly before VM initialization');
        record.passed = true;
    } catch (error) {
        record.failure = String(error);
        record.serialTail = await page.evaluate(() => (window.__serial || '').slice(-6000)).catch(() => 'unavailable');
        await page.screenshot({ path: path.join(output, 'failure.png'), fullPage: true }).catch(() => {});
        throw error;
    } finally {
        fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(record, null, 2) + '\n');
        await browser.close();
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
