"""Check installed-runtime packaging and reject corrupted image components."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest
from unittest import mock

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import image
import qemu

spec = importlib.util.spec_from_file_location("package_runtime", SCRIPTS / "package-runtime.py")
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class RuntimePackagingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.out = Path(self.temporary.name) / "runtime"
        self.site = Path(self.temporary.name) / "site"
        self.site.mkdir()
        self.out.mkdir()
        for name in ("source.tar", "build-key", "packages.txt", "compiler.txt",
                     "configure-arguments.txt", "source-date-epoch.txt", "container-id"):
            (self.out / name).write_text(name)
        (self.out / "container-id").write_text("sha256:" + "1" * 64 + "\n")
        self.root = self.out / "root"
        for name, content in (("usr/bin/emacs", b"static executable"),
                              ("usr/libexec/emacs/emacs-identity.pdmp", b"matching dump")):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        self.repackage()

    def repackage(self):
        check_output = package.subprocess.check_output

        def command(args, **kwargs):
            if args[0] == "docker":
                raise AssertionError("Packaging must record the used image ID, not resolve a mutable tag")
            return check_output(args, **kwargs)

        with mock.patch.object(sys, "argv", ["package-runtime", "--output", str(self.out),
                                             "--site-lisp", str(self.site)]), \
                mock.patch.object(package.subprocess, "check_output", side_effect=command), \
                contextlib.redirect_stdout(io.StringIO()):
            package.main()

    def test_lisp_add_change_and_removal_repackage_without_reinstall(self):
        executable = self.root / "usr/bin/emacs"
        original = executable.read_bytes()
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
            self.assertEqual(original, executable.read_bytes())
        self.assertEqual(hashes[0], hashes[-1])
        self.assertEqual(len(set(hashes)), 3)

    def test_manifest_records_the_immutable_image_used_for_the_build(self):
        manifest = json.loads((self.out / "manifest.json").read_text())
        self.assertEqual(manifest["container_id"], (self.out / "container-id").read_text().strip())
        self.assertEqual(manifest["elf"]["sha256"], package.sha256(self.out / "emacs"))
        self.assertEqual((self.out / "emacs").read_bytes(), (self.root / "usr/bin/emacs").read_bytes())
        self.assertEqual((self.root / "usr/libexec/emacs/emacs.pdmp").read_bytes(), b"matching dump")


class LittlefsDependencyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.checkout = self.root / "third_party/littlefs"
        self.checkout.mkdir(parents=True)
        for path in (self.root, self.checkout):
            self.git(path, "init", "-q")
            self.git(path, "config", "user.name", "Dependency Test")
            self.git(path, "config", "user.email", "test@example.invalid")
        (self.checkout / "lfs.c").write_text("/* Upstream fixture. */\n")
        self.git(self.checkout, "add", ".")
        self.git(self.checkout, "commit", "-qm", "Create upstream fixture")
        self.git(self.root, "add", "third_party/littlefs")
        self.metadata = {"commit": self.git(self.checkout, "rev-parse", "HEAD"),
                         "files_sha256": {"lfs.c": image.digest(self.checkout / "lfs.c")}}
        self.metadata_path = self.root / "third_party/littlefs.json"
        self.write_metadata()
        patcher = mock.patch.object(image, "ROOT", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)

    @staticmethod
    def git(path, *args):
        return subprocess.check_output(["git", "-C", str(path), *args],
                                       stderr=subprocess.PIPE, text=True).strip()

    def write_metadata(self):
        self.metadata_path.write_text(json.dumps(self.metadata))

    def test_uninitialized_dependency_is_rejected_before_compilation(self):
        self.assertEqual(image.verify_littlefs(), self.metadata)
        (self.checkout / ".git").rename(self.root / "saved-git")
        with self.assertRaisesRegex(SystemExit, "not initialized"):
            image.verify_littlefs()

    def test_checkout_gitlink_and_release_metadata_must_agree(self):
        (self.checkout / "lfs.c").write_text("/* New upstream release. */\n")
        self.git(self.checkout, "commit", "-qam", "Change upstream fixture")
        with self.assertRaisesRegex(SystemExit, "differs from the parent pin"):
            image.verify_littlefs()
        self.git(self.root, "add", "third_party/littlefs")
        with self.assertRaisesRegex(SystemExit, "gitlink matching"):
            image.verify_littlefs()
        self.metadata.update(commit=self.git(self.checkout, "rev-parse", "HEAD"),
                             files_sha256={"lfs.c": image.digest(self.checkout / "lfs.c")})
        self.write_metadata()
        self.assertEqual(image.verify_littlefs(), self.metadata)

    def test_local_changes_and_incorrect_source_receipt_are_rejected(self):
        extra = self.checkout / "untracked.c"
        extra.write_text("/* Untracked source. */\n")
        with self.assertRaisesRegex(SystemExit, "must be clean"):
            image.verify_littlefs()
        extra.unlink()
        self.metadata["files_sha256"]["lfs.c"] = "0" * 64
        self.write_metadata()
        with self.assertRaisesRegex(SystemExit, "upstream source differs"):
            image.verify_littlefs()


class DataImageTests(unittest.TestCase):
    def test_missing_disk_is_initialized_but_existing_data_is_never_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template, disk = root / "empty.img", root / "data.img"
            with template.open("wb") as stream:
                stream.write(b"template")
                stream.truncate(qemu.DATA_SIZE)
            qemu.prepare_data(disk, template)
            self.assertEqual(disk.stat().st_size, qemu.DATA_SIZE)
            with disk.open("r+b") as stream:
                stream.write(b"personal")
            qemu.prepare_data(disk, template)
            with disk.open("rb") as stream:
                self.assertEqual(stream.read(8), b"personal")
            disk.write_bytes(b"damaged disk, preserve these bytes")
            with self.assertRaises(ValueError):
                qemu.prepare_data(disk, template)
            self.assertEqual(disk.read_bytes(), b"damaged disk, preserve these bytes")

    def test_retained_data_has_no_snapshot_option_and_paths_escape_commas(self):
        command = qemu.command("system,image.img", data="data,image.img")
        drives = [command[index + 1] for index, value in enumerate(command) if value == "-drive"]
        self.assertIn("snapshot=on", drives[0])
        self.assertIn("file=system,,image.img,", drives[0])
        self.assertIn("file=data,,image.img,", drives[1])
        self.assertNotIn("snapshot", drives[1])
        self.assertNotIn("unsafe", drives[1])


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
