# Roadmap

The selected system is Linux plus the pinned GNU Emacs runtime. Development
focuses on the browser computer and Emacs user workflows.

## Current implementation and acceptance gates

| Area | Implementation | Required evidence |
| --- | --- | --- |
| System image | Fixed-machine Linux build, minimal Alpine userspace and compiled Emacs 30.2; one default build | Manifest-matched actual guest boot |
| Native reference | QEMU, ordinary Emacs files, OS subprocesses and GnuTLS | Two-boot guest acceptance; public DNS/HTTPS is an optional additional check |
| Browser computer | QEMU/Wasm, xterm.js, power/restart/fullscreen, files and Eshell keys | Browser acceptance and screenshots at multiple viewport widths |
| Text workflows | Home buffer, Dired, Org notebook/daily agenda, Eshell, Lisp example and Info | Actual keyboard navigation, file edits, agenda content and command evaluation in the guest |
| Project website | Lightweight introduction and real workflow gallery; computer at `try/` | No runtime loading on the homepage, working project-subpath links and responsive screenshots |
| Browser workspace | A writable ext4 disk owned by the current machine | Save and reload inside the same browser session |
| Public distribution | Source-built QEMU/Wasm and firmware, complete guest sources, GitHub Pages | Verified source bundles, runtime hashes and deployed browser acceptance |
| Browser retention | Not implemented | Guest sync/unmount, storage/export, interruption and reopen tests |
| Browser networking | Disabled | Real transport, connection lifecycle, DNS/TLS and error acceptance |

Build, native guest execution and browser functional acceptance are separate
claims. Generated reports identify the exact source and actual artifacts.
Old custom-kernel milestones and application-manager contracts no longer
define the selected system; their code and documents have been removed.

## Priorities

The product keeps Emacs' text interface, keyboard vocabulary, buffers and
windows. Extend standard tools through parent system Elisp; preserve normal
file semantics and measure first use when adding a workflow. The project
website can use a graphical layout to introduce real text sessions.

1. **Workspace continuity:** define how files leave the guest and return in a
   later machine. Start with explicit save/restore, then browser-local retained
   storage, with interruption, capacity and error acceptance.
2. **Daily text workflows:** refine notes, task capture, search and recent-file
   navigation around concrete tasks. Keep an ordinary command entry for each
   capability and reuse Org, Dired, Eshell and standard help.
3. **Web experience and showcase:** keep terminal input/focus reliable, expand
   the gallery only with accepted workflows, and measure startup and cold
   library/manual delays. Keep the homepage independent of machine boot.
4. **Browser transport:** select a supported network bridge and verify actual
   guest traffic. Validate asynchronous TLS separately on the pinned runtime
   before adding online applications or package downloads.
5. **Distribution:** keep sources and binary releases paired. The text workspace
   has a matching runtime and complete corresponding source release. Every future
   system-input change needs a new runtime and updated public pin before deployment.
   See [release materials](releasing.md).

## Data-disk gate

Before adding disk import/export or persistent storage, define format and size
validation, who owns the bytes, boot attachment versus hot attach, guest
mount/sync/unmount behavior, dirty/error reporting and the retained destination.
Acceptance must reopen a new machine and compare actual saved content. A file
picker or exported byte array alone does not establish guest synchronization.
