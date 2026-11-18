# Documentation

Start with the [project overview](../README.md) or choose the path that matches
what you want to do.

## Use the computer

1. [First session](first-session.md): evaluate Lisp, save a command and explore files.
2. [Browser hosting](web.md): local startup, remote access, memory and session lifetime.
3. [Build and test](build-and-test.md): dependencies, commands and generated files.

## Understand and extend it

| Guide | What it covers |
| --- | --- |
| [Architecture](design.md) | Browser presentation, Emacs, Linux and trust boundaries |
| [Releases and Pages](releasing.md) | Runtime sources, emulator rebuilding and public hosting |
| [Source layout](source-layout.md) | Where system policy belongs and how the Emacs pin works |
| [Runtime build](runtime-build.md) | Image recipe, enabled features and installed runtime |
| [Roadmap](roadmap.md) | Current capabilities and the next implementation gates |
| [Contributing](../CONTRIBUTING.md) | Public language, ownership, PRs and integration |

## Verify and publish

| Guide | What it covers |
| --- | --- |
| [Functional acceptance](mvp.md) | Actual native guest checks and capability boundaries |
| [Browser acceptance](web-browser-checks.md) | Real QEMU/Wasm checks, screenshots and reports |
| [Licensing and distribution](licensing.md) | Parent license, component ownership and runtime release status |
| [Image provenance](images/README.md) | Where the README and showcase screenshots came from |

The browser and native profiles have different storage and networking
lifetimes. A result for one profile does not establish the other. The previous
custom kernel is available in Git history; it is no longer a development target.
