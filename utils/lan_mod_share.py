#!/usr/bin/env python3
"""Publish an explicitly redistributable installed mod for VCMI LAN host transfer."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
import zipfile

MAX_ARCHIVE_BYTES = 2 * 1024**3
MAX_UNPACKED_BYTES = 512 * 1024**2
MAX_ENTRY_BYTES = 128 * 1024**2
MAX_ENTRIES = 8192
# Never publish original Heroes III archives or executable installers.
FORBIDDEN_SUFFIXES = {".lod", ".snd", ".vid", ".exe", ".iso", ".cab"}


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def collect_mod_files(mod_id, mods_dir):
    if not re.fullmatch(r"[a-z0-9_-]+", mod_id):
        raise ValueError("root mod ID must contain only lowercase letters, digits, '-' or '_'")
    root = mods_dir / mod_id
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f"installed mod directory not found: {root}")
    definition = root / "mod.json"
    if definition.is_symlink() or not definition.is_file():
        raise ValueError(f"root mod.json is missing: {definition}")

    files = []
    total = 0
    folded = set()
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root)
        for part in relative.parts:
            if (not part or part in {".", ".."} or part.endswith((".", " "))
                    or any(ord(c) < 32 or ord(c) == 127 or c in '\\:<>|*?' for c in part)):
                raise ValueError(f"unsupported mod package path: {relative}")
        metadata = path.lstat()
        if stat.S_ISLNK(metadata.st_mode):
            raise ValueError(f"mod contains a symbolic link: {relative}")
        if stat.S_ISDIR(metadata.st_mode):
            continue
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError(f"mod contains a non-regular file: {relative}")
        if path.suffix.lower() in FORBIDDEN_SUFFIXES:
            raise ValueError(f"refusing to share potential original Heroes III data: {relative}")
        if metadata.st_size > MAX_ENTRY_BYTES:
            raise ValueError(f"individual mod file exceeds LAN extraction limit: {relative}")
        total += metadata.st_size
        if total > MAX_UNPACKED_BYTES:
            raise ValueError("mod exceeds LAN unpacked-size limit")
        name = relative.as_posix().casefold()
        if name in folded:
            raise ValueError(f"case-insensitive duplicate mod path: {relative}")
        folded.add(name)
        files.append((path, f"{mod_id}/{relative.as_posix()}"))
        if len(files) > MAX_ENTRIES:
            raise ValueError("mod has too many files for LAN staging")
    return files


def publish(mod_id, mods_dir, user_data, replace=False):
    files = collect_mod_files(mod_id, mods_dir)
    if user_data.is_symlink() or not user_data.is_dir():
        raise ValueError(f"VCMI user-data directory not found: {user_data}")
    shares = user_data / "lan-mod-shares"
    shares.mkdir(exist_ok=True)
    if shares.is_symlink() or not shares.is_dir():
        raise ValueError("LAN share directory must not be a symbolic link")
    destination = shares / f"{mod_id}.zip"
    if destination.is_symlink():
        raise ValueError("refusing to replace a symbolic-link share")

    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=shares, prefix=f".{mod_id}-", suffix=".part", delete=False) as target:
            temporary = Path(target.name)
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for source, archive_name in files:
                archive.write(source, archive_name)
        if temporary.stat().st_size <= 0 or temporary.stat().st_size > MAX_ARCHIVE_BYTES:
            raise ValueError("generated archive exceeds the LAN transfer size limit")
        new_hash = sha256_file(temporary)
        if destination.exists():
            if not destination.is_file():
                raise ValueError("existing share is not a regular file")
            if sha256_file(destination) == new_hash:
                return destination, new_hash, False
            if not replace:
                raise ValueError("a different share already exists; use --replace-share to update it")
        os.replace(temporary, destination)
        temporary = None
        return destination, new_hash, True
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def active_mod_roots(settings_path):
    """Return unique installed roots from the current VCMI preset."""
    try:
        settings = json.loads(settings_path.read_text(encoding="utf-8"))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid VCMI mod settings: {error}") from error
    if not isinstance(settings, dict):
        raise ValueError("VCMI mod settings must be a JSON object")
    presets = settings.get("presets")
    active = settings.get("activePreset")
    if not isinstance(presets, dict) or not isinstance(active, str) or active not in presets:
        raise ValueError("active VCMI mod preset is missing")
    selected = presets[active]
    if not isinstance(selected, dict) or not isinstance(selected.get("mods"), list):
        raise ValueError("active VCMI mod preset has no mod list")
    roots = []
    for mod_id in selected["mods"]:
        if not isinstance(mod_id, str):
            raise ValueError("active preset contains a non-string mod ID")
        root_id = mod_id.split(".", 1)[0]
        if root_id in {"vcmi", "core"}:
            continue
        if not re.fullmatch(r"[a-z0-9_-]+", root_id):
            raise ValueError(f"invalid active root mod ID: {mod_id}")
        if root_id not in roots:
            roots.append(root_id)
    if not roots:
        raise ValueError("active preset has no shareable root mods")
    return roots


def client_launch_environment(args):
    """Launch Linux VCMI with the exact preset and data directory we published."""
    environment = os.environ.copy()
    if sys.platform != "linux":
        return environment

    # VCMIDirsXDG always resolves userDataPath() to XDG_DATA_HOME/vcmi.
    data_dir = args.user_data.expanduser().resolve(strict=True)
    if data_dir.name != "vcmi":
        raise ValueError("--launch-client requires --user-data ending in /vcmi on Linux")
    if args.mods_dir is not None:
        selected_mods = args.mods_dir.expanduser().resolve(strict=True)
        if selected_mods != (data_dir / "Mods").resolve(strict=True):
            raise ValueError("--launch-client cannot use --mods-dir outside its VCMI user-data")
    environment["XDG_DATA_HOME"] = str(data_dir.parent)

    if args.active_from_settings is not None:
        settings = args.active_from_settings.expanduser().resolve(strict=True)
        if settings.name != "modSettings.json" or settings.parent.name != "vcmi":
            raise ValueError(
                "--launch-client requires --active-from-settings at "
                "XDG_CONFIG_HOME/vcmi/modSettings.json on Linux"
            )
        # VCMIDirsXDG uses XDG_CONFIG_HOME/vcmi, not the data directory.
        environment["XDG_CONFIG_HOME"] = str(settings.parent.parent)
    return environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mod_id", nargs="?", help="canonical installed root mod ID")
    parser.add_argument("--active-from-settings", type=Path,
                        help="publish all active root mods from this modSettings.json (skips VCMI core)")
    parser.add_argument("--user-data", type=Path, required=True,
                        help="VCMI user-data folder used by the host (contains Mods/)")
    parser.add_argument("--mods-dir", type=Path,
                        help="installed Mods folder (default: USER_DATA/Mods)")
    parser.add_argument("--confirm-redistributable", action="store_true",
                        help="confirm you have permission to share ALL included mod files")
    parser.add_argument("--replace-share", action="store_true",
                        help="replace an older host share of this mod (never modifies Mods/)")
    parser.add_argument("--launch-client", type=Path,
                        help="start VCMI after publishing the host's active mod archives")
    parser.add_argument("--client-arg", action="append", default=[],
                        help="forward an argument to VCMI (repeat; use --client-arg=--help)")
    args = parser.parse_args()
    if (args.mod_id is None) == (args.active_from_settings is None):
        parser.error("provide either mod_id or --active-from-settings PATH")
    if not args.confirm_redistributable:
        parser.error("--confirm-redistributable is required; do not share commercial Heroes III files")
    if args.client_arg and args.launch_client is None:
        parser.error("--client-arg requires --launch-client")
    executable = None
    launch_env = None
    try:
        if args.launch_client is not None:
            executable = args.launch_client.expanduser().resolve(strict=True)
            if not executable.is_file():
                raise ValueError(f"VCMI client executable is not a file: {executable}")
            # Refuse to package one preset and launch another installation.
            launch_env = client_launch_environment(args)
        mod_ids = ([args.mod_id] if args.mod_id is not None
                   else active_mod_roots(args.active_from_settings))
        mods_dir = args.mods_dir or args.user_data / "Mods"
        # Validate every selected root before publishing the first archive.
        for mod_id in mod_ids:
            collect_mod_files(mod_id, mods_dir)
        # A host launch must share the active preset's current files, even after
        # an installed mod update. Explicit one-off publication stays unchanged.
        refresh_active_shares = args.launch_client is not None and args.active_from_settings is not None
        for mod_id in mod_ids:
            destination, digest, changed = publish(mod_id, mods_dir, args.user_data,
                                                    replace=args.replace_share or refresh_active_shares)
            print(f"{'Shared' if changed else 'Already shared'}: {destination}")
            print(f"SHA-256: {digest}")
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Could not share LAN mod: {error}\n")
    print("The running VCMI host advertises these archives only while their root mods are active.")
    if executable is not None:
        print(f"Starting VCMI client: {executable}")
        sys.stdout.flush()
        try:
            os.execve(str(executable), [str(executable), *args.client_arg], launch_env)
        except OSError as error:
            parser.exit(1, f"Could not launch VCMI client: {error}\n")

if __name__ == "__main__":
    main()
