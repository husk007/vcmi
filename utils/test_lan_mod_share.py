"""Executable checks for publishing an installed VCMI mod into the host share directory."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).with_name("lan_mod_share.py")


class LanModShareTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.user_data = Path(self.temp.name)
        self.mod = self.user_data / "Mods" / "fixture"
        self.mod.mkdir(parents=True)
        (self.mod / "mod.json").write_text('{"name":"LAN fixture","version":"1.0"}', encoding="utf-8")
        (self.mod / "assets").mkdir()
        (self.mod / "assets" / "blob.bin").write_bytes(b"G" * 65589)

    def run_share(self, *extra, confirm=True):
        cmd = [sys.executable, str(SCRIPT), "fixture", "--user-data", str(self.user_data)]
        if confirm:
            cmd.append("--confirm-redistributable")
        cmd.extend(extra)
        return subprocess.run(cmd, capture_output=True, text=True)

    def test_host_can_publish_active_mod_archive_and_repeat_without_replacement(self):
        first = self.run_share()
        self.assertEqual(first.returncode, 0, first.stderr)
        archive = self.user_data / "lan-mod-shares" / "fixture.zip"
        self.assertTrue(archive.is_file())
        with zipfile.ZipFile(archive) as contents:
            self.assertEqual(set(contents.namelist()),
                             {"fixture/mod.json", "fixture/assets/blob.bin"})
            self.assertGreater(contents.getinfo("fixture/assets/blob.bin").file_size, 64 * 1024)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertIn(digest, first.stdout)
        repeat = self.run_share()
        self.assertEqual(repeat.returncode, 0, repeat.stderr)
        self.assertIn("Already shared", repeat.stdout)
        self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), digest)

    def test_changed_mod_requires_explicit_share_replacement(self):
        self.assertEqual(self.run_share().returncode, 0)
        archive = self.user_data / "lan-mod-shares" / "fixture.zip"
        old = archive.read_bytes()
        (self.mod / "mod.json").write_text('{"name":"Updated LAN fixture"}', encoding="utf-8")
        rejected = self.run_share()
        self.assertNotEqual(rejected.returncode, 0)
        self.assertEqual(archive.read_bytes(), old)
        updated = self.run_share("--replace-share")
        self.assertEqual(updated.returncode, 0, updated.stderr)
        self.assertNotEqual(archive.read_bytes(), old)

    def test_explicit_redistribution_consent_required(self):
        self.assertNotEqual(self.run_share(confirm=False).returncode, 0)
        self.assertFalse((self.user_data / "lan-mod-shares").exists())

    def test_refuses_original_game_archive_and_symlink(self):
        (self.mod / "H3sprite.lod").write_bytes(b"fake-game-archive")
        self.assertNotEqual(self.run_share().returncode, 0)
        (self.mod / "H3sprite.lod").unlink()
        (self.mod / "link").symlink_to(self.mod / "mod.json")
        self.assertNotEqual(self.run_share().returncode, 0)


if __name__ == "__main__":
    unittest.main()
