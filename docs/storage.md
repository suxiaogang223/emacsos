# Persistent DATA disk

The fixed QEMU profile uses two separate IDE disks. SYSTEM (index 0) contains
the kernel and runtime and retains its snapshot-backed launcher behavior. DATA
(index 1, primary slave) is a 64 MiB raw, unpartitioned littlefs volume projected
at `/etc`, `/home`, `/tmp` and `/var`. `/home/user` is initialized on DATA; `/usr`
stays read-only. Littlefs is the only writable file backend.
This adds no networking, protected processes, service manager or automatic
execution of user files.

## Default native use

`make run` creates `build/data/emacsos-data.img` from the initialized empty
template on first use, then reuses that file. Override it with `EMACSOS_DATA` or
`python3 scripts/qemu.py --data PATH`. Existing images are never formatted or
replaced by the launcher. An existing image must be a regular 64 MiB file.
`--no-data` explicitly starts read-only recovery without a DATA disk.

An absent, invalid, unsupported or unreadable DATA disk enters read-only
recovery. `/etc`, `/home`, `/tmp` and `/var` remain visible but cannot create, modify or
remove files. Emacs still reads its installed runtime, evaluates Elisp and edits
buffers; it never silently saves to temporary RAM storage. Remove or replace the
image before rebooting. A fatal DATA I/O error likewise makes subsequent
mutations fail read-only and is reported through STATUS without treating
ordinary missing-file errors as device failure. Formatting is an offline build
operation, never an automatic recovery action on an existing image.

`/tmp` is an ordinary directory on the same DATA volume. Applications own the
lifetime and cleanup of temporary files; this version has no automatic boot purge
or separate tmpfs. A checkpoint includes files under all four DATA directories.
Cross-directory rename preserves file identity on the shared volume.
`HOME`, `PWD` and the initial working directory are `/home/user`; projects can
live in its initialized `projects` directory. See [directory layout](files.md#directory-layout)
for configuration, state and log placement. `/work` has been removed.

SYSTEM supplies minimal read-only `/etc/passwd` and `/etc/group` records for
recovery. The fresh DATA template includes matching defaults; a successful
mount replaces the entire `/etc` tree only after all candidate entries validate.
User configuration is stored without being loaded automatically. The default
uid/gid 0 identity is unchanged and supplies no multiuser isolation.

Standard Elisp file operations use the same typed PAL and bounded ABI adapter
as the read-only runtime namespace. Parent-owned startup policy enables normal GNU Emacs file
synchronization (`write-region-inhibit-fsync` is nil). Save, backup, symlink
locking and original file errors remain enabled. A successful guest save reaches
littlefs synchronization and an ATA cache flush. This is the emulated device
boundary, not proof of physical durability through every host or power failure.

## Volume and file semantics

`third_party/littlefs/` is a submodule pinned to the official littlefs repository.
`third_party/littlefs.json` records the release, commit and hashes of the compiled
source files and upstream license. The volume carries the `EOSDATA3` adapter
version. Its private `/tree` contains the four directory anchors; adapter object IDs, staging entries and version metadata are
outside that tree. A generic littlefs volume without the emacsOS layout is not
an importable emacsOS DATA disk. Earlier unmerged `EOSDATA1` and `EOSDATA2` images are explicitly
rejected without modification; there is no implicit format migration. Create a
new disk when testing this revised format and preserve old images separately.
The formatter initializes `/etc/emacsos`, `/home/user/projects`, `/var/lib/emacsos`,
`/var/log` and `/var/cache` alongside the four anchors and account defaults.
Mount validation requires the four anchors and a real `/home/user` directory
before recovery writes, garbage collection or namespace publication.

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
`make test-guest` exercises normal editing/application acceptance on a fresh
DATA disk. The storage check separately saves source, modes, symlinks, home and
temporary files and a prior-boot lock, freezes all four DATA directories, copies
the disk, resumes writes and reopens the checkpoint in a fresh guest. It also
boots absent, corrupt and unsupported DATA profiles, proving interactive
read-only recovery and preservation of rejected bytes. Evidence lives
under `build/image/storage/` and binds source and artifact identities.
Browser acceptance is separate because browser retention and its Wasm device
implementation are different boundaries; see [browser checks](web-browser-checks.md).
