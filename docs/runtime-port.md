# First runtime-port contract

The architectural choice is a static GNU Emacs 30.2/musl runtime using a bounded
Linux x86-64 ABI adapter over our typed PAL. The [MVP](mvp.md) exercises this
route for one fixed QEMU machine; it is not
general Linux compatibility. [The architecture](design.md) is the canonical
layer/ownership description. P0 documents the contracts; P1 implements bootstrap
support and E0 proves the runtime.

## Current source evidence

- `kernel/entry.asm`, `hardware.c` and `memory.c` establish the fixed memory
  profile, real page permissions, serial interrupts and PIT/RTC time.
- `kernel/loader.c` validates and loads the supported static ELF, builds its
  initial stack and supplies controlled dump/environment startup.
- `kernel/runtime.c` handles the exercised ABI subset, real timer descriptors
  and readiness; unavailable calls return errors.
- `kernel/files.c` and `file_abi.c` implement typed namespace/file operations
  and ABI translation. See [file contracts](files.md).
- The fork pin remains `73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5` (GNU Emacs
  30.2 plus provenance only). No runtime source patches are needed for this MVP.

[Kernel bootstrap](kernel-bootstrap.md) records concrete ranges, startup and
unsupported semantics. No asynchronous signal delivery, secure entropy, dynamic
linker or external subprocess execution is claimed.

The [runtime build](runtime-build.md) now pins a terminal-only static musl
profile and packages matching artifacts. Compilation and artifact inspection do
not prove guest compatibility; kernel probes and integrated guest checks retain
separate evidence. The requirements below also derive from upstream source
inspection.
Running Emacs requires substantially more than dispatching syscalls.

P1 uses the terminal-only static build with interpreted/bytecompiled Lisp. GUI,
native compilation, Lisp threads, TLS/HTTPS and external helpers are deferred; audit
exact configure flags and remaining call paths rather than assuming all
unsupported function names disappear.

## Compatibility surface to establish

| Area | Required decision and behavior |
| --- | --- |
| Executable/startup | Supported static ELF form and segments; initial stack/alignment, argv/env/auxv, libc entry, thread-local storage (TLS) and errno requirements. No dynamic linker or exec subprocess promise. |
| Memory | RAM discovery/bounds, reserved ranges, runtime maps/heap, alignment, overflow checks and exact release/lifetime rules. Define protection behavior; reject unsupported semantics. |
| Console | Serial input/output, termios/ioctl subset actually needed by terminal Emacs, short I/O and readiness. |
| Time/wait | Monotonic clock, deadline units and readiness integration compatible with Emacs's configured wait path. Audit timer descriptors/signals if the selected build uses them. |
| Runtime files | Path/metadata/read/seek/map operations required for executable, matching dump, Lisp and data; own backing storage lifetimes explicitly. |
| Error/cleanup | Defined unsupported-operation errors, errno translation, partial progress, interrupted waits, descriptor ownership/duplication and final close. |
| Other libc needs | Audit startup identity, randomness, signal setup and any other requests made by the selected static build; implement required semantics or constrain the build explicitly. |

This is a requirements inventory, not a chosen syscall-number list. Derive the
initial supported subset from the selected Emacs/musl build and its actual
startup/interaction paths. A successful placeholder for memory protection,
signal delivery or descriptor behavior is not acceptable evidence. A supported
no-op needs a deliberate bounded contract that is sufficient for this port.

