# Roadmap

Prioritize programming, file operations and networking. The current baseline is
a minimal boot/serial source scaffold, not a running Emacs system. Compilation
and static checks establish only their stated scope; boot and functional
acceptance are not part of this initialization.

## Implementation sequence

| Item | Deliverable | Dependencies and future validation |
| --- | --- | --- |
| B0 | Minimal BIOS/x86-64 kernel and serial scaffold. | Source/build foundation present; boot behavior remains unexecuted here. |
| P0 | Small PAL and runtime bootstrap contracts. | Console, memory, time/wait, ownership and errors; decide libc/compatibility path and readable runtime files. P0 is a plan, not implemented APIs. |
| E0 | Interactive Emacs and Elisp evaluation. | P0 plus executable/runtime loading and terminal integration. Later prove actual guest interaction and evaluation. |
| F0 | Working standard file APIs. | E0 and a file PAL/backend. Start with explicitly volatile working files; preserve ordinary saves, backups, locks and errors. |
| F1 | Minimal persistent file backend. | F0; decide the durable format, synchronization and recovery contract, then prove retained files separately. No service/startup manager is required. |
| N0 | Native and browser transport feasibility. | After the relevant PAL/event contracts; can proceed alongside F0. Prove the actual guest path and disclose any host bridge. |
| N1 | Guest outbound TCP. | N0 and E0 plus Emacs event integration. Numeric-address connect, send/receive, EOF, timeout, error and leak-free close. |
| N2 | DNS and controlled HTTP-to-file example. | N1 and F0. Receive/save/read inside the guest; demonstrate retained source/results only after F1. Evaluate TLS separately. |

F0 and transport research can run in parallel when the shared handle/error/event
contracts are settled. Integrate the smallest working file and network paths
before expanding the abstraction. A full Linux ABI, generic resource framework
or new Lisp wrapper family is not a prerequisite by default; decide the minimal
compatibility surface from actual runtime needs.

## Deferred scope

Services and startup managers, runtime latency optimization, preemptive
scheduling, protected processes, accounts, broad device support and a browser
product are later work. Browser transport research does not promise a browser
UI, persistent browser storage or a download workflow. Buffers should be clients
of working APIs when introduced.

Each later implementation must state exactly what was built and exercised.
Keep host tests, tiny kernel probes and full Emacs behavior distinct. Historical
implementations supply reference material, not current capability or validation.
