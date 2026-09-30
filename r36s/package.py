#!/usr/bin/env python3
"""Assemble the PortMaster zip for the R36S build.

Usage:
    python package.py [--game-bin PATH] [--watch-bin PATH] [--out DIR]

Takes the game from ../aarch64/build/ and the watcher from build/ (build.sh), f3d's licence from
../pc and Wetter's from ../../wetter (override with WETTER).

Layout inside the zip:

    wetrix/
        wetrix.aarch64          the game (../aarch64/build/)
        wetrix-watch            Select+Start exit watcher (build/)
        data/.place-your-rom-here
        LICENSE-fast3d.txt      Fast3D's MIT notice (f3d is derived from it)
        LICENSE-wetter.txt      Wetter's MIT notice (soft and hard)
        LICENSE, NOTICE.md      the GPL v3 text (this project's licence) and the notice for the parts under other licences
        licenses/               the licences of the libraries the runtime bundles
        build-info.txt
        port.json gameinfo.xml
    Wetrix.sh                   launcher

Refuses to include any ROM (.z64/.n64/.v64): the player supplies their own.
Modelled on ge-pc-port/package.py.
"""
import argparse
import os
import datetime
import hashlib
import json
import shutil
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PC = ROOT.parent / "pc"
sys.path.insert(0, str(ROOT.parent / "tools"))
import fetch_deps  # noqa: E402

_DEPS = fetch_deps.resolve_all(clone=False)
WETTER = _DEPS["WETTER_DIR"]
LAUNCHER = ROOT / "launcher" / "Wetrix.sh"
PORT_JSON = ROOT / "launcher" / "port.json"
GAMEINFO = ROOT / "launcher" / "gameinfo.xml"
FAST3D_LICENSE = PC / "fast3d" / "LICENSE-fast3d.txt"
WETTER_LICENSE = WETTER / "LICENSE"
PORTS_LICENSE = ROOT.parent / "LICENSE"
PORTS_NOTICE = ROOT.parent / "NOTICE.md"
RUNTIME = _DEPS["WETRIX_RECOMP_DIR"] / "build" / "n64modernruntime"
RUNTIME_LICENSES = {
    "xxHash.txt": RUNTIME / "thirdparty" / "xxHash" / "LICENSE",
    "miniz.txt": RUNTIME / "thirdparty" / "miniz" / "LICENSE",
    "o1heap.txt": RUNTIME / "thirdparty" / "o1heap" / "LICENSE",
}
DEFAULT_BIN = ROOT.parent / "aarch64" / "build" / "wetrix.aarch64"
DEFAULT_WATCH = ROOT / "build" / "wetrix-watch"
ROM_SUFFIXES = (".z64", ".n64", ".v64")


def die(msg):
    print(f"package.py: error: {msg}", file=sys.stderr)
    sys.exit(1)


def is_aarch64_elf(path):
    with open(path, "rb") as f:
        head = f.read(20)
    return len(head) == 20 and head[:4] == b"\x7fELF" and struct.unpack("<H", head[18:20])[0] == 183


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description="Assemble the PortMaster zip.")
    ap.add_argument("--game-bin", default=str(DEFAULT_BIN))
    ap.add_argument("--watch-bin", default=str(DEFAULT_WATCH))
    ap.add_argument("--out", default=str(ROOT / "dist"))
    args = ap.parse_args()

    game, watch = Path(args.game_bin), Path(args.watch_bin)
    for p, what in ((game, "--game-bin"), (watch, "--watch-bin")):
        if not p.is_file() or not is_aarch64_elf(p):
            die(f"{what} {p}: missing or not an AArch64 ELF")
    for f in (LAUNCHER, PORT_JSON, GAMEINFO, FAST3D_LICENSE, WETTER_LICENSE, PORTS_LICENSE, PORTS_NOTICE,
              *RUNTIME_LICENSES.values()):
        if not f.is_file():
            die(f"missing {f}")
    if b"\r\n" in LAUNCHER.read_bytes():
        die(f"{LAUNCHER} has CRLF line endings")
    zip_name = json.loads(PORT_JSON.read_text(encoding="utf-8"))["name"]

    stage = Path(tempfile.mkdtemp(prefix="wetrix-pkg-"))
    try:
        gd = stage / "wetrix"
        (gd / "data").mkdir(parents=True)
        shutil.copy2(game, gd / "wetrix.aarch64")
        shutil.copy2(watch, gd / "wetrix-watch")
        shutil.copy2(FAST3D_LICENSE, gd / "LICENSE-fast3d.txt")
        shutil.copy2(WETTER_LICENSE, gd / "LICENSE-wetter.txt")
        shutil.copy2(PORTS_LICENSE, gd / "LICENSE")
        shutil.copy2(PORTS_NOTICE, gd / "NOTICE.md")
        (gd / "licenses").mkdir()
        for name, src in RUNTIME_LICENSES.items():
            shutil.copy2(src, gd / "licenses" / name)
        shutil.copy2(PORT_JSON, gd / "port.json")
        shutil.copy2(GAMEINFO, gd / "gameinfo.xml")
        shutil.copy2(LAUNCHER, stage / LAUNCHER.name)
        (gd / "data" / ".place-your-rom-here").write_text(
            "Put your US Wetrix ROM (.z64/.n64/.v64, any name) in wetrix/ or here,\n"
            "then launch. It is checked and adopted as wetrix/wetrix.z64.\n",
            encoding="ascii", newline="\n")
        (gd / "build-info.txt").write_text(
            f"wetrix.aarch64 sha256: {sha256(game)}\n"
            f"wetrix-watch   sha256: {sha256(watch)}\n"
            f"built-utc: {datetime.datetime.now(datetime.timezone.utc):%Y-%m-%dT%H:%M:%SZ}\n",
            encoding="ascii", newline="\n")

        bad = [p for p in stage.rglob("*") if p.is_file() and p.suffix.lower() in ROM_SUFFIXES]
        if bad:
            die("refusing to ship ROM data: " + ", ".join(map(str, bad)))

        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        zpath = out / zip_name
        if zpath.exists():
            zpath.unlink()
        executable = {gd / "wetrix.aarch64", gd / "wetrix-watch", stage / LAUNCHER.name}
        with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in sorted(stage.rglob("*")):
                if not p.is_file():
                    continue
                zi = zipfile.ZipInfo(p.relative_to(stage).as_posix())
                zi.external_attr = (0o755 if p in executable else 0o644) << 16
                zi.compress_type = zipfile.ZIP_DEFLATED
                z.writestr(zi, p.read_bytes())
        with zipfile.ZipFile(zpath) as z:
            names = z.namelist()
        for need in ("wetrix/wetrix.aarch64", "wetrix/wetrix-watch", "Wetrix.sh", "wetrix/port.json"):
            if need not in names:
                die(f"{need} missing from zip")
        print(f"package.py: wrote {zpath} ({zpath.stat().st_size} bytes, {len(names)} files)")
        print(f"package.py: wetrix.aarch64 sha256 {sha256(game)}")
    finally:
        shutil.rmtree(stage, ignore_errors=True)


if __name__ == "__main__":
    main()
