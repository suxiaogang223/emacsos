"""Behavioral tests for project language and PR event checks."""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/check-contribution.py"
SPEC = importlib.util.spec_from_file_location("contribution", SCRIPT)
CONTRIBUTION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTRIBUTION)
MARKER = "\u4e2d\u6587"


class ContributionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "project"
        self.root.mkdir()
        upstream = Path(self.temporary.name) / "upstream"
        upstream.mkdir()
        self.command(upstream, "init", "-q")
        self.configure(upstream)
        self.write_at(upstream, "etc/HELLO", MARKER + "\n")
        self.write_at(upstream, "lisp/example.el", ";;; Upstream example\n")
        self.command(upstream, "add", ".")
        self.command(upstream, "commit", "-qm", "Create upstream fixture")
        base = self.command(upstream, "rev-parse", "HEAD").decode().strip()
        self.provenance = dict(CONTRIBUTION.UPSTREAM, upstream_commit=base)
        patcher = mock.patch.object(CONTRIBUTION, "UPSTREAM", self.provenance)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.git("init", "-q")
        self.configure(self.root)
        self.git("-c", "protocol.file.allow=always", "submodule", "add", "-q",
                 str(upstream), "emacs")
        self.fork = self.root / "emacs"
        self.configure(self.fork)
        self.write("emacs/EMACSOS.json", json.dumps(self.provenance) + "\n")
        self.write("emacs/src/platform.c", "/* Owned platform adaptation. */\n")
        self.command(self.fork, "add", ".")
        self.command(self.fork, "commit", "-qm", "Add fork adaptation")
        self.write("README.md", "English project prose.\n")
        self.git("add", ".")
        self.git("commit", "-qm", "Create project with pinned fork")

    @staticmethod
    def command(root, *args):
        return subprocess.check_output(["git", "-C", str(root), *args],
                                       stderr=subprocess.PIPE)

    def configure(self, root):
        self.command(root, "config", "user.name", "Convention Test")
        self.command(root, "config", "user.email", "test@example.invalid")

    def git(self, *args):
        return self.command(self.root, *args)

    @staticmethod
    def write_at(root, path, text):
        target = root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def write(self, path, text):
        self.write_at(self.root, path, text)

    def errors(self, base="HEAD", require_clean=False):
        return CONTRIBUTION.repository_errors(self.root, base, require_clean)

    def test_unchanged_upstream_language_data_is_preserved(self):
        self.assertEqual(self.errors(), [])
        self.assertEqual(self.errors(base=None, require_clean=True), [])

    def test_single_initial_parent_commit_needs_no_legacy_baseline(self):
        self.git("checkout", "--orphan", "initial-project")
        self.git("commit", "-qm", "Initialize project")
        self.assertEqual(self.git("rev-list", "--count", "HEAD").strip(), b"1")
        self.assertEqual(self.errors(base=None, require_clean=True), [])
        self.write("emacs/lisp/example.el", ";;; Upstream example\n;; " + MARKER + "\n")
        self.assertIn("emacs/lisp/example.el:2:", self.errors(base=None)[0])

    def test_project_prose_is_checked_in_untracked_files(self):
        self.write("docs/new.md", "English\n" + MARKER + "\n")
        self.assertEqual(len(self.errors()), 1)
        self.assertIn("docs/new.md:2:", self.errors()[0])

    def test_project_lisp_and_tests_are_checked_in_full(self):
        for path in ("lisp/emacsos/new.el", "tests/lisp/emacsos/new-tests.el"):
            self.write(path, ";;; " + MARKER + "\n")
        self.assertEqual(len(self.errors()), 2)

    def test_new_fork_source_is_checked_in_full(self):
        self.write("emacs/src/platform.c", "/* " + MARKER + " */\n")
        self.assertEqual(len(self.errors()), 1)
        self.assertIn("emacs/src/platform.c:1:", self.errors()[0])

    def test_added_upstream_comments_are_checked_without_parent_base(self):
        self.write("emacs/lisp/example.el", ";;; Upstream example\n;; " + MARKER + "\n")
        self.assertEqual(len(self.errors(base=None)), 1)
        self.assertIn("example.el:2:", self.errors(base=None)[0])

    def test_committed_fork_changes_are_checked_against_upstream(self):
        self.write("emacs/lisp/example.el", ";;; Upstream example\n;; " + MARKER + "\n")
        self.command(self.fork, "add", ".")
        self.command(self.fork, "commit", "-qm", "Add invalid fork prose")
        self.git("add", "emacs")
        self.assertIn("added fork/vendor text", self.errors(base=None, require_clean=True)[0])

    def test_git_binary_attributes_cannot_hide_comments(self):
        self.write("emacs/.gitattributes", "lisp/example.el -diff\n")
        self.write("emacs/lisp/example.el", ";;; Upstream example\n;; " + MARKER + "\n")
        self.assertEqual(len(self.errors()), 1)
        self.assertIn("example.el:2:", self.errors()[0])

    def test_git_textconv_cannot_replace_checked_contents(self):
        self.write("emacs/.gitattributes", "lisp/example.el diff=hide\n")
        self.write("hide-diff.sh", "printf 'English replacement\\n'\n")
        self.command(self.fork, "config", "diff.hide.textconv", "sh " + str(self.root / "hide-diff.sh"))
        self.write("emacs/lisp/example.el", ";;; Upstream example\n;; " + MARKER + "\n")
        self.assertEqual(len(self.errors()), 1)

    def test_new_upstream_paths_are_checked(self):
        self.write("emacs/lisp/added.el", ";; " + MARKER + "\n")
        self.assertEqual(len(self.errors()), 1)

    def test_removing_upstream_language_data_is_not_an_added_violation(self):
        self.write("emacs/etc/HELLO", "English\n")
        self.assertEqual(self.errors(), [])

    def test_missing_submodule_is_rejected(self):
        self.git("submodule", "deinit", "-f", "emacs")
        self.assertIn("not initialized", self.errors()[0])

    def test_plain_directory_cannot_replace_gitlink(self):
        self.git("rm", "--cached", "emacs")
        self.assertIn("gitlink", self.errors()[0])

    def test_wrong_submodule_commit_is_rejected_until_parent_pin_is_staged(self):
        self.write("emacs/lisp/new.el", ";;; English source.\n")
        self.command(self.fork, "add", ".")
        self.command(self.fork, "commit", "-qm", "Add valid fork change")
        self.assertIn("differs from parent gitlink", self.errors()[0])
        self.git("add", "emacs")
        self.assertEqual(self.errors(require_clean=True), [])

    def test_local_dirty_fork_is_scanned_but_ci_rejects_it(self):
        self.write("emacs/lisp/example.el", ";;; English local edit.\n")
        self.assertEqual(self.errors(), [])
        self.assertIn("clean submodule", self.errors(require_clean=True)[0])

    def test_ci_rejects_untracked_fork_files(self):
        self.write("emacs/lisp/local.el", ";;; English local file.\n")
        self.assertEqual(self.errors(), [])
        self.assertIn("including untracked", self.errors(require_clean=True)[0])

    def test_missing_fork_provenance_is_rejected(self):
        (self.fork / "EMACSOS.json").unlink()
        self.assertIn("missing fork upstream provenance", self.errors()[0])

    def test_wrong_fork_upstream_is_rejected(self):
        for key in self.provenance:
            metadata = dict(self.provenance, **{key: "unexpected"})
            self.write("emacs/EMACSOS.json", json.dumps(metadata))
            self.assertIn("unexpected upstream", self.errors()[0])

    def test_unrelated_history_cannot_claim_upstream_ancestry(self):
        self.command(self.fork, "checkout", "--orphan", "unrelated")
        self.command(self.fork, "commit", "-qm", "Create unrelated history")
        self.git("add", "emacs")
        self.assertIn("must descend", self.errors()[0])

    def test_added_vendor_lines_still_use_parent_baseline(self):
        self.write("third_party/example.c", "/* Original " + MARKER + " */\n")
        self.git("add", "third_party")
        self.git("commit", "-qm", "Import vendor fixture")
        self.assertEqual(self.errors(), [])
        self.write("third_party/example.c", "/* Original " + MARKER + " */\n/* " + MARKER + " */\n")
        self.assertEqual(len(self.errors()), 1)
        self.assertIn("third_party/example.c:2:", self.errors()[0])

    def test_project_binary_is_not_treated_as_prose(self):
        (self.root / "asset.bin").write_bytes(b"\0\xff")
        self.assertEqual(self.errors(), [])

    def test_non_utf8_text_is_rejected(self):
        (self.root / "new.md").write_bytes(b"English\xff")
        self.assertIn("text must use UTF-8", self.errors()[0])

    def test_document_nul_cannot_hide_prose(self):
        self.write("new.md", "English\0" + MARKER)
        self.assertEqual(len(self.errors()), 1)
        self.assertIn("NUL bytes", self.errors()[0])

    def test_utf16_document_is_rejected(self):
        (self.root / "new.md").write_bytes(MARKER.encode("utf-16"))
        self.assertTrue(self.errors())


