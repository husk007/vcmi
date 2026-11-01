"""Checks that the opt-in two-GUI acceptance setup never changes personal game data."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

from utils.lan_gui_acceptance import stage_fixture

PUBLISHER = Path(__file__).with_name("lan_mod_share.py")


class LanGuiAcceptanceSetupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.owned = self.base / "owned-game"
        (self.owned / "Data").mkdir(parents=True)
        (self.owned / "Data" / "H3sprite.lod").write_bytes(b"original game placeholder")
        (self.owned / "Mods" / "private").mkdir(parents=True)
        (self.owned / "Mods" / "private" / "mod.json").write_text("private", encoding="utf-8")

    def test_isolates_fixture_and_publishes_only_generated_mods(self):
        original = (self.owned / "Mods" / "private" / "mod.json").read_bytes()
        workspace = self.base / "session"
        data, config = stage_fixture(self.owned, workspace)
        self.assertTrue((data / "Data").is_symlink())
        self.assertFalse((data / "Mods" / "private").exists())
        self.assertEqual((self.owned / "Mods" / "private" / "mod.json").read_bytes(), original)
        settings = json.loads((config / "vcmi" / "modSettings.json").read_text())
        self.assertEqual(settings["presets"]["default"]["settings"]["lan-ci-extra"]["units"], True)
        published = subprocess.run([
            sys.executable, str(PUBLISHER), "--active-from-settings",
            str(config / "vcmi" / "modSettings.json"), "--user-data", str(data),
            "--confirm-redistributable"
        ], capture_output=True, text=True)
        self.assertEqual(published.returncode, 0, published.stderr)
        for name in ("lan-ci-fixture", "lan-ci-extra"):
            package = data / "lan-mod-shares" / (name + ".zip")
            self.assertGreater(package.stat().st_size, 65536)
            with zipfile.ZipFile(package) as archive:
                self.assertNotIn("H3sprite.lod", " ".join(archive.namelist()))
                self.assertTrue(all(entry.startswith(name + "/") for entry in archive.namelist()))
        self.assertEqual((self.owned / "Mods" / "private" / "mod.json").read_bytes(), original)

    def test_requires_owned_data_and_does_not_touch_workspace_on_failure(self):
        (self.owned / "Data" / "H3sprite.lod").unlink()
        workspace = self.base / "session"
        with self.assertRaisesRegex(ValueError, "original Heroes III"):
            stage_fixture(self.owned, workspace)
        self.assertFalse(workspace.exists())


if __name__ == "__main__":
    unittest.main()
