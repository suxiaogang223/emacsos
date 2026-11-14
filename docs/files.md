# File backends

The kernel links a typed file namespace and a bounded Linux x86-64 descriptor
adapter. The runtime newc archive supplies immutable files, directories and
symbolic links. `/tmp` and `/home/user` are writable RAM directories; `/work`
uses the mounted littlefs DATA disk when present and valid, or RAM when no disk
is attached. `/dev/tty` and `/dev/null` are bounded console/null device entries.
See [persistent DATA](storage.md) for the format, mount failures and retention
boundaries. RAM-directory changes are lost when QEMU exits.

The namespace supports lookup, regular files, sparse writes, truncation,
directories, rename, unlink, symlinks and metadata. File identity survives rename
and unlink while an open description retains it. Duplicated descriptors share
their offset and status; independent opens have independent offsets. Close
consumes a descriptor, and final unlinked-object release returns its allocation.
Mutable file storage is limited to 24 MiB total and 8 MiB per regular file, with
8192 live namespace nodes and 192 descriptors including standard I/O. Symlink
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

On RAM directories, successful fsync/fdatasync reaches only volatile memory.
For persistent `/work`, it synchronizes the littlefs object and flushes ATA;
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
