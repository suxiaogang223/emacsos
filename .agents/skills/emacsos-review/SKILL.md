---
name: emacsos-review
description: Review and integrate emacsOS pull requests with English findings, exact-commit evidence, and guarded merges.
---

# emacsOS review and integration

Read [CONTRIBUTING.md](../../../CONTRIBUTING.md), relevant design and roadmap.
Apply this workflow only within the user's existing authorization.

Review the actual current diff/head, new files, dependencies and error paths.
Check that English prose and evidence match the implemented scope. Independently
run checks that resolve material uncertainty and identify them separately from
author results. Preserve upstream bytes and verify source provenance.

For fork changes, review the fork commit and parent pin separately. Ensure the
fork commit is published and the initialized submodule matches the staged pin.
CI requires a clean submodule. Compilation does not establish boot execution or
full guest behavior. Future file changes must preserve ordinary Emacs saves,
backups, locks and original errors; future persistence/network claims need
corresponding real backend evidence.

Report actionable findings in English, ordered by severity, with file/line,
trigger and impact. Resolve blocking findings. Record the exact reviewed head,
evidence, limits and decision; use COMMENT for same-account PRs without claiming
independent approval.

Before merging, confirm that the review record succeeded, prerequisites are in
the target base, applicable CI passes and the head is unchanged. Use an
expected-head guard. A changed head requires review of new content and affected
checks. Attach the PR, synchronize main without losing unrelated edits, and mark
only satisfied roadmap criteria complete. Delegated authors do not integrate.
