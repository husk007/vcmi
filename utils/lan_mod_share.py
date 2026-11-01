#!/usr/bin/env python3
"""Publish an explicitly redistributable installed mod for VCMI LAN host transfer."""

import argparse
import hashlib
import os
from pathlib import Path
import re
import stat
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mod_id", help="canonical installed root mod ID")
    parser.add_argument("--user-data", type=Path, required=True,
                        help="VCMI user-data folder used by the host (contains Mods/)")
    parser.add_argument("--mods-dir", type=Path,
                        help="installed Mods folder (default: USER_DATA/Mods)")
    parser.add_argument("--confirm-redistributable", action="store_true",
                        help="confirm you have permission to share ALL included mod files")
    parser.add_argument("--replace-share", action="store_true",
                        help="replace an older host share of this mod (never modifies Mods/)")
    args = parser.parse_args()
    if not args.confirm_redistributable:
        parser.error("--confirm-redistributable is required; do not share commercial Heroes III files")
    try:
        destination, digest, changed = publish(args.mod_id, args.mods_dir or args.user_data / "Mods",
                                                args.user_data, replace=args.replace_share)
    except (OSError, ValueError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Could not share LAN mod: {error}\n")
    print(f"{'Shared' if changed else 'Already shared'}: {destination}")
    print(f"SHA-256: {digest}")
    print("The running VCMI host advertises this archive only while the root mod is active.")


if __name__ == "__main__":
    main()
