#!/usr/bin/env python3
"""Where the repositories the ports build against are, cloning them when missing.

    python tools/fetch_deps.py [--shell] [--no-clone]

Prints where the recompilation and Wetter are:

    WETRIX_RECOMP_DIR=<path>     (wetrix-n64-recompilation)
    WETTER_DIR=<path>            (wetter)

--shell prints them as `export` lines for `eval "$(python tools/fetch_deps.py --shell)"`,
which is how the build scripts use it. Each dependency is looked for in this order:

  1. the folder named by its environment variable (WETRIX_RECOMP_DIR, WETTER_DIR);
  2. a sibling folder next to this repository (../<name>), the layout of a
     developer who has cloned everything side by side;
  3. this repository's own deps/ folder.

If none exists it is cloned into deps/ from $WETRIX_GIT_BASE/<name> (default
https://github.com/wetrix-enjoyers), unless --no-clone. A folder that already exists
is used as it is: nothing is pulled and nothing is checked out over it. The
recompilation then finds the disassembly the same way (its tools/deps.py).
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # the wetrix-n64-ports repository

GIT_BASE = os.environ.get("WETRIX_GIT_BASE", "https://github.com/wetrix-enjoyers").rstrip("/")

# The branch, tag or commit a fresh clone is put on. Pin a tag or commit here for a
# release so that the same source always builds the same way.
REF = "main"

DEPENDENCIES = {  # environment variable -> repository name
    "WETRIX_RECOMP_DIR": "wetrix-n64-recompilation",
    "WETTER_DIR": "wetter",
}


def locate(name: str, env_var: str) -> Path | None:
    override = os.environ.get(env_var)
    if override:
        return Path(override).resolve()
    for candidate in (ROOT.parent / name, ROOT / "deps" / name):
        if candidate.is_dir():
            return candidate.resolve()
    return None


def resolve(name: str, env_var: str, clone: bool = True) -> Path:
    found = locate(name, env_var)
    if found is not None:
        return found
    dest = ROOT / "deps" / name
    if not clone:
        return dest
    url = f"{GIT_BASE}/{name}"
    print(f"deps: {name} not found; cloning {url} into {dest}", file=sys.stderr, flush=True)
    dest.parent.mkdir(exist_ok=True)
    subprocess.run(["git", "clone", url, str(dest)], check=True, stdout=sys.stderr)
    subprocess.run(["git", "-C", str(dest), "checkout", REF], check=True, stdout=sys.stderr)
    return dest.resolve()


def resolve_all(clone: bool = True) -> dict[str, Path]:
    return {var: resolve(name, var, clone) for var, name in DEPENDENCIES.items()}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--shell", action="store_true", help="print export lines")
    ap.add_argument("--no-clone", action="store_true", help="only report where they are")
    args = ap.parse_args()
    for var, path in resolve_all(clone=not args.no_clone).items():
        print(f"export {var}='{path.as_posix()}'" if args.shell else f"{var}={path.as_posix()}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as e:
        sys.exit(f"fetch_deps: failed ({e.returncode}): {' '.join(map(str, e.cmd))}")
