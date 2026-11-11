# Public programming-interface plan

Elisp is the intended public language. Reuse standard evaluation, library
loading, buffers, file functions and network connection objects. Buffers and
Eshell are tool clients of those APIs. **No guest Elisp runs on the current
scaffold.** [The architecture](design.md) defines the implementation boundaries;
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
description/offset, separate opens do not. Precise types/bounds are P1 work.
Neither a handle nor an Emacs process object establishes isolation.

## File APIs

F0 supports explicitly volatile working files after read-only runtime loading
and interactive Emacs. Audit `insert-file-contents`, `write-region`,
`find-file-noselect`, `save-buffer`, `directory-files`, `make-directory`,
`file-attributes`, `copy-file`, `rename-file` and `delete-file`. State supported
flags, metadata, directory/namespace behavior and limits.

Preserve normal encodings, backups, locks and modified-buffer/error behavior.
Do not suppress ordinary mechanisms to pass tests. Cleanup/final-close errors
must stay visible; a successful streaming write does not promise rollback on a
later failure. The clean upstream fork has not inherited old save fixes or
validated file behavior. Ordinary `write-region` can omit fsync, so saving is
not inherently a durability boundary.

F1 separately decides persistent format, synchronization, recovery and honest
partial-write behavior. Only later actual retained-file checks can establish
persistence. No service manager or configuration-snapshot protocol is required
for the basic file interface.

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
implemented by this architecture proposal.
