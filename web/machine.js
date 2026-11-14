// The browser hosts one real emulated machine; application state stays in Emacs.
const element = id => document.getElementById(id);
const buttons = ["command-key", "cancel-key", "eval-key"].map(element);
const vendor = new URL("./vendor/", import.meta.url);
let state = "off";
let initializationStarted = false;
let retryable = true;
let approvedReload = false;
let terminal;
let machine;
let slave;
let observed = "";
const decoder = new TextDecoder();

function render(next, message, error = "") {
    state = next;
    document.documentElement.dataset.machineState = state;
    element("status").textContent = message;
    element("error").textContent = error;
    element("error").hidden = !error;
    element("boot").disabled = !(state === "off" || (state === "failed" && retryable));
    element("reset").disabled = state === "off";
    for (const button of buttons) button.disabled = state !== "running";
    if (terminal) terminal.options.disableStdin = state !== "running";
    element("idle-screen").hidden = Boolean(terminal) || !["off", "failed"].includes(state);
    element("boot-progress").hidden = !["loading", "starting"].includes(state);
    if (state === "running") element("boot-progress").value = 100;
}

function fail(error) {
    const reason = error instanceof Error ? error.message : String(error);
    render("failed", initializationStarted ? "Machine error. Restart to try again." : "Power-on failed. You can retry.", reason);
    console.error(error);
}

function observe(bytes) {
    const text = typeof bytes === "string" ? bytes : decoder.decode(bytes, { stream: true });
    observed = (observed + text).slice(-4096);
    if (["exited", "failed"].includes(state)) return;
    const exit = observed.match(/\r\nRuntime exited status=(0x[0-9a-fA-F]{16})\r\n/);
    if (exit) {
        render("exited", `Emacs exited (${exit[1]}). Restart for a fresh machine.`);
    } else if (/\r\n(?:PANIC: |FAULT vector=)/.test(observed)) {
        fail(new Error("The guest stopped with a kernel fault. The terminal contains its diagnostic."));
    } else if (state === "starting" && observed.includes("*scratch*")) {
        render("running", "Emacs ready · COM1 · 80 × 24 · temporary files");
        element("screen-caption")?.replaceChildren(document.createTextNode("GNU Emacs 30.2 · emacsOS"));
        terminal.focus();
    }
}

async function bootImage() {
    const response = await fetch(new URL("./emacsos.img", import.meta.url));
    if (!response.ok) throw new Error(`Boot disk request failed (${response.status}). Build the MVP image first.`);
    const total = Number(response.headers.get("Content-Length"));
    if (!response.body || !Number.isSafeInteger(total) || total <= 0) return response.arrayBuffer();
    const reader = response.body.getReader();
    const chunks = [];
    let size = 0;
    for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        chunks.push(value);
        size += value.byteLength;
        element("boot-progress").value = Math.min(40, size / total * 40);
    }
    if (size !== total) throw new Error("The boot disk download was incomplete. Retry Power.");
    const bytes = new Uint8Array(size);
    let offset = 0;
    for (const chunk of chunks) { bytes.set(chunk, offset); offset += chunk.byteLength; }
    return bytes;
}

async function biosPackage() {
    // The upstream loader throws outside its initialization promise on XHR
    // failures. Supply verified bytes instead so failures reach our UI.
    const [response, pins] = await Promise.all([
        fetch(new URL("load-rom.data", vendor)),
        fetch(new URL("./assets.json", import.meta.url)),
    ]);
    if (!response.ok) throw new Error(`BIOS package request failed (${response.status}).`);
    if (!pins.ok) throw new Error(`Asset manifest request failed (${pins.status}).`);
    const bytes = await response.arrayBuffer();
    const expected = (await pins.json()).assets["load-rom.data"].sha256;
    const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
    const actual = Array.from(digest, byte => byte.toString(16).padStart(2, "0")).join("");
    if (actual !== expected) throw new Error("The BIOS package checksum does not match. Run make web-assets and retry.");
    return bytes;
}

