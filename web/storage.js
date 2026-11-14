// DATA bytes stay local. Guest acknowledgements, not terminal text, delimit copies.
export const DATA_SIZE = 64 * 1024 * 1024;
const DATABASE = "emacsos-data";
const STORE = "disks";
const KEY = "default";
const HEX_ID = /^[0-9a-f]{16}$/;
const LOCK = "emacsos-data-default";
const hex = value => value.toString(16).padStart(16, "0");
const randomSession = () => Array.from(crypto.getRandomValues(new Uint8Array(8)),
    byte => byte.toString(16).padStart(2, "0")).join("");
export async function digest(bytes) {
    return Array.from(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)),
        byte => byte.toString(16).padStart(2, "0")).join("");
}

export class ControlChannel {
    constructor() {
        this.session = randomSession();
        this.sequence = 0n;
        this.input = [];
        this.line = "";
        this.pending = new Map();
        this.queue = Promise.resolve();
    }
    install(FS) {
        // QEMU's pipe backend opens .in and .out; undefined is EAGAIN, never EOF.
        const input = FS.createDevice("/", "control.in", () => this.input.shift(), () => {});
        FS.getDevice(input.rdev).stream_ops.poll = () => this.input.length ? 1 : 0;
        FS.createDevice("/", "control.out", () => undefined, byte => this.receive(byte));
    }
    receive(byte) {
        if (byte !== 10) {
            if (this.line.length < 128) this.line += String.fromCharCode(byte);
            else this.line = "!invalid";
            return;
        }
        const line = this.line;
        this.line = "";
        const match = /^EOS1 ([0-9a-f]{16}) ([0-9a-f]{16}) (FROZEN|THAWED|STATUS|ERROR) ([0-9a-f]{16}) ([0-9]+) ([01]) ([01])$/.exec(line);
        if (!match || match[1] !== this.session) return;
        const pending = this.pending.get(match[2]);
        if (!pending) return;
        this.pending.delete(match[2]);
        clearTimeout(pending.timer);
        const result = { type: match[3], generation: match[4], errno: Number(match[5]),
            mounted: match[6] === "1", frozen: match[7] === "1" };
        pending.resolve(result);
    }
    request(command, timeout = 12000) {
        const operation = this.queue.then(() => new Promise((resolve, reject) => {
            if (this.closed) { reject(new Error("The DATA control session has ended.")); return; }
            const id = hex(++this.sequence);
            const timer = setTimeout(() => {
                this.pending.delete(id);
                reject(new Error(`${command} acknowledgement timed out. DATA state needs confirmation.`));
            }, timeout);
            this.pending.set(id, { resolve, reject, timer });
            if (this.input.length > 4096) { clearTimeout(timer); this.pending.delete(id); reject(new Error("The DATA control input queue is full.")); return; }
            this.input.push(...new TextEncoder().encode(`EOS1 ${this.session} ${id} ${command}\n`));
        }));
        this.queue = operation.catch(() => {});
        return operation;
    }
    close() {
        this.closed = true;
        for (const pending of this.pending.values()) {
            clearTimeout(pending.timer);
            pending.reject(new Error("The DATA control session has ended."));
        }
        this.pending.clear();
        this.input.length = 0;
    }
}

