# emacsOS contributor instructions

Use English for public documentation, comments/docstrings, commits, PRs, issues
and reviews. Private user discussion follows the user's explicit language
preference. Preserve unchanged upstream language resources, Unicode data, names
and tests. See [CONTRIBUTING.md](CONTRIBUTING.md).

Read [design](docs/design.md), [roadmap](docs/roadmap.md) and
[source ownership](docs/source-layout.md) before changing architecture. The
MVP runs one trusted Emacs with volatile files and cooperative application
management on the fixed QEMU profile. See docs/mvp.md for acceptance. Do not
imply networking, persistence, isolation or general Linux compatibility.
Compilation, boot execution and guest functional acceptance are different claims.

Before preparing/updating a PR, read
[emacsos-pr](.agents/skills/emacsos-pr/SKILL.md). Before reviewing/integrating one,
read [emacsos-review](.agents/skills/emacsos-review/SKILL.md).

Delegated development uses independent worktrees and `codex/` branches. Child
tasks open PRs and report exact heads, validation and dependencies; only the
coordinator merges within the user's authorization. Do not modify another task's
checkout. Explicit user instructions may define a different handoff for a task.

`third_party/littlefs/` pins official upstream sources; keep adapter/configuration
in the parent and update its gitlink and `third_party/littlefs.json` together.
`emacs/` is a pinned fork submodule. Publish fork changes first, stage the parent
gitlink, and check the initialized checkout matches it. Keep future system policy
outside the runtime fork. Do not restore archived features implicitly or add
frameworks before concrete backend/runtime needs justify them.