class WorkflowTest(unittest.TestCase):
    def test_push_and_pull_request_baselines_use_event_specific_arguments(self):
        workflow = SCRIPT.parents[1] / ".github/workflows/contribution.yml"
        text = workflow.read_text(encoding="utf-8")
        script = text.split("        run: |\n", 1)[1].split("      - name:", 1)[0]
        script = "\n".join(line[10:] for line in script.splitlines())
        script = "python3() { printf '%s\\n' \"$@\"; }\n" + script
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def git(*args):
                return subprocess.check_output(["git", "-C", str(root), *args],
                                               stderr=subprocess.PIPE).decode().strip()
            git("init", "-q")
            git("config", "user.name", "Convention Test")
            git("config", "user.email", "test@example.invalid")
            git("commit", "--allow-empty", "-qm", "Create initial fixture")
            ancestor = git("rev-parse", "HEAD")
            git("checkout", "--orphan", "new-initial")
            git("commit", "--allow-empty", "-qm", "Reset fixture history")
            unrelated = ancestor
            ancestor = git("rev-parse", "HEAD")
            git("commit", "--allow-empty", "-qm", "Advance fixture")
            scenarios = (
                ("push", "0" * 40, False, False),
                ("push", "f" * 40, False, False),
                ("push", unrelated, False, False),
                ("push", ancestor, True, False),
                ("pull_request", ancestor, True, True),
            )
            for event, base, expect_base, expect_event in scenarios:
                with self.subTest(event=event, base=base):
                    env = dict(os.environ, CONTRIBUTION_EVENT=event, CONTRIBUTION_BASE=base,
                               GITHUB_EVENT_PATH=str(root / "event.json"))
                    env.pop("BASH_ENV", None)
                    args = subprocess.check_output(["bash", "-eu", "-o", "pipefail", "-c", script],
                                                   cwd=root, env=env).decode().splitlines()
                    self.assertIn("--require-clean-submodule", args)
                    self.assertEqual("--base" in args, expect_base)
                    self.assertEqual("--event" in args, expect_event)
                    if expect_base:
                        self.assertEqual(args[args.index("--base") + 1], base)