function openDatabase() {
    return new Promise((resolve, reject) => {
        const request = indexedDB.open(DATABASE, 1);
        request.onupgradeneeded = () => request.result.createObjectStore(STORE);
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error || new Error("Browser storage is unavailable."));
        request.onblocked = () => reject(new Error("Another page blocked browser storage initialization."));
    });
}
function readRecord(db) {
    return new Promise((resolve, reject) => {
        const transaction = db.transaction(STORE, "readonly");
        const request = transaction.objectStore(STORE).get(KEY);
        transaction.oncomplete = () => resolve(request.result);
        transaction.onabort = () => reject(transaction.error || new Error("Could not read the retained DATA disk."));
    });
}
function writeRecord(db, record, current) {
    return new Promise((resolve, reject) => {
        const transaction = db.transaction(STORE, "readwrite");
        const store = transaction.objectStore(STORE);
        const request = store.get(KEY);
        request.onsuccess = () => {
            if (!current()) { transaction.abort(); return; }
            const previous = request.result;
            if (previous?.session === record.session && BigInt(`0x${previous.generation}`) > BigInt(`0x${record.generation}`)) {
                transaction.abort(); return;
            }
            try { store.put(record, KEY); }
            catch { transaction.abort(); }
        };
        transaction.oncomplete = resolve;
        transaction.onabort = () => reject(transaction.error || new Error("The checkpoint was not committed; the previous disk is unchanged."));
    });
}