[pdumper.c](https://github.com/suxiaogang223/emacs/blob/73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5/src/pdumper.c) reserves/maps dump regions and validates
header/fingerprint and exact reads. Its contiguous-loading strategy is selected
at compile time; failed mmap does not universally fall back to heap loading.
Support the selected anonymous reservation, fixed file mappings, MAP_PRIVATE,
zero-fill, permissions and failure rollback semantics, or fail the operation.
Do not report successful memory protection without enforcing it.

[sysdep.c](https://github.com/suxiaogang223/emacs/blob/73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5/src/sysdep.c) and [term.c](https://github.com/suxiaogang223/emacs/blob/73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5/src/term.c) require more
than a banner: audit terminal attributes, ISIG/C-g handling, window size and
terminal capability data. [process.c](https://github.com/suxiaogang223/emacs/blob/73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5/src/process.c) integrates waits
with terminal/network readiness, timers and redisplay; [atimer.c](https://github.com/suxiaogang223/emacs/blob/73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5/src/atimer.c)
needs real timer/signal behavior or an explicit targeted adaptation. Wall-clock
time and entropy quality must be stated honestly; neither a fake clock nor a
constant random result supplies transport encryption or strong entropy.

Musl design references are [x86-64 syscalls](https://git.musl-libc.org/cgit/musl/tree/arch/x86_64/syscall_arch.h),
[libc startup](https://git.musl-libc.org/cgit/musl/tree/src/env/__libc_start_main.c)
and [thread-local storage initialization](https://git.musl-libc.org/cgit/musl/tree/src/env/__init_tls.c).
These upstream HEAD links are not a pinned build; select a revision/configuration
and derive its actual syscall closure before implementation.
Disabling Lisp threads or TLS/HTTPS does not eliminate libc thread-local storage
and errno initialization from the runtime startup contract.

The ABI adapter owns Linux-shaped argument/error translation and descriptor
mapping. The PAL owns backend capability semantics and must not depend on Lisp
objects or buffers. Musl remains a C runtime dependency; it does not supply the
guest's allocator mappings, devices, wait mechanism or files. Direct Emacs C
adaptation and replacement libc remain alternatives if E0 exposes an unsound
assumption, not simultaneous first-port implementation tracks.

## Bootstrap files and writable files

A small read-only runtime image must be available before `load` or dump startup
can work, including the selected Lisp/bytecode, DOC, charset and terminfo paths.
Choose an encoding, validate its bounds and route only its documented
namespace. Its memory/map lifetime must outlive any borrowed runtime access.
Select which files are loaded by the kernel and which Emacs opens through the
adapter; do not require F0 writable files just to bootstrap Emacs.

F0 supplies explicitly volatile working files with ordinary Emacs file
behavior. Directory enumeration, file position, creation flags, metadata,
locking fallback requirements and rename semantics need concrete contracts.
[filelock.c](https://github.com/suxiaogang223/emacs/blob/73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5/src/filelock.c) falls back to regular-file locks only for
specific errors; indiscriminate EROFS or fake success changes normal behavior.
[fileio.c](https://github.com/suxiaogang223/emacs/blob/73cefc0fc8dad3e6709c2d1070b6cfe93102b3a5/src/fileio.c) propagates write/fsync/final-close failures, but
`write-region-inhibit-fsync` normally defaults to true: ordinary save is not a
durability boundary. No old save fixes or validated save contract is inherited
by the clean fork. F1 uses parent-owned startup policy to enable file sync,
and adds a littlefs DATA backend at `/work` without changing Emacs sources.
See [storage](storage.md) for sync, recovery and browser-retention boundaries;
close alone is not a durable-save contract.

## Event and failure boundary

Integrate console and future connection readiness into the command loop's
actual wait path. Check/rearm/recheck must avoid lost wakeups. Use monotonic
absolute deadlines with explicit timeout and interruption results. A callback
that blocks indefinitely can still freeze interaction; an event API does not
create another execution environment.

Descriptor duplication must alias the same open description and offset; separate
opens remain independent. Resource identity must survive path changes rather
than using a path string as identity. Reject stale readiness after close/reuse.

The first port runs one trusted Emacs, without promised process/thread isolation
or external subprocess execution. The [application/process model](process-model.md)
adds A0 instance tracking (the MVP subset is implemented) and future K0 kernel
execution; neither is required by the initial PAL or basic file/network APIs. ABI calls needed only by unsupported
features must fail clearly. Network process objects remain connections in the same
runtime. Even a connection created with `:nowait` does not make every later
`process-send-string` nonblocking; document the supported send/progress behavior.
Correct cleanup implies neither protection nor rollback of completed I/O.

## E0 evidence

The [build and guest acceptance records](mvp.md) bind parent/fork commits, build
options, compiler/libc identity and hashes of executable, dump, Lisp/runtime data
and boot image through their manifests. Verify sufficient mappings and reserved ranges,
artifact consistency, controlled initialization, actual terminal interaction and
Elisp evaluation. Trace unsupported calls and real errors rather than broadening
compatibility by assumption.

Evidence must distinguish host builds, static/component checks, emulator
startup and interactive guest behavior.
