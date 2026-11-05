#!/usr/bin/env python3
"""Run the opt-in two-desktop-client LAN test using the owner's local Heroes III assets.

Only links to the owner's assets. All mod settings, generated fixture mods,
downloaded archives, cache and logs live in a disposable workspace.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path, PurePosixPath
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def stage_fixture(assets: Path, workspace: Path) -> tuple[Path, Path]:
    assets = assets.expanduser().resolve(strict=True)
    if not assets.is_dir():
        raise ValueError("assets must be an existing VCMI user-data directory")
    source_dirs = {p.name.lower(): p for p in assets.iterdir() if p.is_dir()}
    game_data = source_dirs.get("data")
    if game_data is None or not any(p.suffix.lower() == ".lod" for p in game_data.iterdir() if p.is_file()):
        raise ValueError("assets must contain Data/ with the owner's original Heroes III .lod archives")
    if workspace.exists():
        raise ValueError("workspace must not already exist")
    user_data = workspace / "share" / "vcmi"
    user_data.mkdir(parents=True)
    # Link read-only game resources, not personal Mods, Saves or configuration.
    for name in ("data", "maps", "mp3", "video", "music"):
        source = source_dirs.get(name)
        if source:
            (user_data / source.name).symlink_to(source, target_is_directory=True)

    mods = user_data / "Mods"
    first = mods / "lan-ci-fixture"
    second = mods / "lan-ci-extra"
    (first / "assets").mkdir(parents=True)
    (second / "assets").mkdir(parents=True)
    (second / "Mods" / "units").mkdir(parents=True)
    mod_definitions = (
        (first / "mod.json", "LAN CI installed-only fixture", "1.0"),
        (second / "mod.json", "LAN CI second gameplay fixture", "2.0"),
        (second / "Mods" / "units" / "mod.json", "LAN CI second mod units", "2.0"),
    )
    for path, name, version in mod_definitions:
        path.write_text(json.dumps({
            "name": name, "author": "VCMI LAN integration tests",
            "version": version, "modType": "Mechanics", "keepDisabled": True, "settings": {}
        }), encoding="utf-8")
    # The real server must transfer several chunks, not a tiny trivial ZIP.
    payload = b"".join(hashlib.sha256(f"vcmi-lan-fixture-{i}".encode()).digest() for i in range(2200))
    (first / "assets" / "payload.bin").write_bytes(payload)
    (second / "assets" / "payload.bin").write_bytes(payload)

    host_config = workspace / "config"
    settings = host_config / "vcmi" / "modSettings.json"
    settings.parent.mkdir(parents=True)
    settings.write_text(json.dumps({
        "activePreset": "default",
        "presets": {"default": {
            "mods": ["vcmi", "lan-ci-fixture", "lan-ci-extra"],
            "settings": {"lan-ci-extra": {"units": True}}
        }}
    }), encoding="utf-8")
    return user_data, host_config


def owned_map_resource(assets: Path, requested: str) -> str:
    """Resolve an optional game-start map only from linked owner assets."""
    name = PurePosixPath(requested)
    parts = name.parts
    if not parts or name.is_absolute() or chr(92) in requested or ":" in requested or ".." in parts:
        raise ValueError("start map must be a relative file under the owner's Maps directory")
    if parts[0].lower() == "maps":
        parts = parts[1:]
    if not parts or PurePosixPath(parts[-1]).suffix.lower() not in (".h3m", ".vmap"):
        raise ValueError("start map must name an owned .h3m or .vmap file")
    maps = next((p for p in assets.expanduser().resolve(strict=True).iterdir()
                 if p.is_dir() and p.name.lower() == "maps"), None)
    if maps is None:
        raise ValueError("start map requires the owner's Maps directory")
    root = maps.resolve(strict=True)
    selected = maps.joinpath(*parts).resolve(strict=True)
    if not selected.is_relative_to(root) or not selected.is_file():
        raise ValueError("start map must be inside the owner's Maps directory")
    return "Maps/" + "/".join(parts)



def generate_synthetic_start_map(binaries: Path, user_data: Path, workspace: Path,
                                 env: dict[str, str]) -> str:
    """Build the existing three-human LAN map into the disposable game directory.

    If the owner's Maps directory was linked by stage_fixture, remove only the
    sandbox symlink. Never write generated data into the owner's installation.
    """
    maps = user_data / "Maps"
    if maps.is_symlink():
        maps.unlink()
    maps.mkdir(parents=True, exist_ok=True)
    cache = workspace / "generated-map-cache"
    map_env = dict(env, XDG_CACHE_HOME=str(cache))
    report = workspace / "generated-map.log"
    with report.open("w", encoding="utf-8") as output:
        subprocess.run([
            str(binaries / "vcmitest"),
            "--gtest_filter=TinyH3MBuilderTest.ThreeHumanLanStartMap",
            "--gtest_color=no"
        ], cwd=binaries, env=map_env, stdout=output,
           stderr=subprocess.STDOUT, timeout=120, check=True)
    generated = list(cache.rglob("LanThreeHumanStartMap.h3m"))
    if len(generated) != 1 or not generated[0].is_file() or generated[0].stat().st_size == 0:
        raise ValueError(f"vcmitest did not generate the three-player LAN map; inspect {report}")
    shutil.copyfile(generated[0], maps / "LanThreeHumanStartMap.h3m")
    return "Maps/LanThreeHumanStartMap.h3m"


REQUIRED_GUI_TESTS = (
    "LanModSyncTest.RealIndependentVCMIServerProvidesLegacyAndOptInLobbyPreview",
    "LanModSyncTest.RealDesktopClientMountsCachedHostZipThenRecoversPersonalPreset",
)


def required_gui_tests_passed(log: str) -> bool:
    """A green exit code is insufficient when mandatory GoogleTests were skipped."""
    passed = set()
    prefix = "[       OK ] "
    # Client processes share the output stream and can leave an ANSI colour reset before the line.
    for line in log.splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", line)
        if line.startswith(prefix):
            test_name = line[len(prefix):].split(" ", 1)[0]
            if test_name in REQUIRED_GUI_TESTS:
                passed.add(test_name)
    return len(passed) == len(REQUIRED_GUI_TESTS)


def acceptance_environment(workspace: Path, host_config: Path) -> dict[str, str]:
    """Run only against isolated paths, checking actual mod loading with licensed data."""
    env = os.environ.copy()
    env.pop("VCMI_LAN_TEST_START_MAP", None)
    env.update({
        "XDG_CONFIG_HOME": str(host_config),
        "XDG_DATA_HOME": str(workspace / "share"),
        "XDG_CACHE_HOME": str(workspace / "cache"),
        "VCMI_LAN_TEST_REQUIRED_MOD": "lan-ci-fixture",
        "VCMI_LAN_TEST_SECOND_MOD": "lan-ci-extra",
        "VCMI_LAN_TEST_SECOND_SUBMOD": "lan-ci-extra.units",
        "VCMI_LAN_TEST_GUI_CLIENT": "1",
        "VCMI_LAN_TEST_COMPLETE_H3_DATA": "1",
        "VCMI_LAN_TEST_SHARE_REFRESH": "1",
        "SDL_AUDIODRIVER": "dummy",
    })
    return env


def run(args: argparse.Namespace) -> int:
    binaries = args.bin_dir.expanduser().resolve(strict=True)
    for name in ("vcmitest", "vcmiserver", "vcmiclient"):
        exe = binaries / name
        if not exe.is_file() or not os.access(exe, os.X_OK):
            raise ValueError(f"missing built executable: {exe}")
    if not (binaries / "config" / "filesystem.json").is_file() or not (binaries / "Mods").is_dir():
        raise ValueError("build bin/ needs staged VCMI config/ and Mods/ runtime assets")
    xvfb = shutil.which("xvfb-run")
    if not xvfb:
        raise ValueError("install xvfb and xauth to run two real SDL clients")

    workspace = Path(tempfile.mkdtemp(prefix="vcmi-lan-gui-"))
    # stage_fixture intentionally requires a fresh path.
    workspace.rmdir()
    success = False
    try:
        user_data, host_config = stage_fixture(args.assets_dir, workspace)
        env = acceptance_environment(workspace, host_config)
        if args.start_map:
            env["VCMI_LAN_TEST_START_MAP"] = owned_map_resource(args.assets_dir, args.start_map)
        else:
            # Use the same playable, original three-human map as Linux CI.
            # Two real SDL clients now exercise game initialization by default.
            env["VCMI_LAN_TEST_START_MAP"] = generate_synthetic_start_map(
                binaries, user_data, workspace, env)
        publisher = Path(__file__).with_name("lan_mod_share.py")
        subprocess.run([
            sys.executable, str(publisher), "--active-from-settings",
            str(host_config / "vcmi" / "modSettings.json"),
            "--user-data", str(user_data), "--confirm-redistributable"
        ], env=env, cwd=binaries, check=True)
        for mod in ("lan-ci-fixture", "lan-ci-extra"):
            if (user_data / "lan-mod-shares" / (mod + ".zip")).stat().st_size <= 65536:
                raise ValueError(f"host ZIP for {mod} is too small for the multi-chunk test")
        # vcmitest always activates vcmi-test and sends its identifiers in
        # LobbySetMap, so the host must load the same mod. Publish first, as
        # in Linux CI, so the test mod is not shared as a LAN archive.
        test_mod = binaries / "test" / "testdata" / "vcmi-test"
        if not (test_mod / "mod.json").is_file():
            raise ValueError(f"missing vcmitest fixture mod: {test_mod}")
        shutil.copytree(test_mod, user_data / "Mods" / "vcmi-test")
        settings_path = host_config / "vcmi" / "modSettings.json"
        settings = json.loads(settings_path.read_text(encoding="utf-8"))
        settings["presets"]["default"]["mods"].append("vcmi-test")
        settings_path.write_text(json.dumps(settings), encoding="utf-8")
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env["VCMI_LAN_TEST_SERVER_PORT"] = str(port)
        server_log = workspace / "server.log"
        test_log = workspace / "vcmitest.log"
        with server_log.open("w", encoding="utf-8") as server_out:
            server = subprocess.Popen(
                [str(binaries / "vcmiserver"), "--port", str(port)],
                cwd=binaries, env=env, stdout=server_out, stderr=subprocess.STDOUT)
            try:
                time.sleep(10)  # Avoid probing an empty lobby: it may shut down the server.
                if server.poll() is not None:
                    raise RuntimeError(f"vcmiserver exited early; inspect {server_log}")
                with test_log.open("w", encoding="utf-8") as test_out:
                    completed = subprocess.run([
                        xvfb, "--auto-servernum", "--server-args=-screen 0 1280x720x24",
                        str(binaries / "vcmitest"), "--gtest_filter=LanModSyncTest.*",
                        "--gtest_color=no"
                    ], cwd=binaries, env=env, stdout=test_out,
                       stderr=subprocess.STDOUT, timeout=args.timeout)
                # GoogleTest may exit 0 after skipping required tests. Do not
                # report two GUI clients or real mod loading without both OK lines.
                required_pass = required_gui_tests_passed(test_log.read_text(encoding="utf-8", errors="replace"))
                success = completed.returncode == 0 and required_pass
                if not success:
                    print(f"LAN acceptance failed (vcmitest exit {completed.returncode}; "
                          f"required GUI tests passed: {required_pass}): {test_log}", file=sys.stderr)
                else:
                    print("Real vcmitest LAN fixtures passed, including both GUI lobby clients "
                          "and licensed-data mod-loading checks.")
                    print("This is not LAN_PASS: no complete gameplay match was automated.")
                return 0 if success else (completed.returncode or 1)
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()
    finally:
        if args.keep or not success:
            print(f"LAN test workspace (diagnostics and linked owned assets): {workspace}")
        else:
            shutil.rmtree(workspace)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-dir", type=Path, required=True,
                        help="full VCMI build bin/ with vcmitest, vcmiserver and vcmiclient")
    parser.add_argument("--assets-dir", type=Path, required=True,
                        help="your legally installed VCMI data directory containing Data/")
    parser.add_argument("--start-map",
                        help="override the automatically generated three-human map with an owned "
                             "Maps/*.h3m or *.vmap (>=3 human slots)")
    parser.add_argument("--timeout", type=int, default=900,
                        help="maximum seconds for the vcmitest run (default: 900)")
    parser.add_argument("--keep", action="store_true",
                        help="keep isolated test files and logs even on success")
    args = parser.parse_args()
    try:
        return run(args)
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"LAN GUI acceptance setup failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
