# Emacs-first Linux exploration

## Question and proposed direction

Can a small Linux system deliver the Emacs-first interaction model while
letting the project concentrate on system Elisp and application workflows?
This branch provides a separately runnable native-QEMU prototype to test that
question. It is an experiment, not an adopted architecture migration.

Linux owns memory, device drivers, filesystems, networking and OS processes.
A small init owns boot, orphan reaping and shutdown. Emacs owns the user-facing
environment, with ordinary Elisp file, network and process APIs. Necessary
external programs can execute behind buffers without adding another desktop.

## Prototype choice

Use Alpine's prebuilt virtual-machine kernel, musl, BusyBox and terminal
Emacs with GnuTLS. This reduces the time and disk cost of answering the
runtime question. It does not establish Buildroot integration or a minimal
possible image. Buildroot remains a candidate for a subsequent controlled
image build; its [manual](https://buildroot.org/downloads/manual/manual.html)
describes generating a kernel, toolchain and root filesystem.

The experiment pins every installed package and records actual build inputs.
Its distribution Emacs is 30.1; a follow-up must build the pinned 30.2 fork
with the required Linux features before claiming runtime parity. No fork
source or parent gitlink changes are needed for this prototype. System policy
continues to belong in the parent repository.

## Responsibility and failure boundaries

| Concern | Linux route | Remaining project work |
| --- | --- | --- |
| Ordinary files | Linux ext4 plus standard Emacs APIs | Data layout, upgrades, recovery and retention policy |
| Network/TLS | Linux sockets, resolver, GnuTLS and Emacs clients | Configuration, credentials, trust and usable application flows |
| External jobs | OS subprocesses controlled through Emacs | Output presentation, job trees, cancellation and lifecycle policy |
| Shared Lisp execution | One Emacs command loop and heap | Async design; non-returning Lisp can still freeze the interface |
| Failure containment | An external process has its own address space | No sandbox or separate-user isolation is provided by this experiment |
| Startup/shutdown | BusyBox init plus a small Emacs launcher | Recovery UI, crash restart and failed-storage behavior |
| Browser deployment | Not exercised by this prototype | QEMU/Wasm boot, transport, storage export and retention acceptance |

Stopping the tested shell process is not a process-tree cancellation contract.
The existing cooperative application manager is not integrated in this initial
prototype. External-worker termination does not make shared Lisp applications
independently schedulable or forcibly cancellable.

## Acceptance and evidence

The [run instructions](../experiments/linux/README.md) specify a two-boot
interactive acceptance procedure and the generated evidence. Build success,
boot success and those functional assertions are distinct results. The data
retention claim is limited to orderly shutdown and reopening the same native
QEMU disk; browser storage and power-loss recovery remain open.

The distribution's asynchronous HTTPS path remained in `connect` state in
local-fixture experiments, including the timeout-enabled URL helper. The
synchronous helper without its optional timeout completed the same verified
request. Functional acceptance is explicitly limited to that synchronous
network path. Diagnose the asynchronous integration with the pinned runtime
before selecting this as a production base; the root cause is not established.

## Decision after the experiment

Prefer this route for a usable Emacs-first environment if the guest checks
pass and the resulting image cost is acceptable. Retain the own-kernel port
as a separately scoped research result. Before changing the selected
architecture, decide:

1. Whether native QEMU or browser QEMU/Wasm is the primary delivery target.
2. Whether to retain Alpine packaging or build a smaller image with Buildroot.
3. How to build and validate the pinned Emacs fork with Linux/TLS/process support.
4. How saved data survives updates, failures and browser sessions.
5. Which user workflows define the next acceptance target.

Migration would update design, roadmap and source ownership together after
these choices; existing own-kernel acceptance results do not transfer to Linux.
