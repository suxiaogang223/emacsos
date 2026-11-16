"""Protect system artifact identity and existing user disk contents."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("machine", Path(__file__).resolve().parents[2] / "scripts/machine.py")
machine = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(machine)


class MachineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        root = Path(self.temporary.name)
        system, out = root / "system", root / "build/machine"
        system.mkdir()
        out.mkdir(parents=True)
        (root / "scripts").mkdir()
        (root / "scripts/machine.py").write_text("build recipe")
        (system / "boot").write_text("guest startup")
        for name, value in (("ROOT", root), ("SYSTEM", system), ("OUT", out)):
            patcher = patch.object(machine, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)
        patcher = patch.object(machine, "git", return_value="160000 pinned-source 0\temacs")
        patcher.start()
        self.addCleanup(patcher.stop)
        self.out, self.system = out, system
        artifact = out / "vmlinuz"
        artifact.write_bytes(b"kernel bytes")
        self.manifest = {"inputs": machine.inputs(), "fork_pin": "pinned-source",
                         "artifacts": {"vmlinuz": machine.digest(artifact)}}
        self.save_manifest()

    def save_manifest(self):
        (self.out / "manifest.json").write_text(json.dumps(self.manifest))

    def test_matching_artifacts_and_inputs_are_accepted(self):
        self.assertEqual(machine.verify(), self.manifest)

    def test_changed_or_missing_kernel_is_rejected(self):
        artifact = self.out / "vmlinuz"
        artifact.write_bytes(b"different kernel")
        with self.assertRaisesRegex(RuntimeError, "artifact changed"):
            machine.verify()
        artifact.unlink()
        with self.assertRaises((RuntimeError, FileNotFoundError)):
            machine.verify()

    def test_changed_startup_or_fork_pin_is_rejected(self):
        startup = self.system / "boot"
        startup.write_text("changed startup")
        with self.assertRaisesRegex(RuntimeError, "sources changed"):
            machine.verify()
        startup.write_text("guest startup")
        self.manifest["fork_pin"] = "different-source"
        self.save_manifest()
        with self.assertRaisesRegex(RuntimeError, "sources changed"):
            machine.verify()

    def test_existing_user_disk_is_never_formatted(self):
        disk = self.out / "data.ext4"
        disk.write_bytes(b"user files")
        with patch.object(machine, "run") as command:
            with self.assertRaises(FileExistsError):
                machine.create_disk(disk)
            command.assert_not_called()
        self.assertEqual(disk.read_bytes(), b"user files")


if __name__ == "__main__":
    unittest.main()
