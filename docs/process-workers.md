# Bounded user-process foundation (P1 / K0a)

P1 adds small, independently mapped ring-3 ELF workers, managed through a trusted
kernel C interface. The main Emacs remains privileged, and its cooperative
application instances are unchanged. No worker is launched automatically and
there is no Elisp process-launch binding yet. This is not the roadmap's earlier
P1 runtime-bootstrap task; it is K0a from the [memory stages](memory.md).

## Execution and identity

`kernel/process.h` exposes spawn, run/resume, inspect/list, terminate, output and
reap operations. Management is serialized on a **shared supervisor kernel stack**;
it must not run directly on the main Emacs runtime stack, which is absent from a
worker's address space. A future Emacs binding must enter through an appropriate
kernel boundary. No generic Linux subprocess interface is supplied.

There are eight records. Successful spawns receive monotonically increasing
positive 64-bit PIDs starting at 2; exhausting the positive signed range rejects
further creation. Reusing a slot does not reuse its PID. Stale identities return
ESRCH, and records must be reaped before their slots are reused. Parent identity
1 denotes this trusted kernel manager, separately from the main runtime's
existing libc compatibility identity. PIDs are not persisted across boot.

Each worker owns its address space, a 64 KiB user stack, a 64 KiB supervisor
kernel stack, saved execution state and up to 4 KiB captured stdout/stderr.
The [M1 page limit](memory.md) charges user data and page tables. Kernel stacks,
records and retained output are separately bounded by the eight-slot capacity;
they are not included in the page count.

The lifecycle is READY → RUNNING → READY on yield, or RUNNING → EXITING → EXITED
on exit, user fault or invalid return context. Reclamation occurs only after
restoring the manager's address space and stack. Exit releases all private pages,
page tables and the kernel stack; the bounded exit record/output remain until
reap. Terminating a READY worker performs the same cleanup; repeating termination
of an exited worker succeeds. Reaping a live worker is rejected.

`run` resumes a READY worker and returns when it yields, exits or faults. It is
**not preemptive**: a worker that loops forever without entering a yielding/exit
path keeps the CPU. There is no guarantee that the main Emacs can regain control
or kill it in this phase. Running-worker termination, blocking/wakeup, deadline
waits and scheduling belong to P2 / K0b. User fault handling is included now so
invalid worker instructions cannot use the trusted runtime's panic/exit path.
Kernel-mode faults still panic.

## ELF contract

Spawn takes an immutable borrowed ELF byte buffer for the duration of loading,
not a path or retained pointer. The limit is 1 MiB and sixteen program headers.
The loader accepts little-endian x86-64 ET_EXEC with bounded PT_LOAD segments in
virtual 4–64 MiB. The entry must lie inside an executable load segment. It checks
file extents, alignment, permissions and page-rounded segment overlap before
mapping. Dynamic/interpreter/TLS segments and writable executable segments are
rejected. Empty stack space below the 64 KiB stack remains unmapped as a guard.

The initial stack at the top of 64–68 MiB is 16-byte aligned and supplies empty
argc/argv/env/auxv; all initial general registers are zero except the control
frame. This is a freestanding entry contract, not musl startup, an inherited
shell environment, a portable dump or an independent Emacs runtime. Fresh data
and initial x87/MMX/XMM state do not inherit another execution's contents.

## User/kernel boundary

User code/data selectors are fixed at 0x2b/0x33, after the existing TSS slots.
`int 0x80` uses a DPL3 interrupt gate and TSS.rsp0. While a worker runs, STAR,
LSTAR and FMASK select a dedicated worker SYSCALL entry; returning to the manager
restores its exact MSRs. Workers never enter the trusted Emacs syscall dispatcher.
The dedicated entry switches to the supervisor stack without reading user-stack
memory, and constructs the same kernel-owned frame as the interrupt path.

Both routes return through IRET with fixed user selectors. Every resumable return
checks an executable user RIP and writable owned memory immediately below RSP.
Arithmetic flags are retained; IF and reserved bit 1 are set, while IOPL, TF, NT,
VM, AC and DF are cleared. SYSCALL masks dangerous incoming flags before kernel
instructions execute. General registers, FS/GS bases and segment selectors,
x87/MMX/XMM registers and MXCSR survive yield/resume. The manager's corresponding
state, syscall MSRs, TSS stack and original interrupt-enable state are restored.
FSGSBASE and OSXSAVE are not enabled in this fixed profile; AVX/XSAVE state is
unsupported. See the [Intel architecture manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
for the transition and state-save mechanisms.

User copies reject overflow, supervisor/high aliases, holes, PROT_NONE and
inadequate permissions. They preflight every page before copying through the
supervisor physical alias, including for an inactive space. Copies to a user
buffer require write access throughout, so a bad suffix cannot partially change
a valid prefix. Only owned private mappings qualify. No user pointers remain
registered for asynchronous callbacks.

## Worker syscall subset

Both entry instructions use the x86-64 register convention: RAX number,
RDI/RSI/RDX/R10/R8/R9 arguments, signed result in RAX. SYSCALL clobbers RCX/R11.
The int80 route also uses this convention, not the Linux i386 ABI.

| Number | Operation and limits |
| --- | --- |
| 1 | Capture write to fd 1/2 only; requests at most 4 KiB, partial writes up to retained capacity, ENOSPC when full. There is no console or file-descriptor inheritance. |
| 9/10/11/12 | Anonymous/private mmap, mprotect, munmap and brk in this space; mmap requires fd -1/offset 0, existing fixed-profile ranges apply, W+X is rejected. Allocation failure preserves existing state. |
| 24 | Yield to the trusted manager; the next run resumes after this call. |
| 39/110 | Worker PID and kernel-manager parent identity. |
| 60/231 | Exit with signed 32-bit status. |
| 158 | Set/get FS or GS base; nonzero bases must name readable owned user memory, and get validates the output range. No ELF TLS allocation/startup. |
| 228 | CLOCK_MONOTONIC timespec output only. |

Read/close return EBADF because no ordinary descriptors are provided. Other
calls return ENOSYS. Worker memory calls cannot alter shared kernel or another
space's mappings. No file/network/timer access, signals, fork/exec, libc worker
compatibility, IPC or independent Lisp GC is implied by the syscall numbers.

## Validation

`make test-process` builds freestanding ELF fixtures and boots a dedicated probe
kernel in native QEMU using the actual process/memory/interrupt implementation.
It verifies actual CPL3 execution, both syscall gates, private same-address data,
TLS/general/FPU/SSE context, clean initial FP state, main MSR/TSS restoration,
cross-page copy failure without prefix mutation, page quotas, bounded output,
stale PIDs, table exhaustion, malformed ELF, allocation rollback and repeated
cleanup. Malicious workers attempt kernel/high-alias writes, CLI, HLT, WRMSR,
port I/O, kernel interrupt gates, FSGSBASE, NX execution, RO writes, invalid RSP
and debug/AC/DF flags; the manager continues and reclaims their resources.

Evidence is in `build/process-probe/`, with exact source, toolchain, worker and
boot-image hashes, commands and serial output. The probe stops after PASS; it is
not the production Emacs image. `make check` includes this native probe and the
existing Emacs/GC/file/application, DATA recovery and networking acceptance.

`make test-browser-process` runs that same probe disk through the pinned
QEMU/Wasm and actual page/serial stack, recording fetched-image identity and
results separately. It substitutes the probe disk only inside its test browser;
it does not change the served production image. Optional browser-driver setup
is in [browser checks](web-browser-checks.md). Production Emacs browser regression
is still a separate acceptance command.
