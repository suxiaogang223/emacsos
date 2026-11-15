# File namespace and storage

The kernel links a typed file namespace and a bounded Linux x86-64 descriptor
adapter. The runtime newc archive supplies immutable files, directories and
symbolic links. All writable regular files, directories and symlinks live on
one littlefs DATA volume exposed at `/work`, `/home` (including `/home/user`)
and `/tmp`. `/usr` and the surrounding namespace are read-only. `/dev/tty` and
`/dev/null` provide console/null device I/O rather than regular-file storage.
There is no writable RAM file backend or no-disk fallback. Missing or rejected
DATA leaves all three directory anchors read-only while the runtime and editable
Emacs buffers remain available. See [DATA storage](storage.md).

The namespace supports lookup, regular files, sparse writes, truncation,
directories, rename, unlink, symlinks and metadata. File identity survives rename
and unlink while an open description retains it. Duplicated descriptors share
their offset and status; independent opens have independent offsets. Close
consumes a descriptor, and final unlinked-object release returns its allocation.
Each regular file is limited to 8 MiB; the namespace has at most 8192 live nodes
and 192 descriptors including standard I/O. File contents stream through bounded
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
