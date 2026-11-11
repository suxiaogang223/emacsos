# Roadmap

Design the overall architecture before implementing the PAL or runtime port.
The current source is a minimal boot/serial scaffold with an Emacs source
submodule. Emacs, files and networking do not run here. This planning change
performs documentation/static checks only, with no build or boot execution.

## Staged work and dependencies

| Item | Deliverable | Gate and future validation |
| --- | --- | --- |
| B0 | Minimal BIOS/x86-64 kernel and serial scaffold. | Source/build foundation present; boot behavior remains unexecuted in this architecture work. |
| P0 | [Architecture](design.md), [runtime-port](runtime-port.md) and public interface contracts. | This documentation proposal; complete only after review/merge. No implemented PAL is implied. |
| P1 | Bootstrap runtime support over a small PAL. | After P0 and separate implementation authorization: pin Emacs/musl build, derive ABI subset; implement memory, console, time/wait, read-only runtime files and static ELF/startup support. |
| E0 | Interactive Emacs and Elisp evaluation. | P1; matching executable/dump/Lisp/data manifest, controlled base initialization and actual command-loop interaction. Validate the chosen compatibility route. |
| F0 | Standard file APIs over volatile working files. | E0 and typed file/directory backend. Preserve ordinary saves, backups, locks and original errors; document supported options and limits. |
| F1 | Minimal persistent file backend. | F0; choose format, sync/recovery and error contracts, then prove retained data separately. No service/startup manager prerequisite. |
| N0 | Native and future browser transport feasibility. | After P0 event/ownership contracts, research may run alongside P1/F0. Prove available transport before selecting NIC/stack/bridge; no browser product is implied. |
| N1 | Guest outbound TCP. | N0 and E0; connection/readiness integration, numeric-address connect, send/receive, EOF, timeout, error and leak-free close on the supported transport. |
| N2 | DNS and controlled HTTP-to-file example. | N1 and F0. Receive/save/read in the guest; retained source/results require F1. Assess TLS separately. |

Implementation tasks remain future work and need a deliberate reviewable scope;
merging design does not authorize automatic expansion into a full runtime port.
F0 and N0 can proceed independently once their shared handle/error/event contracts
are settled. Networking research must address both native QEMU and the pinned
future Wasm target before promising equivalent guest capabilities.

## Deferred scope

Service managers, durable startup snapshots, a generic resource registry,
preemption, runtime latency optimization, protected workers, accounts, external
Unix processes, broad device support and a browser product are deferred.
Standard Emacs API availability does not establish backend support. A compatibility
subset is selected from this runtime, not expanded toward all of Linux.

Each future change states what was built and exercised. Distinguish host checks,
kernel probes, emulator startup and interactive guest behavior. File sync,
physical durability and browser/host retention need separate evidence when those
backends are introduced. Historical implementations are outside this repository
and provide no present capability or validation.
