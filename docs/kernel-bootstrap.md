# Fixed-machine runtime bootstrap

The kernel now loads a static x86-64 ELF from the primary IDE disk and enters
its initial musl ABI stack. This is trusted ring-0 execution, without process
isolation or a Linux kernel. The first acceptance artifact is a tiny static
musl probe, not GNU Emacs. File namespaces and the Emacs runtime are integrated
separately through the typed file hooks.

## Machine and memory contract

Use QEMU's `pc` machine with exactly 1 GiB RAM, the default primary IDE disk and
COM1 at 115200 baud. This port does not discover arbitrary hardware or RAM.
The entry maps the first 1 GiB; the runtime loader reserves these ranges:

| Physical / identity virtual range | Owner |
| --- | --- |
| Below 4 MiB | BIOS tables, kernel, page tables and fixed kernel stacks |
| 4–64 MiB | Validated static ET_EXEC load segments |
| 64–68 MiB | Downward-growing runtime stack |
| 80–124 MiB | musl brk arena, including its initial inaccessible guard |
| 128–448 MiB | Reusable anonymous/private-file mappings |
| 448–512 MiB | Reclaiming kernel heap and split page tables |
| 512–1024 MiB | Immutable runtime ELF and archive staging |

An allocation failure is reported. Anonymous and file-private mappings are
zero-filled; `munmap` permits reuse. Page permissions use actual present,
write and NX bits, with supervisor write protection enabled. This enforces
ordinary protections in the shared mapping; privileged code can still change
page tables, so it establishes no security boundary. The heap uses first-fit
splitting and coalescing. Page tables remain allocated for the boot lifetime.

The ELF loader bounds the header/program table, rejects a dynamic interpreter,
validates each load extent and segment overlap, requires its entry in an
executable segment, copies file bytes and zeros BSS. Its initial stack supplies
argv, a controlled environment and the auxv needed by musl. The bootstrap
AT_RANDOM bytes are not cryptographic entropy; secure randomness is unsupported.

## Disk payload

The BIOS still loads only the small kernel. At disk byte offset 1 MiB, a 512-byte
header contains `<8sQQ` little-endian fields: magic `EMACSOS1`, ELF byte length,
and archive byte length, followed by zeros. Sector-padded ELF and archive bytes
follow. `scripts/pack-runtime.py` builds this form. IDE PIO reads have bounded
LBA/RAM extents and a timeout after initialization. The archive format consumed
by the separate filesystem integration is uncompressed newc CPIO.

## Console, time and syscall boundary

COM1 has a bounded input ring and IRQ4 readiness; IRQ0/PIT advances a monotonic
millisecond clock. Waits recheck level readiness after each interrupt. RTC date
initialization supplies a UTC epoch plus elapsed monotonic time, with one-second
initial precision. This fixed QEMU profile expects the usual 24-hour RTC mode.
IDT exception diagnostics report vector, error, RIP and CR2, then halt. Exceptions
are not isolated or recovered.

SYSCALL enters a dedicated kernel stack and translates the bounded x86-64 Linux
call subset. The initial port supports FS-base setup for libc TLS, memory,
console and terminal attributes, clocks/waits, identity and resource queries.
Timer descriptors occupy 240–255 and report real expiration/readiness; file
integration must keep its descriptors below 240. SIGACTION, alternate signal
stacks, POSIX signal timers, futex and secure randomness return ENOSYS. There
is no asynchronous signal delivery or CPU-bound Lisp cancellation guarantee.
Unsupported calls return a real negative error and print their number.
Terminal support is still a raw serial subset; full line discipline, signals,
flow control, hangup and arbitrary terminal resizing are not implemented.

`abi_files(nr, args, out)` returns whether the separate file adapter handled a
call. `fs_init(archive, size)` initializes that backend; `file_map_read` copies
its file bytes into private mappings. Weak defaults allow the kernel probe to
run before those backend objects are linked. They provide no fake filesystem.

## Validation

Run `python3 scripts/check-kernel-probe.py`. It builds a fresh static-musl probe
in the existing Alpine build image, constructs three boot disks and executes
QEMU. The normal case verifies TLS-backed libc startup, mmap, RO protection,
unmap, clock progress, sleep, output and exit. Two negative cases deliberately
write a read-only page and execute an NX page; they must report the corresponding
real page faults. Logs and a source/fork/artifact manifest are written under
`build/skeleton/`. These checks are kernel/probe acceptance only. They prove
neither Emacs startup, application behavior, persistent storage nor isolation.
