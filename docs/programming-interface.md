# Programming-interface plan

Elisp is the intended public programming language. Reuse standard evaluation,
loading, buffers, file functions and network connection objects. Function
availability in upstream Emacs is not evidence that a guest backend supports it.
**No guest Elisp API runs on the current scaffold.**

## Platform contract first

Define a small PAL for the operations needed by the first runtime port:

- Console input/output, including short reads/writes and terminal readiness.
- Memory allocation/mapping requirements and precise failure behavior.
- Monotonic time, timer deadlines and bounded/event-driven waiting.
- Readable runtime files required to load Emacs, Lisp and any portable dump.

State what the compatibility adapter translates and what the kernel implements.
Do not invent success-only stubs for unsupported semantics. Resource acquisition
must identify ownership; close/release consumes the resource according to an
explicit rule even when cleanup reports an error. Distinguish temporary
unavailability, EOF, invalid handles and operational failure.

Use explicit operation and result types. Preserve useful underlying errors
through the adapter so Emacs can signal its ordinary conditions. Document
blocking operations, readiness notifications, cancellation limits and buffer
lifetimes. Bound queues and handle tables where they exist. The exact ABI,
handle representation and event transport remain design work.

## File APIs

After interactive Emacs works, implement and document the supported subset of
`insert-file-contents`, `write-region`, `find-file-noselect`, `save-buffer`,
`directory-files`, `make-directory`, `file-attributes`, `copy-file`, `rename-file`
and `delete-file`. Specify supported flags, metadata and namespace rules.

Keep ordinary Emacs saving, backups, locks, encodings and modified-buffer state
on failure. Begin with a clear working-file backend; persistent disks follow
explicit format, synchronization and recovery decisions. Never infer durable
saving from a successful RAM write or suppress normal save behavior to pass a
test. Later persistence needs actual restart/readback evidence and honest
partial-write/error behavior.

## Network APIs

Prove an available native and browser transport before choosing a device or
host bridge. Identify which part transports guest packets or streams, its
limits and how readiness reaches the runtime. A host mock or JavaScript fetch
alone is not guest networking.

Then support outbound TCP to numeric addresses through standard Emacs
connection functions, filters and sentinels. Cover connect, send/receive,
readiness/status, EOF, timeout, error and close without leaked resources. Add
DNS and a controlled HTTP-to-file program afterward. TLS, HTTPS, listening
sockets and UDP require separate decisions; they are not implied.

The functional goal is to author a program inside emacsOS, receive data, save
and read it, then continue editing retained source. The [roadmap](roadmap.md)
orders the prerequisites. None of this functionality is implemented by the
initial source reset.
