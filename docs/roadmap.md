# Roadmap

The [first MVP](mvp.md) implements the selected runtime route: interactive GNU
Emacs, volatile file operations and cooperative application instances. Its
acceptance uses the actual fixed-machine QEMU guest. Broader stages below remain
contracts rather than implied capabilities.

## Current status

| Item | Status | Evidence and remaining scope |
| --- | --- | --- |
| B0 / P0 | Implemented and reviewed | BIOS/x86-64 boot plus architecture/source contracts. |
| P1 / E0 | MVP implemented | Pinned static runtime, real loader/mappings/console/time/wait, matching artifacts; interactive input, evaluation and timer acceptance. |
| F0 | MVP implemented subset | Volatile files, normal save/backup/lock/errors and directory operations; bounded unsupported options documented in files.md. |
| A0 | MVP subset | Host ERT and guest two-instance manager lifecycle; message queues, completion subscriptions and deadline waits remain open. |
| F1 / N0–N2 / K0 | Not implemented | Persistence, transport/networking and independent protected execution require separate decisions and evidence. |

## Staged work and dependencies

| Item | Deliverable | Gate and future validation |
| --- | --- | --- |
| B0 | Minimal BIOS/x86-64 kernel and serial scaffold. | Boot executed in kernel probes and integrated guest acceptance. |
| P0 | [Architecture](design.md), [runtime-port](runtime-port.md), [application/process model](process-model.md) and public interface contracts. | Architecture merged; current implementation follows the bounded MVP subset. |
| P1 | Bootstrap runtime support over a small PAL. | After P0 in a separate implementation task: pin Emacs/musl build, derive ABI subset; implement memory, console, time/wait, read-only runtime files and static ELF/startup support. |
| E0 | Interactive Emacs and Elisp evaluation. | P1; matching executable/dump/Lisp/data manifest, controlled base initialization and actual command-loop interaction. Validate the chosen compatibility route. |
| A0 | Trusted shared-runtime Elisp application instances. | E0; distinct runs, lifecycle/context, explicit owned/borrowed/shared resources, late-event rejection, bounded messages, explicit finish/cleanup barrier, nonblocking stop/wait and truthful inspection. No forced termination or isolation. |
| F0 | Standard file APIs over volatile working files. | E0 and typed file/directory backend. Preserve ordinary saves, backups, locks and original errors; document supported options and limits. |
| F1 | Minimal persistent file backend. | F0; choose format, sync/recovery and error contracts, then prove retained data separately. No service/startup manager prerequisite. |
| N0 | Native and future browser transport feasibility. | After P0 event/ownership contracts, research may run alongside P1/F0. Prove available transport before selecting NIC/stack/bridge; no browser product is implied. |
| N1 | Guest outbound TCP. | N0 and E0; connection/readiness integration, numeric-address connect, send/receive, EOF, timeout, error and leak-free close on the supported transport. |
| N2 | DNS and controlled HTTP-to-file example. | N1 and F0. Receive/save/read in the guest; retained source/results require F1. Assess TLS/HTTPS separately. |
| K0 | Protected kernel processes and independent execution. | P1/E0 plus A0 ownership/context contracts; real address spaces, scheduling, implemented resource limits and fault/exit cleanup. Terminating a hung worker leaves kernel/main Emacs progressing. Basic files/network do not depend on K0. |

Further implementation needs a deliberate reviewable scope.
A0 can start with buffers/timers after E0; file/network resources are adopted
only when available. F0/F1 and N0/N1 do not depend on A0 or K0.
F0 and N0 can proceed independently once their shared handle/error/event contracts
are settled. Networking research must address both native QEMU and a future Wasm
target selected and pinned by N0 before promising equivalent guest capabilities.

## Deferred scope

The application/process model is defined; A0 has an MVP subset and K0 remains
a later stage. Service managers, durable startup snapshots, a generic resource
registry, runtime latency optimization, accounts, broad Unix subprocess support,
broad device support and a browser product remain deferred.
Standard Emacs API availability does not establish backend support. A compatibility
subset is selected from this runtime, not expanded toward all of Linux.

Each future change states what was built and exercised. Distinguish host checks,
kernel probes, emulator startup and interactive guest behavior. File sync,
physical durability and browser/host retention need separate evidence when those
backends are introduced. Historical implementations are outside this repository
and provide no present capability or validation.
