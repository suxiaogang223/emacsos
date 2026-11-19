// Accept the lightweight showcase independently of booting the computer.
const { chromium } = require(process.env.EMACSOS_PLAYWRIGHT || 'playwright');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { execFileSync } = require('node:child_process');

const url = new URL(process.argv[2] || 'http://127.0.0.1:18082/');
const output = path.resolve(process.env.EMACSOS_SITE_REPORT || 'build/site-browser');
fs.mkdirSync(output, { recursive: true });
const record = { passed: false, url: url.href, checks: [], errors: [],
    webSource: execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(),
    webSourceDirty: Boolean(execFileSync('git', ['status', '--porcelain'], { encoding: 'utf8' }).trim()) };

async function main() {
    const browser = await chromium.launch({ headless: true,
        ...(process.env.EMACSOS_BROWSER ? { executablePath: process.env.EMACSOS_BROWSER } : {}),
        args: ['--no-sandbox'] });
    const page = await browser.newPage({ viewport: { width: 1440, height: 1100 } });
    const requests = [];
    page.on('pageerror', error => record.errors.push(String(error)));
    page.on('request', request => requests.push(new URL(request.url()).pathname));
    page.on('response', response => {
        if (response.status() >= 400) record.errors.push(`${response.status()} ${response.url()}`);
    });
    const check = message => { record.checks.push(message); console.log('PASS:', message); };
    try {
        assert.equal((await page.goto(url.href)).status(), 200);
        assert.equal(await page.locator('h1').innerText(), 'Your machine.\nYour Lisp.');
        await page.locator('img').evaluateAll(images => Promise.all(images.map(image => {
            image.loading = 'eager';
            return image.decode();
        })));
        assert.equal(await page.locator('img').count(), 6);
        assert(await page.locator('img').evaluateAll(images => images.every(image => image.naturalWidth > 0 && image.alt.length > 20)));
        assert(!requests.some(name => /\.(?:wasm|ext4|squashfs)$|\/vmlinuz$|\/vendor\/|\/machine\.js|\/coi-serviceworker\.js/.test(name)));
        check('All six real screenshots load without emulator, guest images or isolation scripts');
        for (const href of await page.locator('a').evaluateAll(links => links.map(link => link.getAttribute('href')))) {
            const destination = new URL(href, url);
            if (destination.origin !== url.origin) continue;
            assert(destination.pathname.startsWith(url.pathname), `Link escaped project path: ${href}`);
            if (destination.hash) {
                assert(await page.locator(destination.hash).count(), `Missing anchor: ${href}`);
            } else {
                assert.equal((await page.request.get(destination.href)).status(), 200, href);
            }
        }
        check('Local navigation and screenshot paths resolve inside the hosting project subpath');
        for (const width of [1440, 1100, 1024, 768, 390]) {
            await page.setViewportSize({ width, height: 1100 });
            assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), `Overflow at ${width}`);
            assert(await page.getByRole('link', { name: 'Start emacsOS', exact: true }).first().isVisible());
        }
        await page.screenshot({ path: path.join(output, 'mobile.png'), fullPage: true });
        await page.setViewportSize({ width: 1440, height: 1100 });
        await page.screenshot({ path: path.join(output, 'desktop.png'), fullPage: true });
        check('Readable layout and start links at five desktop/tablet/mobile widths');
        await page.keyboard.press('Tab');
        assert.equal(await page.locator(':focus').innerText(), 'Skip to content');
        assert.match(await page.locator('.edition-notes').innerText(), /restarting discards your files/);
        assert.match(await page.locator('.edition-notes').innerText(), /networking is currently disabled/);
        assert.deepEqual(record.errors, []);
        check('Keyboard skip link and visible browser storage/network limitations');
        record.passed = true;
    } catch (error) {
        record.failure = String(error);
        await page.screenshot({ path: path.join(output, 'failure.png'), fullPage: true }).catch(() => {});
        throw error;
    } finally {
        fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(record, null, 2) + '\n');
        await browser.close();
    }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
