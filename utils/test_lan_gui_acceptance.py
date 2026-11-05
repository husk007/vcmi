"""Checks that the opt-in two-GUI acceptance setup never changes personal game data."""
import json
from pathlib import Path
from unittest.mock import patch
import subprocess
import sys
import tempfile
import unittest
import zipfile

from utils.lan_gui_acceptance import (acceptance_environment, generate_synthetic_start_map,
                                      owned_map_resource, required_gui_tests_passed, stage_fixture)

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

    def test_licensed_acceptance_checks_real_mod_loading_in_isolated_paths(self):
        workspace = self.base / "session"
        config = workspace / "config"
        env = acceptance_environment(workspace, config)
        self.assertEqual(env["VCMI_LAN_TEST_GUI_CLIENT"], "1")
        self.assertEqual(env["VCMI_LAN_TEST_COMPLETE_H3_DATA"], "1")
        self.assertEqual(env["XDG_CONFIG_HOME"], str(config))
        self.assertEqual(env["XDG_DATA_HOME"], str(workspace / "share"))
        self.assertEqual(env["XDG_CACHE_HOME"], str(workspace / "cache"))

    def test_optional_game_start_only_uses_owned_maps(self):
        maps = self.owned / "Maps"
        maps.mkdir()
        (maps / "Three Players.h3m").write_bytes(b"owned map placeholder")
        self.assertEqual(owned_map_resource(self.owned, "Maps/Three Players.h3m"),
                         "Maps/Three Players.h3m")
        self.assertEqual(owned_map_resource(self.owned, "Three Players.h3m"),
                         "Maps/Three Players.h3m")
        with self.assertRaises(ValueError):
            owned_map_resource(self.owned, "../outside.h3m")
        with self.assertRaises((ValueError, OSError)):
            owned_map_resource(self.owned, "missing.h3m")
        with patch.dict("os.environ", {"VCMI_LAN_TEST_START_MAP": "Maps/stale.h3m"}):
            env = acceptance_environment(self.base / "sandbox", self.base / "config")
        self.assertNotIn("VCMI_LAN_TEST_START_MAP", env)

    def test_generated_gui_map_stays_off_the_owners_maps_directory(self):
        owned_maps = self.owned / "Maps"
        owned_maps.mkdir()
        personal_map = owned_maps / "personal.h3m"
        personal_map.write_bytes(b"owned original map")
        workspace = self.base / "session"
        user_data, config = stage_fixture(self.owned, workspace)
        self.assertTrue((user_data / "Maps").is_symlink())

        generated = workspace / "generated-map-cache" / "vcmi" / "Maps" / "LanThreeHumanStartMap.h3m"

        def fake_map_test(command, **options):
            self.assertIn("--gtest_filter=TinyH3MBuilderTest.ThreeHumanLanStartMap", command)
            self.assertEqual(options["env"]["XDG_CACHE_HOME"],
                             str(workspace / "generated-map-cache"))
            generated.parent.mkdir(parents=True, exist_ok=True)
            generated.write_bytes(b"synthetic three-human map")
            return subprocess.CompletedProcess(command, 0)

        with patch("utils.lan_gui_acceptance.subprocess.run", side_effect=fake_map_test) as run_test:
            resource = generate_synthetic_start_map(
                self.base / "bin", user_data, workspace, acceptance_environment(workspace, config))
        self.assertEqual(run_test.call_count, 1)
        self.assertEqual(resource, "Maps/LanThreeHumanStartMap.h3m")
        self.assertFalse((user_data / "Maps").is_symlink())
        self.assertEqual((user_data / resource).read_bytes(), b"synthetic three-human map")
        self.assertEqual(personal_map.read_bytes(), b"owned original map")
        self.assertFalse((owned_maps / "LanThreeHumanStartMap.h3m").exists())

    def test_generator_missing_map_rejects_gui_game_start(self):
        workspace = self.base / "session"
        user_data, config = stage_fixture(self.owned, workspace)
        with patch("utils.lan_gui_acceptance.subprocess.run",
                   return_value=subprocess.CompletedProcess([], 0)):
            with self.assertRaisesRegex(ValueError, "did not generate"):
                generate_synthetic_start_map(
                    self.base / "bin", user_data, workspace, acceptance_environment(workspace, config))

    def test_gui_acceptance_rejects_skips_even_on_green_gtest_exit(self):
        lobby = "LanModSyncTest.RealIndependentVCMIServerProvidesLegacyAndOptInLobbyPreview"
        parsed_mod = "LanModSyncTest.RealDesktopClientMountsCachedHostZipThenRecoversPersonalPreset"
        both_passed = f"[       OK ] {lobby} (250 ms)\n[       OK ] {parsed_mod} (300 ms)"
        self.assertTrue(required_gui_tests_passed(both_passed))
        self.assertTrue(required_gui_tests_passed("\x1b[0m" + both_passed))
        self.assertFalse(required_gui_tests_passed(f"[       OK ] {lobby} (250 ms)\n"
                                               f"[  SKIPPED ] {parsed_mod} (0 ms)"))
        self.assertFalse(required_gui_tests_passed("[==========] 0 tests ran. (0 ms total)"))

    def test_requires_owned_data_and_does_not_touch_workspace_on_failure(self):
        (self.owned / "Data" / "H3sprite.lod").unlink()
        workspace = self.base / "session"
        with self.assertRaisesRegex(ValueError, "original Heroes III"):
            stage_fixture(self.owned, workspace)
        self.assertFalse(workspace.exists())


if __name__ == "__main__":
    unittest.main()
