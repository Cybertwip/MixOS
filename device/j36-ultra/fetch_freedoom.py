#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0 OR GPL-2.0-or-later
# Copyright (c) 2025-2026 the MixOS project.  MPL-2.0 or GPL-2.0-or-later, at your
# option; see device/j36-ultra/LICENSE for the texts and for what they do not cover.
"""Fetch one Freedoom IWAD for the J36 Ultra fbdoom payload.

doomgeneric is an engine and ships no game data, so something has to supply an
IWAD.  Freedoom is used because it is freely redistributable -- the build can
download it unattended and the result can be handed to anybody -- and because
doomgeneric's own d_iwad.c recognises the filename: its iwads[] table maps
"freedoom1.wad" to (doom, retail) and "freedoom2.wad" to (doom2, commercial).
An IWAD under any other name has to be one of the id filenames in that table or
the engine refuses it, which is why this script writes the member out under the
name it has inside the zip rather than something tidier.

Only the standard library is used on purpose.  The kernel build's apt
dependencies are installed once behind a stamp file, so a build that needed
curl, wget or unzip would silently not have them on a VM created before this
existed.  urllib and zipfile are always there.

The download is pinned by URL and verified against the SHA256 published in the
release's own signed CHECKSUM file, so a truncated transfer or a moved tag is a
failure here and not a corrupt WAD on the card.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

# Freedoom 0.13.0, released 2024-01-29.  The digest is the one in
# freedoom-0.13.0-CHECKSUM, which is PGP-signed by the Freedoom maintainers:
#   SHA256 (freedoom-0.13.0.zip) = 3f9b264f...
DEFAULT_VERSION = "0.13.0"
DEFAULT_SHA256 = "3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59"
DEFAULT_MEMBER = "freedoom1.wad"


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, target: Path) -> None:
    partial = target.with_suffix(target.suffix + ".part")
    print(f"[freedoom] downloading {url}")
    with urllib.request.urlopen(url, timeout=120) as response, partial.open("wb") as out:
        shutil.copyfileobj(response, out, length=1 << 20)
    partial.replace(target)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default=DEFAULT_VERSION)
    parser.add_argument("--sha256", default=DEFAULT_SHA256)
    parser.add_argument("--member", default=DEFAULT_MEMBER,
                        help="WAD to extract from the archive")
    parser.add_argument("--cache", type=Path, required=True,
                        help="directory the archive is kept in between builds")
    parser.add_argument("--out", type=Path, required=True,
                        help="path to write the WAD to")
    parser.add_argument("--url", default=None,
                        help="override the release URL entirely")
    args = parser.parse_args()

    if args.out.is_file() and args.out.stat().st_size > 0:
        print(f"[freedoom] {args.out} is already there; leaving it alone")
        return 0

    args.cache.mkdir(parents=True, exist_ok=True)
    archive = args.cache / f"freedoom-{args.version}.zip"
    url = args.url or (
        "https://github.com/freedoom/freedoom/releases/download/"
        f"v{args.version}/freedoom-{args.version}.zip"
    )

    if archive.is_file():
        # A cached archive is still checked: the cache outlives the build tree
        # and a half-written file from an interrupted run looks identical to a
        # good one until the digest is taken.
        if sha256_of(archive) != args.sha256:
            print(f"[freedoom] cached {archive.name} has the wrong digest; refetching")
            archive.unlink()

    if not archive.is_file():
        try:
            download(url, archive)
        except Exception as error:  # network, DNS, HTTP status, disk
            print(f"[freedoom] download failed: {error}", file=sys.stderr)
            return 1

    actual = sha256_of(archive)
    if actual != args.sha256:
        print(f"[freedoom] {archive.name} sha256 {actual}\n"
              f"[freedoom] expected      {args.sha256}", file=sys.stderr)
        return 1

    with zipfile.ZipFile(archive) as zf:
        members = [n for n in zf.namelist() if n.endswith("/" + args.member)
                   or n == args.member]
        if not members:
            print(f"[freedoom] {args.member} is not in {archive.name}", file=sys.stderr)
            return 1
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with zf.open(members[0]) as src, args.out.open("wb") as dst:
            shutil.copyfileobj(src, dst, length=1 << 20)

    print(f"[freedoom] {args.out} ({args.out.stat().st_size} bytes) from {members[0]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
