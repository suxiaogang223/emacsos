// Real-browser checks against a fresh browser-owned machine, never a shared guest.
const { chromium } = require(process.env.EMACSOS_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const hosted = process.env.EMACSOS_HOSTED === '1';
const url = process.argv[2] || 'http://127.0.0.1:18082/';
const output = path.resolve(process.env.EMACSOS_WEB_REPORT || 'build/web-browser');
fs.mkdirSync(output, { recursive: true });
const checks = [];
const errors = [];
const record = { passed: false, url, checks, errors,
    scope: 'Browser presentation checks; Mac platform is simulated. Native persistence and networking require their own acceptance.' };
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
            if (new URL(response.url).pathname.match(/\/(vmlinuz|initramfs\.cpio\.gz|runtime\.squashfs|data-template\.ext4)$/) && response.ok) {
                window.__imageHashes ??= {};
                const hash = response.clone().arrayBuffer().then(async bytes => {
                    const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
                    return Array.from(digest, byte => byte.toString(16).padStart(2, '0')).join('');
                });
                // Failed-boot retries cancel sibling downloads. Observe clone
                // rejections now; awaiting the retained promise still rejects.
                hash.catch(() => {});
                window.__imageHashes[new URL(response.url).pathname.split('/').pop()] = hash;
            }
            return response;
        };
    });
    // Install before xterm loads, including on service-worker navigations.
    // Hosted requests pass through the worker and cannot use page.route().
    await page.addInitScript(() => {
        window.__serial = '';
        let exposed;
        Object.defineProperty(window, 'Terminal', {
            configurable: true,
            get: () => exposed,
            set: Target => {
                exposed = new Proxy(Target, { construct(Constructor, args) {
                    const terminal = Reflect.construct(Constructor, args);
                    const write = terminal.write.bind(terminal);
                    const decoder = new TextDecoder();
                    terminal.write = (bytes, ...rest) => {
                        window.__serial += typeof bytes === 'string' ? bytes : decoder.decode(bytes, { stream: true });
                        return write(bytes, ...rest);
                    };
                    return terminal;
                }});
            },
        });
    });
    let firstImage = true;
    if (!hosted) await page.route('**/vmlinuz*', async route => {
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
        assert(!form.includes(marker), "Result marker must be absent from keyboard input");
        const start = await serialLength();
        await page.click('#eval-key');
        await page.keyboard.type(form, { delay: 2 });
        await page.keyboard.press('Enter');
        await waitSerial(marker, start);
    }
    async function state(value, target = page) {
        await target.waitForFunction(value => document.documentElement.dataset.machineState === value ||
            (value !== 'failed' && document.documentElement.dataset.machineState === 'failed'), value,
            { timeout: 300000 });
        const actual = await target.locator('html').getAttribute('data-machine-state');
        assert.equal(actual, value, await target.locator('#error').innerText());
    }
    const check = message => { checks.push(message); console.log('PASS:', message); };
    try {
        let response = await page.goto(url);
        if (hosted) {
            await page.waitForFunction(() => crossOriginIsolated, null, { timeout: 30000 });
            response = await page.reload();
        }
        assert.equal(response.headers()['cross-origin-opener-policy'], 'same-origin');
        assert.equal(response.headers()['cross-origin-embedder-policy'], 'require-corp');
        assert(await page.evaluate(() => isSecureContext && crossOriginIsolated));
        record.guestBuild = await (await page.request.get(new URL('build.json', url).href)).json();
        record.assets = await (await page.request.get(new URL('assets.json', url).href)).json();
        await state('off');
        assert(await page.locator('#reset').isDisabled());
        assert(await page.locator('#command-key').isDisabled());
        assert.equal(await page.locator('input[type=file]').count(), 0);
        assert.match(await page.locator('[data-drive=data]').innerText(), /THIS SESSION/);
        check('Isolated browser, initial controls and session disk notice');
        for (const width of [1440, 1100, 1024, 768, 390]) {
            await page.setViewportSize({ width, height: 1100 });
            assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `Page overflow at ${width}`);
        }
        await page.screenshot({ path: path.join(output, 'mobile.png'), fullPage: true });
        await page.setViewportSize({ width: 1440, height: 1100 });
        await page.screenshot({ path: path.join(output, 'off.png'), fullPage: true });
        check('Responsive outer page at five desktop/tablet/mobile widths');

        if (!hosted) {
        await page.click('#boot');
        await state('failed');
        assert(await page.locator('#error').isVisible());
        assert(await page.locator('#boot').isEnabled());
        assert(await page.locator('#boot svg').count());
        check('Missing boot image is visible and permits a pre-initialization retry');
        await page.evaluate(async () => {
            await caches.delete('emacsos-boot-v1');
            window.__imageHashes = {};
        });
        }
        const begin = Date.now();
        await page.click('#boot');
        await state('running');
        await waitSerial('Your working directory is ~/work.');
        record.bootSeconds = (Date.now() - begin) / 1000;
        console.log('BOOT:', record.bootSeconds.toFixed(3), 'seconds');
        record.artifactHashes = await page.evaluate(async () => Object.fromEntries(await Promise.all(Object.entries(window.__imageHashes).map(async ([name, value]) => [name, await value]))));
        for (const [name, hash] of Object.entries(record.artifactHashes)) assert.equal(hash, record.guestBuild.artifacts[name]);
        assert.equal(Object.keys(record.artifactHashes).length, 4);
        assert(await page.locator('#boot').isDisabled());
        assert(await page.locator('#command-key').isEnabled());
        check('Manifest-matched Linux and pinned Emacs boot in QEMU/Wasm');
        await page.waitForTimeout(250);
        await page.screenshot({ path: path.join(output, 'running.png'), fullPage: true });

        await evaluate('(message "PRELOAD-%s" (if (and (featurep \'dired) (featurep \'info) (featurep \'esh-mode) (string-prefix-p "/home/emacs/" (expand-file-name eshell-directory-name))) "READY" "MISSING"))', 'PRELOAD-READY');
        check('Quick-action libraries are preloaded with the guest user home');

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

        await evaluate('(progn (require \'dired) (require \'find-func) (message "SOURCE-%s" (if (string-suffix-p ".el" (buffer-file-name (car (find-function-noselect \'dired)))) "AVAILABLE" "MISSING")))', 'SOURCE-AVAILABLE');
        check('Lisp source opens through find-function in the browser guest');
        const infoBegin = Date.now();
        await evaluate('(progn (info "(emacs)Top") (message "MANUAL-%s" (if (and (eq major-mode \'Info-mode) (equal Info-current-node "Top")) "AVAILABLE" "MISSING")))', 'MANUAL-AVAILABLE');
        record.firstInfoSeconds = (Date.now() - infoBegin) / 1000;
        check('Emacs manual opens in Info in the browser guest');
        await page.keyboard.press('q');
        await evaluate('(message "HELP-%s" (if (eq major-mode \'Info-mode) "OPEN" "CLOSED"))', 'HELP-CLOSED');

        await page.keyboard.press('Control+x');
        await page.keyboard.press('Control+f');
        await page.keyboard.type('/home/emacs/work/browser-proof.el', { delay: 3 });
        await page.keyboard.press('Enter');
        await page.waitForTimeout(300);
        await page.keyboard.type('(setq browser-proof 42)\n', { delay: 3 });
        await page.keyboard.press('Control+x');
        await page.keyboard.press('Control+s');
        await evaluate('(progn (kill-buffer) (load-file "/home/emacs/work/browser-proof.el") (message "BROWSER-SAVED-%d" browser-proof))', 'BROWSER-SAVED-42');
        check('Ctrl-x Ctrl-f / Ctrl-x Ctrl-s save source that the guest loads');
        start = await serialLength();
        const shellBegin = Date.now();
        await page.click('#shell-key');
        // First startup loads built-in modules before accepting commands.
        await waitSerial('~/work $ ', start, 120000);
        record.firstShellSeconds = (Date.now() - shellBegin) / 1000;
        await evaluate('(message "SHELL-MODE-%s" major-mode)', 'SHELL-MODE-eshell-mode');
        await page.keyboard.type('echo hello-from-linux', { delay: 3 });
        start = await serialLength();
        await page.keyboard.press('Enter');
        await waitSerial('hello-from-linux', start);
        check('SHELL opens a real Eshell session');
        start = await serialLength();
        await page.click('#files-key');
        await page.keyboard.press('Enter');
        await waitSerial('browser-proof.el', start);
        await evaluate('(message "FILES-MODE-%s" major-mode)', 'FILES-MODE-dired-mode');
        await page.waitForTimeout(250);
        await page.screenshot({ path: path.join(output, 'workspace.png'), fullPage: true });
        check('FILES opens the Linux working directory in Dired');

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
        check('Confirmed RESTART reloads a fresh powered-off page');

        const cached = await page.evaluate(async () => {
            const cache = await caches.open('emacsos-boot-v1');
            const keys = await cache.keys();
            return keys.map(key => new URL(key.url).pathname.split('/').pop()).sort();
        });
        assert.deepEqual(cached, ['data-template.ext4', 'initramfs.cpio.gz', 'runtime.squashfs', 'vmlinuz']);
        check('Boot cache contains only the four verified static images');
        await page.evaluate(async () => {
            const cache = await caches.open('emacsos-boot-v1');
            const key = (await cache.keys()).find(key => new URL(key.url).pathname.endsWith('/vmlinuz'));
            await cache.put(key, new Response('Corrupt cache fixture'));
        });
        const warmBegin = Date.now();
        await page.click('#boot');
        await state('running');
        await waitSerial('Your working directory is ~/work.');
        record.warmRestartSeconds = (Date.now() - warmBegin) / 1000;
        console.log('WARM RESTART:', record.warmRestartSeconds.toFixed(3), 'seconds');
        const refetched = await page.evaluate(async () => Object.fromEntries(await Promise.all(Object.entries(window.__imageHashes || {}).map(async ([name, value]) => [name, await value]))));
        assert.deepEqual(Object.keys(refetched), ['vmlinuz']);
        assert.equal(refetched.vmlinuz, record.guestBuild.artifacts.vmlinuz);
        await evaluate('(message "FRESH-%s" (if (file-exists-p "/home/emacs/work/browser-proof.el") "LEAKED" "EMPTY"))', 'FRESH-EMPTY');
        check('Warm boot reuses verified images, repairs corrupt cache and starts with a fresh home');
        assert.deepEqual(errors, []);

        if (!hosted) {
        const blocked = await browser.newPage();
        await blocked.addInitScript(() => {
            Object.defineProperty(window, 'crossOriginIsolated', { value: false });
            Object.defineProperty(navigator, 'serviceWorker', { value: undefined });
        });
        await blocked.goto(url);
        await blocked.click('#boot');
        await state('failed', blocked);
        assert.match(await blocked.locator('#error').innerText(), /HTTPS or localhost/);
        assert(await blocked.locator('#boot').isEnabled());
        await blocked.close();
        check('Unsuitable browser context fails visibly before VM initialization');
        } else {
            check('Hosted service-worker navigation and restart preserve browser isolation');
        }
        record.passed = true;
    } catch (error) {
        record.failure = String(error);
        record.serialTail = await page.evaluate(() => (window.__serial || '').slice(-6000)).catch(() => 'unavailable');
        await page.screenshot({ path: path.join(output, 'failure.png'), fullPage: true }).catch(() => {});
        throw error;
    } finally {
        const transcript = await page.evaluate(() => window.__serial || '').catch(() => '');
        fs.writeFileSync(path.join(output, 'guest.transcript'), transcript);
        fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(record, null, 2) + '\n');
        await browser.close();
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
