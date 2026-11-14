# Persistent DATA disk

The fixed QEMU profile uses two separate IDE disks. SYSTEM (index 0) contains
the kernel and runtime and retains its snapshot-backed launcher behavior. DATA
(index 1, primary slave) is a 64 MiB raw, unpartitioned littlefs volume projected
at `/work`. `/tmp` and `/home/user` remain volatile; `/usr` stays read-only.
This adds no networking, protected processes, service manager or automatic
execution of user files.

## Default native use

`make run` creates `build/data/emacsos-data.img` from the initialized empty
template on first use, then reuses that file. Override it with `EMACSOS_DATA` or
`python3 scripts/qemu.py --data PATH`. Existing images are never formatted or
replaced by the launcher. An existing image must be a regular 64 MiB file.
`--no-data` explicitly selects the original volatile profile.

A disk absent at boot preserves the volatile `/work` behavior. An attached but
invalid, unsupported or unreadable DATA disk must not become a fresh writable
filesystem. The guest exposes the mount failure and keeps DATA unavailable;
remove or replace the image before rebooting. A fatal DATA I/O error makes
subsequent mutations fail read-only and is reported through STATUS without
treating ordinary missing-file errors as device failure. Formatting is an offline
build operation, never a recovery action performed on an imported image.

Standard Elisp file operations use the same typed PAL and bounded ABI adapter
as volatile files. Parent-owned startup policy enables normal GNU Emacs file
synchronization (`write-region-inhibit-fsync` is nil). Save, backup, symlink
locking and original file errors remain enabled. A successful guest save reaches
littlefs synchronization and an ATA cache flush. This is the emulated device
boundary, not proof of physical durability through every host or power failure.

## Volume and file semantics

`third_party/littlefs/SOURCE.json` pins the unmodified littlefs sources and their
license. The volume also carries an emacsOS adapter version. User paths project
only the user tree; adapter object IDs, staging entries and version metadata are
outside that tree. A generic littlefs volume without the emacsOS layout is not
an importable emacsOS DATA disk.

Stable object IDs preserve an open regular file across rename, replacement and
unlink. Independent opens see the same object contents while retaining separate
offsets; duplicated descriptors share an offset. Namespace publication uses
littlefs atomic operations. Symlink targets are stored as object data rather
than being constrained by littlefs's smaller custom-attribute limit. Modes and
timestamps are stored with object metadata. Unreachable adapter objects can be
reclaimed without deleting ordinary user filenames, including names beginning
with `.#`.

Each mounted boot gets a durable, monotonically increasing identity used by the
bounded runtime PID interface. GNU Emacs can recognize an earlier boot's lock
owner as gone. The guest does not disable locking or delete user lock-like names
at startup. This identity does not implement independent OS processes.

## Browser retention and disk exchange

The browser creates an initialized empty DATA disk automatically. Successful
checkpoints are retained in IndexedDB for the same browser profile and origin.
Import and export are optional; routine first use requires neither action.
Changing the host, scheme or port changes the browser origin and its retained
store. Private browsing, quota limits and browser eviction can prevent or remove
retention. The page distinguishes retained data from session-only data and
reports errors without silently replacing the previous checkpoint.

The page and guest use a bounded COM2 control protocol, separate from the COM1
terminal. At a safe kernel boundary, FREEZE rejects DATA mutations, synchronizes
open objects and namespace metadata, flushes ATA and acknowledges the current
generation. The browser copies independent disk bytes only after that response.
Autosave/export can then thaw the guest; committing a checkpoint requires a
completed IndexedDB transaction. Restart keeps the guest frozen until the
checkpoint commits and the page reloads. A failed checkpoint does not claim a
successful retained save. Browser unload warnings cannot guarantee a final
checkpoint; use the page's save/restart controls for an explicit boundary.

Requests are serialized and carry session/request identities. A Web Lock
prevents two tabs from writing the same retained disk concurrently. Import is
staged before POWER: the page checks the image size, then the guest validates the
volume and adapter layout before the candidate can replace the old checkpoint.
Failed imports preserve the previous retained image. The current version has
no disk hot-swap, host-directory mounts, partitions, resizing or encryption.

An exported disk is a consistent full DATA image. It can be supplied to the
native launcher or imported before a browser boot. SYSTEM is a separate artifact
and is not included in the export. Starting with `-Q` still avoids automatic
loading of imported user Lisp.

## Validation boundaries

`make test-files` runs the actual backend and adapter on the host with
sanitizers, including object identity, limits, metadata, remount and injected
storage failures. `make test-probes` separately exercises real ATA device I/O
and COM2 control in native QEMU. Neither proves an interactive Emacs save.

`make test-storage` boots the real runtime repeatedly with a private DATA image.
It repeats the original editing/application acceptance on littlefs, then saves
source, modes, symlinks and a prior-boot lock, freezes and copies DATA, resumes
writes and reopens the independent checkpoint in a fresh guest. Evidence lives
under `build/image/storage/` and binds source and artifact identities.
Browser acceptance is separate because browser retention and its Wasm device
implementation are different boundaries; see [browser checks](web-browser-checks.md).