export class BrowserDisk {
    constructor(update) {
        this.update = update;
        this.control = new ControlChannel();
        this.session = this.control.session;
        this.active = true;
        this.queue = Promise.resolve();
        this.generation = null;
        this.savedGeneration = null;
        this.pending = true;
        this.frozen = false;
        this.unknown = false;
        this.mounted = false;
        this.busy = false;
        this.lastAttempt = 0;
        this.emit("ready", "DATA attaches automatically at POWER.");
    }
    emit(state, message) { this.state = state; this.message = message; this.update(this); }
    async ownership() {
        if (this.hasLock || this.sessionOnly) return;
        if (!navigator.locks) {
            this.sessionOnly = true;
            this.emit("session-only", "Session only: browser ownership locks are unavailable. Export to keep DATA.");
            return;
        }
        const acquired = await new Promise((resolve, reject) => {
            this.lockTask = navigator.locks.request(LOCK, { mode: "exclusive", ifAvailable: true }, lock => {
                if (!lock) { resolve(false); return; }
                this.hasLock = true;
                resolve(true);
                return new Promise(release => { this.release = release; });
            }).catch(reject);
        });
        if (!acquired) throw new Error("DATA is already open in another tab. Close that machine before POWER.");
    }
    prepare() {
        if (!this.preparing) {
            this.preparing = this.prepareDisk().finally(() => { this.preparing = undefined; this.update(this); });
            this.update(this);
        }
        return this.preparing;
    }
    async prepareDisk() {
        await this.ownership();
        if (!this.db && !this.sessionOnly) {
            try { this.db = await openDatabase(); }
            catch (error) { this.sessionOnly = true; this.storageError = error.message; }
        }
        if (!this.bytes && this.db) {
            const record = await readRecord(this.db);
            if (record) {
                if (record.version !== 1 || typeof record.diskId !== "string" || !HEX_ID.test(record.session) ||
                    !HEX_ID.test(record.generation) || !(record.bytes instanceof ArrayBuffer) || record.bytes.byteLength !== DATA_SIZE ||
                    !/^[0-9a-f]{64}$/.test(record.sha256) || await digest(record.bytes) !== record.sha256) {
                    throw new Error("The retained DATA checkpoint failed validation. It is preserved; choose a valid IMPORT or NEW DISK.");
                }
                this.bytes = new Uint8Array(record.bytes);
                this.diskId = record.diskId;
                this.source = "restored";
            }
        }
        if (!this.bytes) await this.newDisk(false);
        this.emit(this.sessionOnly ? "session-only" : "loading", this.sessionOnly ?
            "Session only: automatic retention is unavailable. Export to keep DATA." : "DATA selected; waiting for guest mount validation.");
        return this.bytes;
    }
    async newDisk(notify = true) {
        const response = await fetch(new URL("./data-empty.img", import.meta.url));
        if (!response.ok) throw new Error(`Empty DATA template is unavailable (${response.status}). Build the system first.`);
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (bytes.length !== DATA_SIZE) throw new Error("The empty DATA template has an unexpected size.");
        this.bytes = bytes;
        this.diskId = crypto.randomUUID();
        this.source = "new";
        if (notify) this.emit("ready", "New DATA staged. The retained disk is replaced only after guest validation and checkpoint.");
    }
    async import(file) {
        if (file.size !== DATA_SIZE) throw new Error("Choose a 64 MiB raw emacsOS DATA image. The original disk was not changed.");
        const bytes = new Uint8Array(await file.arrayBuffer());
        if (bytes.length !== DATA_SIZE) throw new Error("The DATA import was incomplete.");
        this.bytes = bytes;
        this.diskId = crypto.randomUUID();
        this.source = "imported";
        this.emit("ready", "Import staged. The guest must validate its filesystem before it replaces retained DATA.");
    }
    install(FS) {
        FS.writeFile("/data.img", this.bytes, { canOwn: true });
        this.bytes = undefined;
        this.control.install(FS);
    }
    async attach(machine) {
        this.machine = machine;
        const status = await this.control.request("STATUS");
        this.applyStatus(status);
        if (!status.mounted || status.errno) {
            this.emit("error", status.mounted ?
                `DATA reports a storage fault (error ${status.errno}). The previous retained checkpoint is preserved.` :
                `DATA mount rejected (error ${status.errno}). Original retained/imported bytes are preserved; recovery files are volatile.`);
            return;
        }
        try { await this.checkpoint(); }
        finally { this.timer = setInterval(() => this.poll(), 1000); }
    }
    applyStatus(status) {
        this.frozen = status.frozen;
        this.mounted = status.mounted;
        this.generation = status.generation;
        // STATUS carries latched media health; THAW can succeed without clearing it.
        if (status.type === "STATUS" && status.errno) this.healthError = status.errno;
        this.pending = this.savedGeneration !== this.generation;
    }
    serialize(operation) {
        const result = this.queue.then(async () => {
            if (!this.active) throw new Error("This DATA session has ended.");
            this.busy = true; this.update(this);
            try { return await operation(); }
            finally { this.busy = false; this.update(this); }
        });
        this.queue = result.catch(() => {});
        return result;
    }
    async poll() {
        if (this.busy || this.unknown || !this.active || !this.mounted) return;
        try {
            await this.serialize(async () => {
                const status = await this.control.request("STATUS", 5000);
                this.applyStatus(status);
                if (status.errno || !status.mounted || status.frozen) throw new Error(
                    `DATA needs attention (error ${status.errno}); its guest state is not writable.`);
                if (this.pending) {
                    if (this.sessionOnly || !this.db || !this.hasLock) this.emit("session-only", "Session only: export DATA before closing this page.");
                    else this.emit(this.retentionError ? "error" : "pending", this.retentionError ?
                        this.retentionError + " Changes remain pending; the last checkpoint is preserved." : "Changes pending in this browser.");
                }
            });
            if (this.pending && this.db && this.hasLock && Date.now() - this.lastAttempt >= 5000) await this.checkpoint();
        } catch (error) {
            this.emit("error", error.message + " Last completed checkpoint is preserved.");
        }
    }
    async freeze() {
        this.unknown = true;
        let status;
        try { status = await this.control.request("FREEZE"); }
        catch { status = await this.control.request("STATUS"); }
        this.applyStatus(status);
        if (!status.frozen) { this.unknown = false; throw new Error(`DATA freeze failed (error ${status.errno}).`); }
        if (status.errno || !status.mounted) throw new Error(`DATA synchronization failed (error ${status.errno}).`);
        this.unknown = false;
        return status;
    }
    async thaw() {
        this.unknown = true;
        let status;
        try { status = await this.control.request("THAW"); }
        catch { status = await this.control.request("STATUS"); }
        this.applyStatus(status);
        if (!status.mounted || status.frozen || status.errno) throw new Error("DATA resume was not confirmed. Use RESUME before further storage actions.");
        this.unknown = false;
    }
    async recover() {
        return this.serialize(async () => {
            const status = await this.control.request("STATUS");
            this.applyStatus(status);
            if (!status.mounted || status.errno) throw new Error(`DATA recovery failed (error ${status.errno}).`);
            if (status.frozen) await this.thaw();
            else this.unknown = false;
            this.emit("pending", "DATA resumed; changes await a completed checkpoint.");
        });
    }
    checkpoint({ restart = false, exportImage = false } = {}) {
        return this.serialize(async () => {
            this.lastAttempt = Date.now();
            if (this.unknown) throw new Error("DATA state is unresolved. Use RESUME to confirm its state.");
            if (!this.mounted) throw new Error("DATA is not mounted. The previous retained disk is unchanged.");
            if (this.healthError) throw new Error(`DATA reports a storage fault (error ${this.healthError}). The last checkpoint is preserved.`);
            this.emit("pending", restart ? "Saving DATA before restart…" : "Checkpointing DATA…");
            let copy, generation;
            try {
                const status = await this.freeze();
                generation = status.generation;
                // readFile returns owned bytes in this artifact; slice makes ownership explicit.
                copy = this.machine.FS.readFile("/data.img").slice();
                if (copy.byteLength !== DATA_SIZE) throw new Error("The emulated DATA disk changed size.");
                if (!restart) await this.thaw();
                if (!this.active) throw new Error("This DATA session has ended.");
                if (!this.db || !this.hasLock) {
                    if (restart) throw new Error("Automatic retention is unavailable. Export DATA or explicitly discard before restarting.");
                    this.emit("session-only", "Session only: export DATA before closing this page.");
                } else {
                    try {
                        const sha256 = await digest(copy);
                        const record = { version: 1, diskId: this.diskId, session: this.session,
                            generation, sha256, bytes: copy.buffer, savedAt: Date.now() };
                        await writeRecord(this.db, record, () => this.active && this.hasLock && record.session === this.session);
                        this.savedGeneration = generation;
                        let unconfirmed = false;
                        if (!restart) {
                            try { this.applyStatus(await this.control.request("STATUS")); }
                            catch { unconfirmed = true; }
                        }
                        this.pending = unconfirmed || Boolean(this.healthError) || !this.mounted || this.generation !== generation;
                        if (this.healthError || !this.mounted) {
                            this.retentionError = `DATA reports a storage fault (error ${this.healthError || "unknown"}).`;
                            this.emit("error", "Checkpoint saved; " + this.retentionError + " Further snapshots are disabled.");
                        } else {
                            this.emit(this.pending ? "pending" : "saved", this.pending ?
                                "Checkpoint saved; newer changes are pending or unconfirmed." : "Saved in this browser · /work on DATA");
                            this.retentionError = undefined;
                        }
                    } catch (error) {
                        if (!exportImage) throw error;
                        this.retentionError = error.message;
                        this.emit("error", "Exporting current DATA; browser retention failed. Previous checkpoint is preserved.");
                    }
                }
                if (exportImage) {
                    const url = URL.createObjectURL(new Blob([copy], { type: "application/octet-stream" }));
                    const anchor = document.createElement("a");
                    anchor.href = url; anchor.download = "emacsos-data.img"; anchor.click();
                    setTimeout(() => URL.revokeObjectURL(url), 60000);
                }
                return copy;
            } catch (error) {
                if (this.frozen && !this.unknown) {
                    try { await this.thaw(); } catch { this.unknown = true; }
                }
                this.retentionError = error.message;
                this.emit("error", error.message + (this.unknown ? " DATA state is unknown or frozen; use RESUME." : " Previous browser checkpoint is unchanged."));
                throw error;
            }
        });
    }
    close() {
        this.active = false;
        clearInterval(this.timer);
        this.control.close();
        return this.queue.finally(async () => {
            this.db?.close();
            this.hasLock = false;
            this.release?.();
            await this.lockTask;
        });
    }
}
