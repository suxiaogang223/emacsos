# Roadmap

Design the overall architecture before implementing the PAL or runtime port.
The current source is a minimal boot/serial scaffold with an Emacs source
submodule. Emacs, files and networking do not run here. Architecture comes
first; PAL/runtime/application/process implementations remain separate tasks.

## Staged work and dependencies

| Item | Deliverable | Gate and future validation |
| --- | --- | --- |
| B0 | Minimal BIOS/x86-64 kernel and serial scaffold. | Source/build foundation present; boot behavior remains unexecuted in this architecture work. |
| P0 | [Architecture](design.md), [runtime-port](runtime-port.md), [application/process model](process-model.md) and public interface contracts. | This documentation proposal; complete only after review/merge. No implemented PAL is implied. |
| P1 | Bootstrap runtime support over a small PAL. | After P0 in a separate implementation task: pin Emacs/musl build, derive ABI subset; implement memory, console, time/wait, read-only runtime files and static ELF/startup support. |
| E0 | Interactive Emacs and Elisp evaluation. | P1; matching executable/dump/Lisp/data manifest, controlled base initialization and actual command-loop interaction. Validate the chosen compatibility route. |
| A0 | Trusted shared-runtime Elisp application instances. | E0; distinct runs, lifecycle/context, explicit owned/borrowed/shared resources, late-event rejection, bounded messages, explicit finish/cleanup barrier, nonblocking stop/wait and truthful inspection. No forced termination or isolation. |
| F0 | Standard file APIs over volatile working files. | E0 and typed file/directory backend. Preserve ordinary saves, backups, locks and original errors; document supported options and limits. |
| F1 | Minimal persistent file backend. | F0; choose format, sync/recovery and error contracts, then prove retained data separately. No service/startup manager prerequisite. |
| N0 | Native and future browser transport feasibility. | After P0 event/ownership contracts, research may run alongside P1/F0. Prove available transport before selecting NIC/stack/bridge; no browser product is implied. |
| N1 | Guest outbound TCP. | N0 and E0; connection/readiness integration, numeric-address connect, send/receive, EOF, timeout, error and leak-free close on the supported transport. |
| N2 | DNS and controlled HTTP-to-file example. | N1 and F0. Receive/save/read in the guest; retained source/results require F1. Assess TLS/HTTPS separately. |
| K0 | Protected kernel processes and independent execution. | P1/E0 plus A0 ownership/context contracts; real address spaces, scheduling, implemented resource limits and fault/exit cleanup. Terminating a hung worker leaves kernel/main Emacs progressing. Basic files/network do not depend on K0. |

Implementation tasks remain future work and need a deliberate reviewable scope.
A0 can start with buffers/timers after E0; file/network resources are adopted
only when available. F0/F1 and N0/N1 do not depend on A0 or K0.
F0 and N0 can proceed independently once their shared handle/error/event contracts
are settled. Networking research must address both native QEMU and a future Wasm
target selected and pinned by N0 before promising equivalent guest capabilities.

## Deferred scope

The application/process model is defined now; A0 and K0 implementations remain
later stages. Service managers, durable startup snapshots, a generic resource
registry, runtime latency optimization, accounts, broad Unix subprocess support,
broad device support and a browser product remain deferred.
Standard Emacs API availability does not establish backend support. A compatibility
subset is selected from this runtime, not expanded toward all of Linux.

Each future change states what was built and exercised. Distinguish host checks,
kernel probes, emulator startup and interactive guest behavior. File sync,
physical durability and browser/host retention need separate evidence when those
backends are introduced. Historical implementations are outside this repository
and provide no present capability or validation.