async function start() {
    if (!(state === "off" || (state === "failed" && retryable))) return;
    render("loading", "Loading the boot disk…");
    element("boot-progress").value = 0;
    try {
        if (!window.isSecureContext || !window.crossOriginIsolated || typeof SharedArrayBuffer === "undefined") {
            throw new Error("Use HTTPS or localhost with cross-origin isolation. Plain HTTP on a remote address cannot run this machine.");
        }
        if (typeof Terminal !== "function" || typeof openpty !== "function") {
            throw new Error("Terminal assets are unavailable. Run make web-assets and reload.");
        }
        const [image, bios] = await Promise.all([bootImage(), biosPackage()]);
        terminal = new Terminal({
            cols: 80, rows: 24, cursorBlink: true, macOptionIsMeta: true,
            fontFamily: "ui-monospace, Menlo, Consolas, monospace", fontSize: 16,
            theme: { background: "#101b16", foreground: "#d4dec7", cursor: "#d4dec7" },
        });
        const write = terminal.write.bind(terminal);
        terminal.write = (bytes, ...args) => { observe(bytes); return write(bytes, ...args); };
        terminal.open(element("terminal"));
        const pty = openpty();
        slave = pty.slave;
        terminal.loadAddon(pty.master);
        const raw = () => slave.ioctl("TCSETS", { ...slave.ioctl("TCGETS"), iflag: 0, oflag: 0, lflag: 0 });
        raw();
        terminal.options.disableStdin = true;
        const moduleUrl = new URL("out.js", vendor).href;
        const config = {
            arguments: ["-M", "pc", "-m", "1G", "-nic", "none", "-display", "none", "-monitor", "none",
                "-serial", "stdio", "-L", "/pack-rom", "-accel", "tcg,tb-size=64",
                "-drive", "file=/emacsos.img,format=raw,if=ide,index=0,media=disk"],
            locateFile: path => new URL(path, vendor).href,
            mainScriptUrlOrBlob: moduleUrl,
            getPreloadedPackage: () => bios,
            pty: slave, preRun: [],
            onExit: code => {
                if (state === "failed") return;
                if (code) fail(new Error(`Emulator exited unexpectedly (${code}).`));
                else render("exited", "Emulator exited. Restart for a fresh machine.");
            },
            onAbort: reason => fail(new Error(`Emulator aborted: ${reason}`)),
        };
        // The upstream BIOS loader attaches its package callbacks to this object.
        window.Module = config;
        initializationStarted = true;
        retryable = false;
        // Workers can fail before the upstream pool installs its own handler.
        // This page owns only emulator workers; observe each from construction.
        const BrowserWorker = window.Worker;
        window.Worker = class extends BrowserWorker {
            constructor(...args) {
                super(...args);
                this.addEventListener("error", event => fail(new Error(event.message || "An emulator worker could not start. Restart to retry.")));
                this.addEventListener("messageerror", () => fail(new Error("An emulator worker could not decode a message.")));
            }
        };
        render("starting", "Starting QEMU/Wasm and booting emacsOS…");
        element("boot-progress").removeAttribute("value");
        const [{ default: initialize }] = await Promise.all([
            import(moduleUrl),
            new Promise((resolve, reject) => {
                const script = document.createElement("script");
                script.src = new URL("load-rom.js", vendor).href;
                script.onload = resolve;
                script.onerror = () => reject(new Error("Could not load the BIOS package. Restart to retry."));
                document.head.append(script);
            }),
        ]);
        config.preRun.push(module => module.FS.writeFile("/emacsos.img", image instanceof Uint8Array ? image : new Uint8Array(image), { canOwn: true }));
        machine = await initialize(config);
        raw();
        // The bridge's poll/read must report would-block instead of fake EOF.
        const streams = machine.TTY.stream_ops;
        const read = streams.read;
        streams.poll = () => (slave.readable ? 1 : 0) | (slave.writable ? 4 : 0);
        streams.read = (stream, buffer, offset, length, position) => {
            if (length && !slave.readable) throw new machine.FS.ErrnoError(6);
            return read(stream, buffer, offset, length, position);
        };
    } catch (error) {
        if (!initializationStarted && terminal) {
            terminal.dispose(); terminal = undefined;
            element("terminal").replaceChildren();
        }
        fail(error);
    }
}

function send(data) {
    if (state !== "running") return;
    // xterm.js 5.3 has no public input() method. Its paste() sends input;
    // omit bracketed-paste framing only for these control sequences.
    const previous = terminal.options.ignoreBracketedPasteMode;
    terminal.options.ignoreBracketedPasteMode = true;
    try { terminal.paste(data); }
    finally { terminal.options.ignoreBracketedPasteMode = previous; }
    terminal.focus();
}

element("boot").addEventListener("click", start);
element("reset").addEventListener("click", () => {
    if (initializationStarted && !window.confirm("Restart this machine? All temporary files and unsaved work will be lost.")) return;
    approvedReload = true;
    window.location.reload();
});
element("command-key").addEventListener("click", () => send("\x1bx"));
element("cancel-key").addEventListener("click", () => send("\x07"));
element("eval-key").addEventListener("click", () => send("\x1b:"));
element("fullscreen").addEventListener("click", async () => {
    try {
        if (document.fullscreenElement) await document.exitFullscreen();
        else await element("monitor").requestFullscreen();
        terminal?.focus();
    } catch (error) {
        element("error").hidden = false;
        element("error").textContent = `Fullscreen is unavailable: ${error.message}`;
    }
});
window.addEventListener("beforeunload", event => {
    if (initializationStarted && !approvedReload) { event.preventDefault(); event.returnValue = ""; }
});
// Worker startup errors in this prebuilt emulator can escape its onAbort hook
// and leave its initialization promise pending. Surface uncaught failures too.
window.addEventListener("error", event => {
    if (initializationStarted && ["starting", "running"].includes(state)) {
        fail(event.error || new Error(event.message || "An emulator worker or script failed."));
    }
});
window.addEventListener("unhandledrejection", event => {
    if (initializationStarted && ["starting", "running"].includes(state)) fail(event.reason);
});
render("off", "Power off · files are temporary");
