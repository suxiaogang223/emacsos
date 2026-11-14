"""Exercise runtime reuse and reject stale or corrupted image components."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import image

spec = importlib.util.spec_from_file_location("package_runtime", SCRIPTS / "package-runtime.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class RuntimeCacheTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.out = Path(self.temporary.name) / "runtime"
        self.site = Path(self.temporary.name) / "site"
        self.site.mkdir()
        self.out.mkdir()
        for name in ("source.tar", "build-key", "packages.txt", "compiler.txt",
                     "configure-arguments.txt", "source-date-epoch.txt"):
            (self.out / name).write_text(name)
        self.root = self.out / "root"
        for name, content in (("usr/bin/emacs", b"static executable"),
                              ("usr/libexec/emacs/emacs-identity.pdmp", b"matching dump")):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        self.repackage()

    def repackage(self, parent=None):
        check_output = package.subprocess.check_output

        def command(args, **kwargs):
            if args[0] == "docker":
                return "sha256:test-container\n"
            if parent and args[-2:] == ["rev-parse", "HEAD"] and args[2] == str(SCRIPTS.parent):
                return parent + "\n"
            return check_output(args, **kwargs)

        with mock.patch.object(sys, "argv", ["package-runtime", "--output", str(self.out),
                                             "--site-lisp", str(self.site)]), \
                mock.patch.object(package.subprocess, "check_output", side_effect=command), \
                contextlib.redirect_stdout(io.StringIO()):
            package.main()

    def test_unchanged_archive_and_executable_are_not_rewritten(self):
        paths = [self.out / "emacs", self.out / "runtime.cpio", self.out / "manifest.json"]
        before = [path.stat().st_mtime_ns for path in paths]
        self.repackage()
        self.assertEqual(before, [path.stat().st_mtime_ns for path in paths])

    def test_lisp_add_change_and_removal_repackage_without_reinstall(self):
        executable = self.root / "usr/bin/emacs"
        before = executable.stat().st_mtime_ns
        archive = self.out / "runtime.cpio"
        hashes = [package.sha256(archive)]
        source = self.site / "sample.el"
        for text in ("(message \"first\")", "(message \"second\")", None):
            if text is None:
                source.unlink()
            else:
                source.write_text(text)
            self.repackage()
            hashes.append(package.sha256(archive))
            installed = self.root / "usr/share/emacs/site-lisp/sample.el"
            self.assertEqual(installed.exists(), text is not None)
            self.assertEqual(before, executable.stat().st_mtime_ns)
        self.assertEqual(hashes[0], hashes[-1])
        self.assertEqual(len(set(hashes)), 3)

    def test_parent_revision_refresh_does_not_rewrite_runtime_bytes(self):
        archive = self.out / "runtime.cpio"
        before = archive.stat().st_mtime_ns
        self.repackage(parent="new-parent-revision")
        manifest = json.loads((self.out / "manifest.json").read_text())
        self.assertEqual(manifest["parent_commit"], "new-parent-revision")
        self.assertEqual(before, archive.stat().st_mtime_ns)

    def test_packaging_recipe_change_requires_reinstallation(self):
        path = self.out / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["packager_sha256"] = "old-recipe"
        path.write_text(json.dumps(manifest))
        self.assertIsNone(package.cached_manifest(self.out))

    def test_changed_artifacts_and_installed_bytes_invalidate_cache(self):
        for name in ("source.tar", "build-key", "emacs", "runtime.cpio", "root/usr/bin/emacs"):
            with self.subTest(name=name):
                path = self.out / name
                original = path.read_bytes()
                path.write_bytes(original + b"changed")
                self.assertIsNone(package.cached_manifest(self.out))
                path.write_bytes(original)
                self.assertIsNotNone(package.cached_manifest(self.out))

    def test_missing_extra_mode_and_symlink_changes_invalidate_cache(self):
        path = self.root / "usr/bin/emacs"
        content = path.read_bytes()
        path.unlink()
        self.assertIsNone(package.cached_manifest(self.out))
        path.write_bytes(content)
        path.chmod(0o755)
        self.assertIsNotNone(package.cached_manifest(self.out))
        path.chmod(0o644)
        self.assertIsNone(package.cached_manifest(self.out))
        path.chmod(0o755)
        extra = self.root / "unexpected"
        extra.touch()
        self.assertIsNone(package.cached_manifest(self.out))
        extra.unlink()
        alias = self.root / "usr/libexec/emacs/emacs.pdmp"
        alias.unlink()
        alias.symlink_to("different.pdmp")
        self.assertIsNone(package.cached_manifest(self.out))


class ImageTests(unittest.TestCase):
    def test_corrupt_header_payload_padding_and_trailing_data_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            boot, elf, archive, disk = [root / name for name in ("boot", "elf", "archive", "disk")]
            for path, data in ((boot, b"boot"), (elf, b"ELF"), (archive, b"archive")):
                path.write_bytes(data)
            image.pack_image(boot, elf, archive, disk)
            image.verify_image(disk, boot, elf, archive)
            original = disk.read_bytes()
            for offset in (0, 1024 * 1024, 1024 * 1024 + 512,
                           1024 * 1024 + 512 + 3, len(original)):
                with self.subTest(offset=offset):
                    damaged = bytearray(original)
                    if offset == len(damaged):
                        damaged += b"extra"
                    else:
                        damaged[offset] ^= 1
                    disk.write_bytes(damaged)
                    with self.assertRaises(SystemExit):
                        image.verify_image(disk, boot, elf, archive)
            disk.write_bytes(original)
            elf.write_bytes(b"")
            with self.assertRaises(ValueError):
                image.pack_image(boot, elf, archive, disk)
            self.assertEqual(disk.read_bytes(), original)
            self.assertFalse(disk.with_suffix(".part").exists())


if __name__ == "__main__":
    unittest.main()
