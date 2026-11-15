# Roadmap

The [first MVP](mvp.md) established interactive GNU Emacs, file operations and
cooperative application instances. Littlefs DATA now replaces its temporary RAM
file backend. Its
acceptance uses the actual fixed-machine QEMU guest. Broader stages below remain
contracts rather than implied capabilities.

## Current status

| Item | Status | Evidence and remaining scope |
| --- | --- | --- |
| B0 / P0 | Implemented and reviewed | BIOS/x86-64 boot plus architecture/source contracts. |
| P1 / E0 | MVP implemented | Pinned static runtime, real loader/mappings/console/time/wait, matching artifacts; interactive input, evaluation and timer acceptance. |
| F0 | MVP implemented subset | Standard file APIs, normal save/backup/lock/errors and directory operations; bounded unsupported options documented in files.md. |
| A0 | MVP subset | Host ERT and guest two-instance manager lifecycle; message queues, completion subscriptions and deadline waits remain open. |
| Browser presentation | Implemented | Pinned QEMU/Wasm boots the same MVP image; xterm.js supplies the raw serial terminal and retro computer controls. See web.md for browser evidence and limits. |
| F1 | Implemented subset | Littlefs is the only writable backend for `/etc`, `/home`, `/tmp`, `/var`; native sync/reboot acceptance and separate browser checkpoint/import/export acceptance; see storage.md. |
| N0 | Transport proved | Actual native and pinned-Wasm Ethernet/ARP, including the explicit browser host bridge. |
| N1 | Implemented subset | NE2000 ISA/lwIP numeric IPv4 TCP; native Emacs echo, half-close, timers, refusal and timeout pass. Separate pinned-Wasm TCP, file save, checkpoint and restart acceptance passes; see network.md. |
| N2a / N2b | Native numeric HTTP/file loop validated; DNS deferred | Actual native HTTP download/hash/save/manual load and fresh-boot source retention pass. Browser TCP/file retention has separate acceptance; DNS and guest TLS are unimplemented. |
| M1 | Implemented foundation | Kernel-only physical pages/address spaces, table-inclusive page limits and reclamation; trusted Emacs remains ring 0. See memory.md for scope and validation. |
| K0 | Planned after M1 | Protected workers require ring-3 execution, complete contexts, resource ownership, user-copy validation, scheduling and fault containment. |

## Staged work and dependencies

| Item | Deliverable | Gate and future validation |
| --- | --- | --- |
| B0 | Minimal BIOS/x86-64 kernel and serial scaffold. | Boot executed in kernel probes and integrated guest acceptance. |
| P0 | [Architecture](design.md), [runtime-port](runtime-port.md), [application/process model](process-model.md) and public interface contracts. | Architecture merged; current implementation follows the bounded MVP subset. |
| P1 | Bootstrap runtime support over a small PAL. | After P0 in a separate implementation task: pin Emacs/musl build, derive ABI subset; implement memory, console, time/wait, read-only runtime files and static ELF/startup support. |
| E0 | Interactive Emacs and Elisp evaluation. | P1; matching executable/dump/Lisp/data manifest, controlled base initialization and actual command-loop interaction. Validate the chosen compatibility route. |
| A0 | Trusted shared-runtime Elisp application instances. | E0; distinct runs, lifecycle/context, explicit owned/borrowed/shared resources, late-event rejection, bounded messages, explicit finish/cleanup barrier, nonblocking stop/wait and truthful inspection. No forced termination or isolation. |
| F0 | Standard file APIs over a typed working-file backend. | E0 and typed file/directory backend. Preserve ordinary saves, backups, locks and original errors; document supported options and limits. |
| F1 | Minimal persistent file backend. | F0; choose format, sync/recovery and error contracts, then prove retained data separately. No service/startup manager prerequisite. |
| N0 | Native and browser guest-network transport feasibility. | After P0 event/ownership contracts, research may run alongside file work. Prove available transport before selecting NIC/stack/bridge; a working browser console does not prove guest networking. |
| N1 | Guest outbound TCP. | N0 and E0; connection/readiness integration, numeric-address connect, send/receive, EOF, timeout, error and leak-free close on the supported transport. |
| N2a | Controlled numeric HTTP-to-file example. | N1 and F0. Receive/save/read in actual guest Emacs; retained source/results require F1 and separate reboot/browser-checkpoint evidence. |
| N2b | DNS. | After numeric TCP: select resolution behavior and validate actual guest names, errors and deadlines. DNS is not implied by HTTP over a numeric address. |
| M1 | Physical frame and address-space foundation. | Preserve the runtime layout, charge owned data/table pages, roll back failed allocation and reclaim inactive spaces. Separate real MMU probes from Emacs GC/regression acceptance; see memory.md. |
| K0 | Protected kernel processes and independent execution. | P1/E0 plus A0 ownership/context contracts; real address spaces, scheduling, implemented resource limits and fault/exit cleanup. Terminating a hung worker leaves kernel/main Emacs progressing. Basic files/network do not depend on K0. |

Memory/process work now takes priority over DNS and further network expansion.
The sequence is M1, bounded user workers, preemption/fault cleanup, then Emacs
process management; see [memory stages](memory.md).
Further implementation needs a deliberate reviewable scope.
A0 can start with buffers/timers after E0; file/network resources are adopted
only when available. F0/F1 and N0/N1 do not depend on A0 or K0.
F0 and N0 can proceed independently once their shared handle/error/event contracts
are settled. N0 has now selected NE2000 ISA and lwIP after proving native and
pinned-Wasm frame transport. [Networking](network.md) records the automatic
browser bridge and distinct TCP/Emacs/browser acceptance gates.

## DATA disk boundaries

F1 uses a fixed 64 MiB littlefs DATA disk separately from SYSTEM. Native startup
creates an initialized disk only when missing. Browser startup supplies an empty
disk automatically, then restores completed checkpoints for the same origin.
All four writable directories share this volume; missing or rejected DATA
enters read-only recovery. There is no writable RAM filesystem or fallback.
Optional import/export moves complete DATA images. See [storage](storage.md).

Guest mount validation precedes promotion of an imported image. COM2 freeze,
littlefs sync and ATA flush delimit the consistent copy; a completed IndexedDB
transaction establishes browser retention. Failed mount or checkpoint operations
preserve the previous retained disk and expose the error. Host tests, native
reboot acceptance and actual pinned-Wasm/browser acceptance are separate gates.

Future storage expansion includes broader disk profiles, capacity management,
additional persistent directories and stronger recovery tooling. Hot-swap,
partitions, disk growth, encryption and automatic user startup remain outside
this implementation.

## Deferred scope

The application/process model is defined; A0 has an MVP subset and K0 remains
a later stage. Service managers, durable startup snapshots, a generic resource
registry, runtime latency optimization, accounts, broad Unix subprocess support,
and broad device support remain deferred.
Standard Emacs API availability does not establish backend support. A compatibility
subset is selected from this runtime, not expanded toward all of Linux.

Each future change states what was built and exercised. Distinguish host checks,
kernel probes, emulator startup and interactive guest behavior. File sync,
physical durability and browser/host retention need separate evidence when those
backends are introduced. Historical implementations are outside this repository
and provide no present capability or validation.
