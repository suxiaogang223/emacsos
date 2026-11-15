# Memory ownership and process foundations

M1 separates physical frames from runtime virtual addresses and adds kernel-only
address-space objects. The production runtime still runs one trusted ring-0
Emacs. Creating or activating an address space does not create a process,
provide scheduling or establish isolation. The application manager continues to
manage cooperative instances in the shared Emacs runtime.

## Fixed-machine allocation

The supported machine remains QEMU `pc` with 1 GiB RAM. Physical frames in
128–448 MiB form a pool of 81,920 4 KiB pages. Both private data and dynamically
allocated page tables use this pool. Kernel/bootstrap tables and stacks, static
ELF segments, the runtime stack, the kernel heap and immutable image staging
are reserved outside it. No hardware memory discovery or paging to disk exists.

A supervisor-only, non-executable direct map at `0xffff800000000000` covers the
fixed physical RAM. The kernel uses it to access frames independently of the
active low virtual mappings. The bootstrap removes the pool's low identity
mapping before allocation, and removes the brk identity mapping before growth.
Existing Emacs virtual ranges remain unchanged: brk at 80–124 MiB and anonymous
or private-file mappings at 128–448 MiB. Both now acquire physical backing from
the same pool; a virtual address no longer determines its physical frame.

The kernel heap remains a separate 64 MiB first-fit allocator with splitting
and coalescing. Address-space metadata and transient mapping plans use this
heap; they are released after failure or destruction but are outside page quotas.
The direct map is a privileged access mechanism, not a security boundary against
the current privileged runtime.

## Address-space operations

`kernel/memory.h` provides serialized kernel operations, not an Elisp memory API:

- Create an empty private low address space with a page limit; it initially owns
  three page-table frames. Kernel code/stacks, heap, staging and the high direct
  map remain shared supervisor mappings.
- Map, protect, unmap and grow/shrink brk in a specified space. Private fixed
  mappings can use 4–448 MiB except the reserved brk interval; automatic mmap
  searches 128–448 MiB. The bootstrap retains its loader/stack exceptions.
- Activate a valid live space by loading its actual CR3. Callers must execute on
  shared kernel code and stacks and serialize these operations. There is no
  interrupt-driven scheduler or concurrent page-table mutation.
- Inspect owned data/table pages and the limit, or inspect the global pool.
- Destroy an inactive private space and reclaim its owned data, page tables and
  metadata. Destroying the active or bootstrap space is rejected.

Fresh frames are zero-filled before exposure. PROT_NONE mappings retain their
ownership until unmap. Permission changes preserve physical backing and reject
holes before changing the mapped prefix. Empty owned leaf tables are reclaimed.
Allocation failure rolls back newly acquired frames and tables without changing
existing mappings or bytes. Fixed replacement reuses old backing so a successful
replacement does not need temporary quota headroom, and clears its contents
only after all required allocations succeed. Brk failure preserves the old break;
regrowth clears newly exposed bytes, including retained partial pages.

Quotas count actual owned **data plus page-table frames**. Borrowed bootstrap
mappings, shared mappings and kernel-heap metadata are excluded. They are not
whole-process RSS, reserved virtual address totals or Lisp object sizes. Global
pool exhaustion and per-space quota exhaustion both reject allocation. The
current bootstrap has the pool capacity as its limit. No public quota-setting
API, process identity or per-application hard quota is introduced here.

## Garbage collection boundary

Emacs and musl continue to own object allocation and garbage collection. Elisp
uses normal strings, lists, vectors and buffers; it does not allocate or free
physical pages. GC can free objects for runtime reuse without returning pages
through munmap or brk. A stable page count after GC therefore does not by itself
prove a leak. Lisp live objects, allocator-retained memory and kernel page
charges describe different quantities.

`gc-cons-threshold` and `gc-cons-percentage` control collection triggers, not OS
memory limits; buffer text is not included in the threshold counter. Shared Lisp
references also prevent reliable hard per-application heap accounting in one
Emacs. Future hard limits apply to an independent runtime's address space.
The kernel must not invoke GC from an IRQ or an arbitrary syscall stack, keep
unmanaged Lisp pointers or rely on runtime finalizers for process reclamation.
Allocation failure does not guarantee recoverable Lisp OOM; that requires
separate runtime failure-path acceptance.

## Implementation stages

| Stage | Scope and gate |
| --- | --- |
| M1 | Physical frames, independent mappings, page charges/limits, rollback and reclamation. Real kernel MMU probes plus existing runtime regression. |
| K0a | Bounded static ELF workers, PID/generation, ring-3 entry/return, validated user copies, per-worker resources and complete execution context including TLS and FPU/SSE. |
| K0b | Preemption, blocking/wakeup, worker fault containment, termination/wait and cleanup from a safe kernel context. A busy or faulty worker must leave the kernel and main Emacs progressing. |
| K0c | Emacs start/list/inspect/terminate/wait interface and manager views with separate application IDs and kernel PIDs. Independent Emacs workers need their own GC and OOM acceptance. |

K0a–K0c are planned, not implemented. The current global syscall stacks,
descriptors, timers and exception handling must change before protected worker
execution. Switching the page-table root alone does not validate user pointers,
contain faults or save execution state. Swap, COW, SMP and general Unix fork/exec
remain deferred. DNS and further network expansion are deferred while this work
proceeds.

## Validation

`make test-memory` builds and executes four small kernels in native QEMU using
the production memory implementation. The normal probe switches real CR3 roots,
checks different bytes at the same virtual address, allocation and table failure
rollback, exact quotas, PROT_NONE unmap, repeated destruction/zero reuse,
byte-granular brk regrowth, overflow bounds and global exhaustion/reclamation.
Three negative probes check real write-protection and NX page faults, including
the high direct map. Evidence under `build/memory-probe/` records the source,
toolchain, image hashes, commands and transcripts.

`make check` includes these probes, static-musl acceptance, actual Emacs
allocation/GC and application checks, DATA reboot/recovery and native networking.
Browser acceptance is separate. Kernel probes do not establish ring-3 isolation,
process scheduling, Emacs OOM recovery or independent Lisp workers.
