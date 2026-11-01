"""Executable checks for publishing an installed VCMI mod into the host share directory."""
import hashlib
import json
import os
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
        self.user_data = Path(self.temp.name) / "vcmi"
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

    def test_active_preset_publishes_unique_roots_without_inactive_mods(self):
        for name in ("second", "inactive"):
            root = self.user_data / "Mods" / name
            root.mkdir()
            (root / "mod.json").write_text('{"name":"A fixture mod"}', encoding="utf-8")
        config = self.user_data / "modSettings.json"
        config.write_text(json.dumps({
            "activePreset": "lan-host",
            "presets": {
                "lan-host": {"mods": ["vcmi", "core", "fixture.addon", "fixture", "second"]},
                "personal": {"mods": ["inactive"]}
            }
        }), encoding="utf-8")
        command = [sys.executable, str(SCRIPT), "--active-from-settings", str(config),
                   "--user-data", str(self.user_data), "--confirm-redistributable"]
        published = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(published.returncode, 0, published.stderr)
        shares = self.user_data / "lan-mod-shares"
        self.assertEqual({path.name for path in shares.iterdir()},
                         {"fixture.zip", "second.zip"})
        self.assertEqual(published.stdout.count("SHA-256:"), 2)
        with zipfile.ZipFile(shares / "fixture.zip") as contents:
            self.assertIn("fixture/mod.json", contents.namelist())
        repeated = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(repeated.returncode, 0, repeated.stderr)
        self.assertEqual(repeated.stdout.count("Already shared:"), 2)

    def test_missing_active_root_refuses_entire_preset_before_writing(self):
        config = self.user_data / "modSettings.json"
        config.write_text(json.dumps({
            "activePreset": "lan-host",
            "presets": {"lan-host": {"mods": ["fixture", "missing-mod"]}}
        }), encoding="utf-8")
        command = [sys.executable, str(SCRIPT), "--active-from-settings", str(config),
                   "--user-data", str(self.user_data), "--confirm-redistributable"]
        rejected = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse((self.user_data / "lan-mod-shares").exists())
        config.write_text('{"activePreset":"does-not-exist","presets":{}}', encoding="utf-8")
        rejected = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse((self.user_data / "lan-mod-shares").exists())


    @unittest.skipIf(sys.platform == "win32", "Executable shebang fixture requires POSIX")
    def test_launch_client_after_publishing_and_never_on_failure(self):
        marker = self.user_data / "launched.txt"
        fake_client = self.user_data / "fake-vcmiclient"
        fake_client.write_text(
            f"#!{sys.executable}\n"
            "import pathlib, sys\n"
            f"pathlib.Path({str(marker)!r}).write_text(','.join(sys.argv[1:]), encoding='utf-8')\n",
            encoding="utf-8")
        fake_client.chmod(0o755)

        launched = self.run_share("--launch-client", str(fake_client),
                                  "--client-arg=--help", "--client-arg=--version")
        self.assertEqual(launched.returncode, 0, launched.stderr)
        self.assertEqual(marker.read_text(encoding="utf-8"), "--help,--version")
        self.assertTrue((self.user_data / "lan-mod-shares" / "fixture.zip").is_file())

        marker.unlink()
        (self.mod / "H3sprite.lod").write_bytes(b"proprietary placeholder")
        rejected = self.run_share("--launch-client", str(fake_client))
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse(marker.exists())

    @unittest.skipUnless(sys.platform == "linux", "Requires Linux VCMIDirsXDG")
    def test_launcher_uses_the_published_host_preset_and_data_directory(self):
        config_home = Path(self.temp.name) / "host-config"
        config_dir = config_home / "vcmi"
        config_dir.mkdir(parents=True)
        host_settings = config_dir / "modSettings.json"
        host_settings.write_text(json.dumps({
            "activePreset": "host",
            "presets": {"host": {"mods": ["vcmi", "fixture"]}}
        }), encoding="utf-8")

        marker = self.user_data / "launch-environment.json"
        fake_client = self.user_data / "fake-vcmiclient"
        fake_client.write_text(
            f"#!{sys.executable}\n"
            "import json, os\n"
            "from pathlib import Path\n"
            f"Path({str(marker)!r}).write_text(json.dumps({{"
            "'data': os.environ.get('XDG_DATA_HOME'), "
            "'config': os.environ.get('XDG_CONFIG_HOME')}))\n",
            encoding="utf-8")
        fake_client.chmod(0o755)

        command = [sys.executable, str(SCRIPT),
                   "--active-from-settings", str(host_settings),
                   "--user-data", str(self.user_data),
                   "--confirm-redistributable",
                   "--launch-client", str(fake_client)]
        environment = os.environ.copy()
        environment["XDG_DATA_HOME"] = str(self.user_data / "wrong-data")
        environment["XDG_CONFIG_HOME"] = str(self.user_data / "wrong-config")
        launched = subprocess.run(command, capture_output=True, text=True, env=environment)
        self.assertEqual(launched.returncode, 0, launched.stderr)
        observed = json.loads(marker.read_text(encoding="utf-8"))
        self.assertEqual(observed, {
            "data": str(self.user_data.parent.resolve()),
            "config": str(config_home.resolve())
        })
        self.assertTrue((self.user_data / "lan-mod-shares" / "fixture.zip").is_file())

    @unittest.skipUnless(sys.platform == "linux", "Requires Linux VCMIDirsXDG")
    def test_launcher_rejects_preset_or_mod_directory_it_cannot_use(self):
        marker = self.user_data / "fake-vcmiclient"
        marker.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        marker.chmod(0o755)

        wrong_settings = self.user_data / "modSettings.json"
        wrong_settings.write_text(json.dumps({
            "activePreset": "host",
            "presets": {"host": {"mods": ["fixture"]}}
        }), encoding="utf-8")
        bad_preset = [sys.executable, str(SCRIPT),
                      "--active-from-settings", str(wrong_settings),
                      "--user-data", str(self.user_data),
                      "--confirm-redistributable", "--launch-client", str(marker)]
        result = subprocess.run(bad_preset, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("XDG_CONFIG_HOME", result.stderr)

        other_mods = self.user_data.parent / "other-mods"
        other_mods.mkdir()
        bad_mod_path = self.run_share("--mods-dir", str(other_mods),
                                     "--launch-client", str(marker))
        self.assertNotEqual(bad_mod_path.returncode, 0)
        self.assertIn("--mods-dir", bad_mod_path.stderr)
        self.assertFalse((self.user_data / "lan-mod-shares").exists())

    def test_client_arguments_require_a_client_executable(self):
        rejected = self.run_share("--client-arg=--help")
        self.assertNotEqual(rejected.returncode, 0)
        self.assertFalse((self.user_data / "lan-mod-shares").exists())

if __name__ == "__main__":
    unittest.main()