class PullRequestTest(unittest.TestCase):
    def event(self, title="docs: Translate the project documentation"):
        return {"pull_request": {"title": title, "body": "\n\n".join(
            f"## {section}\n\n{content}" for section, content in zip(
                CONTRIBUTION.SECTIONS,
                ("Make the documentation accessible.", "Translate seven documents.",
                 "Link and evidence checks passed.", "None")))}}

    def test_populated_english_template_passes(self):
        self.assertEqual(CONTRIBUTION.pull_request_errors(self.event()), [])

    def test_missing_scope_or_chinese_title_fails(self):
        for title in ("Translate docs", "docs: " + MARKER):
            self.assertTrue(CONTRIBUTION.pull_request_errors(self.event(title)))

    def test_template_comments_do_not_count_as_section_content(self):
        event = self.event()
        event["pull_request"]["body"] = "\n\n".join(
            f"## {section}\n\n<!-- Fill this in. -->" for section in CONTRIBUTION.SECTIONS)
        self.assertEqual(len(CONTRIBUTION.pull_request_errors(event)), 4)

    def test_chinese_in_a_hidden_comment_is_rejected(self):
        event = self.event()
        event["pull_request"]["body"] += "\n<!-- " + MARKER + " -->"
        self.assertTrue(CONTRIBUTION.pull_request_errors(event))

    def test_missing_repeated_or_reordered_sections_fail(self):
        for body in ("## Summary\n\nOnly one section", self.event()["pull_request"]["body"]
                     .replace("## Changes", "## Summary")):
            event = self.event()
            event["pull_request"]["body"] = body
            self.assertTrue(CONTRIBUTION.pull_request_errors(event))

    def test_shell_metacharacters_remain_plain_data(self):
        event = self.event()
        event["pull_request"]["body"] += "\n`$(touch should-not-exist)` and `${PATH}`"
        self.assertEqual(CONTRIBUTION.pull_request_errors(event), [])


if __name__ == "__main__":
    unittest.main()
