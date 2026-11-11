---
name: emacsos-pr
description: Prepare or update emacsOS pull requests with English titles, structured descriptions, scoped evidence, and explicit dependency handoff.
---

# emacsOS pull requests

Read [CONTRIBUTING.md](../../../CONTRIBUTING.md) and use the
[PR template](../../../.github/pull_request_template.md).

- Keep one reviewable behavior per PR and name its base/dependencies.
- Use English, `<area>: <Imperative summary>`, and four populated sections:
  Summary, Changes, Validation, Limits and follow-up.
- Inspect the final diff and rewrite the description around the implemented
  result. Distinguish present behavior, proposals and archived code.
- Run relevant checks and contribution checks; report actual commands, failures
  and skips. Compilation is not boot or functional acceptance.
- Publish necessary fork commits before updating the parent's gitlink. Identify
  both revisions and stage the pin before checks; CI requires a clean submodule.
- Push the tested head, create/update the PR with structured arguments or a
  UTF-8 body file, and attach it to the task. Report the exact head and limits.
  A delegated author does not merge or push main.

For future file support, preserve normal Emacs save, backup, lock and error
behavior. Guest tests need actual source/image identity; persistence claims need
explicit sync/retention evidence. Do not invent completed tests or capabilities.
