# Roadmap

The selected system is Linux plus the pinned GNU Emacs runtime. Development
focuses on the browser computer and Emacs user workflows.

## Current implementation and acceptance gates

| Area | Implementation | Required evidence |
| --- | --- | --- |
| System image | Pinned Alpine kernel/userspace plus compiled Emacs 30.2; one default build | Manifest-matched actual guest boot |
| Native reference | QEMU, ordinary Emacs files, OS subprocesses and GnuTLS | Two-boot guest acceptance; public DNS/HTTPS is an optional additional check |
| Browser computer | QEMU/Wasm, xterm.js, power/restart/fullscreen, files and Eshell keys | Browser acceptance and screenshots at multiple viewport widths |
| Browser workspace | A writable ext4 disk owned by the current machine | Save and reload inside the same browser session |
| Browser retention | Not implemented | Guest sync/unmount, storage/export, interruption and reopen tests |
| Browser networking | Disabled | Real transport, connection lifecycle, DNS/TLS and error acceptance |

Build, native guest execution and browser functional acceptance are separate
claims. Generated reports identify the exact source and actual artifacts.
Old custom-kernel milestones and application-manager contracts no longer
define the selected system; their code and documents have been removed.

## Priorities

1. **Web experience:** clear download/startup states, stable terminal sizing,
   keyboard/focus, useful direct actions and honest session-lifetime messages.
2. **Workspace continuity:** define how user files leave the guest and return
   in a later machine, then validate interruption and storage errors.
3. **Emacs workflows:** files, Eshell, program evaluation and useful tools,
   using standard APIs and external workers where appropriate.
4. **Browser transport:** select a supported network bridge and verify actual
   guest traffic. Validate asynchronous TLS separately on the pinned runtime.
5. **Distribution:** reconstruct the prebuilt emulator's source/compiler and
   complete corresponding-source/license materials, then optimize download
   size and startup time from measurements.

## Data-disk gate

Before adding disk import/export or persistent storage, define format and size
validation, who owns the bytes, boot attachment versus hot attach, guest
mount/sync/unmount behavior, dirty/error reporting and the retained destination.
Acceptance must reopen a new machine and compare actual saved content. A file
picker or exported byte array alone does not establish guest synchronization.
