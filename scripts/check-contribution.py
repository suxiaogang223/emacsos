#!/usr/bin/env python3
"""Check project language markers and PR conventions without external packages."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

HAN = re.compile("[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\U00020000-\U000323af]")
SECTIONS = ("Summary", "Changes", "Validation", "Limits and follow-up")
UPSTREAM = {
    "upstream_repository": "https://github.com/emacs-mirror/emacs",
    "upstream_tag": "emacs-30.2",
    "upstream_commit": "636f166cfc86aa90d63f592fd99f3fdd9ef95ebd",
}
BINARY_SUFFIXES = {".bin", ".data", ".elc", ".elf", ".gif", ".ico", ".img",
                   ".jpeg", ".jpg", ".o", ".otf", ".pdf", ".pdmp", ".png",
                   ".ttf", ".wasm", ".webp", ".woff", ".woff2"}


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def imported(path):
    return path.startswith(("emacs/", "third_party/"))


def text_errors(text, label):
    return [f"{label}:{number}: Han characters found; write project prose in English"
            for number, line in enumerate(text.splitlines(), 1) if HAN.search(line)]


def read_text(root, path):
    if Path(path).suffix.lower() in BINARY_SUFFIXES:
        return None, []
    data = (root / path).read_bytes()
    if b"\0" in data:
        return None, [f"{path}: NUL bytes are not valid UTF-8 project text"]
    try:
        return data.decode("utf-8"), []
    except UnicodeDecodeError:
        return None, [f"{path}: text must use UTF-8"]


def file_errors(root, path):
    text, errors = read_text(root, path)
    return errors + (text_errors(text, path) if text is not None else [])


def added_text_errors(root, base, paths, untracked=(), label_prefix=""):
    """Check real added bytes, independent of Git attributes or text converters."""
    errors = []
    for path in sorted(paths):
        if not (root / path).is_file():
            continue
        label = label_prefix + path
        if path in untracked:
            errors.extend(error.replace(path + ":", label + ":", 1)
                          for error in file_errors(root, path))
            continue
        text, encoding_errors = read_text(root, path)
        errors.extend(error.replace(path + ":", label + ":", 1)
                      for error in encoding_errors)
        if text is None:
            continue
        patch = git(root, "diff", "--text", "--no-textconv", "--no-ext-diff", "--no-color",
                    "--no-renames", "--unified=0", base, "--", path).decode("utf-8")
        line_number = None
        for line in patch.splitlines():
            hunk = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@", line)
            if hunk:
                line_number = int(hunk[1])
            elif line_number is not None and line.startswith("+"):
                if HAN.search(line[1:]):
                    errors.append(f"{label}:{line_number}: added fork/vendor text contains Han characters")
                line_number += 1
            elif line_number is not None and line.startswith(" "):
                line_number += 1
    return errors


def fork_errors(root, require_clean=False):
    """Validate the pinned checkout and inspect fork changes even without --base.

    Local edits are scanned by default. Stage a changed fork commit in the parent
    before checking it; CI additionally requires a clean submodule worktree.
    """
    fork = root / "emacs"
    entries = git(root, "ls-files", "--stage", "--", "emacs").decode().splitlines()
    if len(entries) != 1 or not re.fullmatch(r"160000 [0-9a-f]{40} 0\temacs", entries[0]):
        return ["emacs: parent index must contain one initialized submodule gitlink"]
    pin = entries[0].split()[1]
    if not (fork / ".git").exists():
        return ["emacs: submodule is not initialized; run git submodule update --init --recursive"]
    if Path(git(fork, "rev-parse", "--show-toplevel").decode().strip()).resolve() != fork.resolve():
        return ["emacs: expected a separate initialized submodule repository"]
    errors = []
    if git(fork, "rev-parse", "HEAD").decode().strip() != pin:
        errors.append("emacs: checkout HEAD differs from parent gitlink; stage the intended fork commit")
    modules = root / ".gitmodules"
    if not modules.is_file():
        errors.append(".gitmodules: missing submodule declaration")
    else:
        paths = git(root, "config", "--file", str(modules), "--get-regexp",
                    r"^submodule\..*\.path$").decode().splitlines()
        keys = [line.rsplit(" ", 1)[0][:-5] for line in paths if line.endswith(" emacs")]
        if len(keys) != 1:
            errors.append(".gitmodules: declare exactly one submodule at emacs")
        else:
            url = git(root, "config", "--file", str(modules), "--get", keys[0] + ".url").decode().strip()
            if not url:
                errors.append(".gitmodules: Emacs submodule URL must not be empty")
    if require_clean and git(fork, "status", "--porcelain", "--untracked-files=all"):
        errors.append("emacs: CI requires a clean submodule checkout, including untracked files")
    metadata_path = fork / "EMACSOS.json"
    if not metadata_path.is_file():
        return errors + ["emacs/EMACSOS.json: missing fork upstream provenance"]
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if not isinstance(metadata, dict) or any(metadata.get(key) != value
                                            for key, value in UPSTREAM.items()):
        return errors + ["emacs/EMACSOS.json: unexpected upstream repository, tag, or commit"]
    base = UPSTREAM["upstream_commit"]
    git(fork, "rev-parse", "--verify", base + "^{commit}")
    ancestry = subprocess.run(["git", "-C", str(fork), "merge-base", "--is-ancestor", base, pin],
                              check=False, capture_output=True)
    if ancestry.returncode != 0:
        return errors + ["emacs: pinned fork commit must descend from the declared upstream commit"]
    changed = set(git(fork, "diff", "--name-only", "--diff-filter=ACMR", "--no-renames",
                      "-z", base).decode().split("\0")) - {""}
    untracked = set(git(fork, "ls-files", "--others", "--exclude-standard", "-z")
                    .decode().split("\0")) - {""}
    errors.extend(added_text_errors(fork, base, changed | untracked,
                                   untracked, "emacs/"))
    return errors


def repository_errors(root, base=None, require_clean_submodule=False):
    paths = set(git(root, "ls-files", "-co", "--exclude-standard", "-z")
                .decode("utf-8").split("\0")) - {""}
    errors = []
    for path in sorted(paths):
        if not imported(path) and (root / path).is_file():
            errors.extend(file_errors(root, path))
    errors.extend(fork_errors(root, require_clean_submodule))
    if base is not None:
        base = git(root, "rev-parse", "--verify", base + "^{commit}").decode().strip()
        changed = set(git(root, "diff", "--name-only", "--diff-filter=ACMR", "--no-renames",
                          "-z", base, "--", "third_party").decode().split("\0")) - {""}
        untracked = set(git(root, "ls-files", "--others", "--exclude-standard", "-z",
                            "--", "third_party").decode().split("\0")) - {""}
        errors.extend(added_text_errors(root, base, changed | untracked, untracked))
    return errors


def pull_request_errors(event):
    pr = event.get("pull_request")
    if not isinstance(pr, dict):
        return ["Event payload must contain a pull_request object"]
    title = pr.get("title") or ""
    body = pr.get("body") or ""
    errors = text_errors(title, "PR title") + text_errors(body, "PR body")
    if not re.fullmatch(r"[a-z][a-z0-9-]*: [A-Z][^\r\n]+", title):
        errors.append("PR title must use '<area>: <Imperative summary>'")
    visible = re.sub(r"<!--.*?-->", "", body, flags=re.DOTALL)
    headings = list(re.finditer(r"^## (.+?)\s*$", visible, flags=re.MULTILINE))
    if tuple(match[1] for match in headings) != SECTIONS:
        errors.append("PR body must contain the four template sections in order")
        return errors
    for index, heading in enumerate(headings):
        end = headings[index + 1].start() if index + 1 < len(headings) else len(visible)
        if not visible[heading.end():end].strip():
            errors.append(f"PR section '{heading[1]}' must describe the change")
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="Parent base commit/ref for added third-party text")
    parser.add_argument("--require-clean-submodule", action="store_true",
                        help="Reject dirty or untracked nested fork files (required in CI)")
    parser.add_argument("--event", type=Path, help="GitHub pull_request event JSON")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    try:
        errors = repository_errors(root, args.base, args.require_clean_submodule)
        if args.event:
            errors.extend(pull_request_errors(json.loads(args.event.read_text(encoding="utf-8"))))
    except (OSError, UnicodeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Contribution check could not run: {type(error).__name__}", file=sys.stderr)
        return 2
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print("Contribution language and format checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
