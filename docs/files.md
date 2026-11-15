# File namespace and storage

The kernel links a typed file namespace and a bounded Linux x86-64 descriptor
adapter. The runtime newc archive supplies immutable files, directories and
symbolic links. All writable regular files, directories and symlinks live on
one littlefs DATA volume exposed at `/etc`, `/home`, `/tmp` and `/var`.
`/usr` and the surrounding namespace are read-only. `/dev/tty` and
`/dev/null` provide console/null device I/O rather than regular-file storage.
There is no writable RAM file backend or no-disk fallback. Missing or rejected
DATA leaves all four directory anchors read-only while the runtime and editable
Emacs buffers remain available. See [DATA storage](storage.md).

## Directory layout

The namespace follows conventional Unix directory responsibilities without
claiming full FHS or Linux compatibility. Directory placement does not require
a separate partition or filesystem for each path.

| Path | Purpose | Backing |
| --- | --- | --- |
| `/usr` | Installed Emacs, system Lisp and runtime resources. | Immutable SYSTEM archive. |
| `/etc` | System configuration; `/etc/emacsos` is initialized empty. | DATA; minimal immutable account records remain available in recovery. |
| `/home/user` | Default home and initial working directory; `projects` is initialized empty. | DATA. |
| `/var/lib/emacsos` | Persistent system/application state. | DATA; initialized empty. |
| `/var/log` | Logs when an application writes them. | DATA; initialized empty. |
| `/var/cache` | Rebuildable cached data. | DATA; initialized empty. |
| `/tmp` | Application-managed temporary files. | DATA; no automatic boot purge or tmpfs. |
| `/dev/tty`, `/dev/null` | Console and null device operations. | Kernel devices, not stored DATA files. |

`HOME`, `PWD`, the initial descriptor-adapter working directory and the default
account home agree on `/home/user`. The uid/gid 0 record is a trusted-runtime
placeholder, not an implemented account or permission boundary. No shell binary
is provided. `/work` is no longer part of the guest namespace.

Normal DATA initialization includes `/etc/passwd` and `/etc/group` with the same
minimal defaults as SYSTEM. On a successful mount, the complete DATA `/etc`
replaces the immutable recovery directory; this is not an overlay or a file-by-file
fallback. Configuration edits persist, but storage alone does not add a
configuration loader or enable automatic execution of user Lisp. Rejected DATA
never replaces the recovery namespace. `/home/user` must remain a real directory
for a volume to mount; other initialized application directories are ordinary
user-managed directories. `/run`, `/proc`, `/sys`, general mounts and multiuser
security remain future work.

## File operations

The namespace supports lookup, regular files, sparse writes, truncation,
directories, rename, unlink, symlinks and metadata. File identity survives rename
and unlink while an open description retains it. Duplicated descriptors share
their offset and status; independent opens have independent offsets. Close
consumes a descriptor, and final unlinked-object release returns its allocation.
Each regular file is limited to 8 MiB; the namespace has at most 8192 live nodes
and the descriptor table has 192 entries shared by files, standard I/O and TCP
streams. The [network backend](network.md) separately limits unique streams to
16; duplicating a descriptor does not create another stream. File contents stream through bounded
littlefs caches. In-memory indexes, readonly archive backing and private mapping
copies are not a second writable filesystem. Symlink
resolution is bounded to 16 traversals. Paths/components are bounded to
1023/255 bytes. Capacity errors remain visible; writes are not transactional.

The adapter translates supported open/status flags, descriptor duplication,
byte/vector I/O, positioned reads, seeks, stat, directory iteration, access,
working directory, rename/remove, symlinks, mode and timestamp operations.
Private file mapping copies are validated before allocation/replacement; pages
wholly beyond EOF are rejected rather than claiming unavailable SIGBUS behavior.
Unaligned stat/iovec/timestamp buffers are copied without C alignment assumptions.
Unknown options fail explicitly. Hard links return ENOSYS, allowing upstream
fallbacks where they exist. Removing or replacing an open directory returns
EBUSY. O_CREAT with O_DIRECTORY, and mutating/access flags with O_PATH, are
rejected before namespace changes. This is a documented runtime subset, not
general Linux filesystem compatibility or a multiuser permissions system.

Successful fsync/fdatasync on DATA synchronizes littlefs and flushes ATA;
browser retention additionally requires a completed checkpoint transaction. Normal GNU Emacs
save, backup, symlink-lock and error behavior must stay enabled in guest
acceptance. No archived Emacs save changes were restored. The backend does not
intercept Lisp application ownership; explicit application adoption is separate.

Run `make test-files` with Clang and its sanitizer runtime. It
compiles these exact PAL/adapter sources and exercises readonly archives,
descriptor aliasing, open-unlink/rename identity, sparse data, reclamation,
limits, symlinks, failure paths, alignment and malformed archive rejection on
the host. `make test-probes` additionally checks that the
linked backend preserves the static-musl kernel probes. These checks alone do
not establish interactive GNU Emacs file acceptance; the complete MVP needs
actual guest save/reopen/load, backup, lock and original-error evidence.
