# Public programming-interface plan

Elisp is the intended public language. Reuse standard evaluation, library
loading, buffers, file functions and network connection objects. Buffers and
Eshell are tool clients of those APIs. The [MVP](mvp.md) runs interactive guest
Elisp and file APIs; F1 supplies littlefs DATA for all writable directories. Networking and external subprocesses remain
unimplemented. [The architecture](design.md) defines the implementation boundaries;
[runtime support](runtime-port.md) explains the first static Emacs/musl port.

## API and resource rules

Document the supported subset of standard GNU Emacs behavior rather than adding
a parallel family of wrappers. Standard functions are available only when their
real backend works. Necessary platform/runtime changes belong to the fork;
future system Lisp policy belongs to the parent repository.

The limited Linux ABI adapter translates to typed PAL operations. It preserves
partial I/O, would-block, EOF, real errors and resource ownership. Readiness is
level-style and rechecked after wake; deadlines are monotonic. Close consumes
the handle even if final cleanup fails; duplicated descriptors share an open
description/offset, separate opens do not. The implemented profile and bounds
are recorded in [kernel bootstrap](kernel-bootstrap.md)
and [files](files.md).
Neither a handle nor an Emacs process object establishes isolation.

## Application management

[Applications and processes](process-model.md) are distinct from buffers and
GNU connection objects. A0 adds start/list/inspect/request-stop/finish/wait and
restart-as-new-run contracts after interactive E0. Managed callback entry carries
a run identity/context and restores its caller on unwind; owned/shared/borrowed
resource adoption and late-event suppression are explicit.

Wait is nonblocking inspection/polling in the initial runtime. Cancellation is
cooperative, resource tracking is not enforcement over ambient standard APIs,
and the shared heap has no per-app isolation/precise memory accounting. Bounded
value messages require explicit serialization before future protected process
boundaries. Kernel processes and forced termination are a later K0 implementation;
file/network APIs do not depend on that implementation or an app framework.

## File APIs

The F0 file API remains the common public surface over littlefs after read-only
runtime loading and interactive Emacs. It includes `insert-file-contents`, `write-region`,
`find-file-noselect`, `save-buffer`, `directory-files`, `make-directory`,
`file-attributes`, `copy-file`, `rename-file` and `delete-file`. State supported
flags, metadata, directory/namespace behavior and limits.

Preserve normal encodings, backups, locks and modified-buffer/error behavior.
Do not suppress ordinary mechanisms to pass tests. Cleanup/final-close errors
must stay visible; a successful streaming write does not promise rollback on a
later failure. The unchanged runtime fork uses the parent file backend; the MVP
validates ordinary saves, backups and locks in the actual guest. Ordinary
`write-region` can omit fsync, so saving is
not inherently a durability boundary. The controlled parent startup therefore
sets `write-region-inhibit-fsync` to nil. Applications that override it also
change their save guarantee.

F1 uses the same functions over one littlefs DATA disk projected at `/work`,
`/home` and `/tmp`. Guest
sync reaches littlefs and ATA flush; browser retention needs an acknowledged
frozen copy and completed IndexedDB transaction. No writable RAM file backend remains. Missing or rejected DATA supports only
read-only file operations; buffer editing and evaluation still work.
See [storage](storage.md) for format, recovery and separate acceptance. No service
manager or configuration-snapshot protocol is required for the basic file interface.

## Network APIs

N0 remains open: prove native and future Wasm transport feasibility and disclose
any host bridge before selecting a NIC, protocol stack or event transport.
A host mock or direct JavaScript fetch is not a working guest network path.

N1 then supports outbound TCP to numeric addresses via `make-network-process`,
`open-network-stream`, `process-send-string`, filters/sentinels, status and
`delete-process`. Specify connect progress, short send/receive, readiness, EOF,
timeouts, real failure and cleanup. `:nowait` does not guarantee every later
send is nonblocking; document actual behavior. Callbacks must return promptly;
continuous Lisp/C computation can still block interaction.

N2 adds DNS and a controlled HTTP-to-file program. TLS/HTTPS, strong entropy,
listening sockets and UDP are separate decisions. The eventual program is
authored inside emacsOS, receives data and saves/reads it through standard APIs;
continuing from retained source requires F1. None of those capabilities is
implemented by the MVP.
