# Design

The goal is an operating environment in which users write Elisp programs,
operate files and communicate over the network through standard Emacs APIs.
Buffers and Eshell will consume those APIs rather than introduce separate
implementations.

## Architecture

```text
Elisp programs and interactive Emacs tools
                   |
         standard GNU Emacs APIs
                   |
  Emacs runtime + libc/compatibility adapter
                   |
      small platform abstraction layer
                   |
         C kernel and device backends
```

The platform abstraction layer (PAL) gives the runtime a small, explicit set of
host capabilities. It must first cover console I/O, memory, time and waiting;
file and network operations then extend it with concrete typed contracts.
A file and a connection can share lifetime and byte-I/O conventions without
forcing directory operations, rename or connection setup into one generic call.

The libc/compatibility adapter remains necessary. GNU Emacs and its C library
expect operating-system behavior beyond an abstract read/write interface.
Choose explicitly whether the first port uses a minimal Linux syscall subset,
a libc port, or a combination. Account for startup, executable loading, memory,
terminal behavior and event waits. A PAL does not make those requirements
vanish, and none of these adapters is implemented in this baseline.

## Responsibilities

| Layer | Responsibility |
| --- | --- |
| Elisp | Public programming interfaces, programs and interaction. Prefer upstream Emacs behavior. |
| Emacs fork and compatibility adapter | Runtime/platform integration, original Emacs error handling and event dispatch. |
| PAL | Typed capability operations, ownership, errors, blocking/readiness and resource cleanup contracts. |
| C kernel/backends | Boot, interrupts and actual memory/device operations; enforce bounds that are implemented. |

Use concrete operations and stable contracts before considering a resource
registry, plugin framework or generalized resource filesystem. Platform handles
are not a security or isolation boundary by themselves.

## Present baseline

The parent source contains a minimal BIOS loader, x86-64 entry, C kernel/linker
script and build/launcher tools. Its intended observable behavior is serial
output. Emacs is a source dependency only. This reset does not claim boot
execution, interactive Emacs, file operations, networking or persistence.

The initial development target is a fixed QEMU machine. Avoid broad hardware
support, accounts, service orchestration, schedulers, protected processes or a
browser product before the basic programming/file/network path works. Future
concurrency or isolation needs separate mechanisms and evidence; the architecture
alone promises neither.
