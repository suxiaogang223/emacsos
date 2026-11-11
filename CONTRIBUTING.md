# Contributing to emacsOS

Use English for public contributor discussion, documentation, code comments,
docstrings, commits, PRs, issues and reviews. Private discussion follows the
user's language preference. Preserve imported upstream language resources,
Unicode tables, names, test data and unchanged third-party sources.

Read the [design](docs/design.md), [roadmap](docs/roadmap.md) and
[source ownership](docs/source-layout.md) before architectural changes. This
baseline contains a kernel/serial scaffold and an Emacs source dependency;
it does not run Emacs. Describe implemented behavior separately from plans.

## Source ownership

Initialize `emacs/` with `git submodule update --init --recursive`. Necessary
runtime modifications belong to the fork; parent system features and future PAL
backends belong here. Publish fork commits before updating the parent gitlink.
Stage the intended pin before contribution checks. CI requires a clean,
initialized submodule matching that pin; dirty local source is not a published
handoff. Keep dependencies between fork and parent changes explicit.

## Pull requests

Use a focused branch; delegated development uses independent worktrees and
`codex/` branches. Target `main`, or explicitly identify a prerequisite branch.
The delegated author opens the PR and reports its exact head, checks and limits.
Only the coordinating maintainer integrates it within the user's authorization.
Do not change another task's checkout.

Use an English title `<area>: <Imperative summary>`, preferably at most 72
characters. Populate all four sections of the [template](.github/pull_request_template.md):

- **Summary:** concrete problem and resulting behavior.
- **Changes:** implementation details and dependencies needed for review.
- **Validation:** exact commands and observed results, including failures/skips.
- **Limits and follow-up:** actual gaps; use `None` when there are none.

Match claims to evidence. Compilation does not prove booting; host experiments
do not prove guest behavior. Do not claim functional acceptance unless it was
actually run. For future guest tests, record exact parent/fork source and image
identity. File changes must retain normal Emacs saving, backups, locks and
original errors. Persistence requires a separately specified synchronization
and retention boundary.

## Review and integration

Review the actual diff at an exact head. Report actionable findings in English
with file/line references, concrete triggers and impact. Distinguish reviewer
checks from author evidence. Resolve blocking findings and check dependencies,
current head and CI before integration. Record the review successfully before
issuing an expected-head guarded merge. A changed head requires review of the
new content and affected evidence.

Use COMMENT when the same GitHub account authored the PR; do not claim an
independent approval. Prefer squash merges for focused changes. Attach the PR,
synchronize the main checkout without losing unrelated edits, and update the
roadmap only when the stated criteria are satisfied. Skills:
[PR preparation](.agents/skills/emacsos-pr/SKILL.md) and
[review](.agents/skills/emacsos-review/SKILL.md).

## Contribution checks

```sh
python3 scripts/check-contribution.py --require-clean-submodule
python3 -m unittest discover -s scripts/tests -p 'test_contribution.py'
```

Use `--base origin/main` to check added third-party text against a branch base.
The checker scans parent-owned text and fork additions relative to the declared
GNU baseline, preserving unchanged upstream multilingual bytes. It rejects Han
prose and malformed text with diagnostics; it is not a general language
detector. Reviewers still verify English. PR CI also checks the title and four
populated sections using the event payload as data and read-only permissions.
