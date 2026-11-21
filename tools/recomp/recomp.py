#!/usr/bin/env python3
"""Recompile the game's boot executable to C++.

    recomp.py --disc "<Fear Effect (USA) (Disc 1).cue>" --out <dir> [--shards N]
    recomp.py --exe  SLUS_009.20                        --out <dir>

The output is derived from the game's code. It belongs in the build
directory and must never be committed or distributed.
"""
from __future__ import annotations

import argparse
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.dirname(HERE))

from analyze import discover, load_psx_exe  # noqa: E402
from emit import emit_module  # noqa: E402


def exe_from_disc(image: str) -> bytes:
    from psxdisc import Disc

    disc = Disc(image)
    cnf = disc.find("SYSTEM.CNF")
    text = disc.read(cnf.lba, cnf.size).decode("ascii", "replace")
    boot = None
    for line in text.splitlines():
        if line.upper().replace(" ", "").startswith("BOOT="):
            boot = line.split("=", 1)[1].strip().split("\\")[-1].split(";")[0]
    if not boot:
        raise SystemExit("SYSTEM.CNF has no BOOT line")
    entry = disc.find(boot)
    return disc.read(entry.lba, entry.size)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--disc")
    src.add_argument("--exe")
    ap.add_argument("--out", required=True)
    ap.add_argument("--shards", type=int, default=16)
    args = ap.parse_args()

    if args.disc:
        data = exe_from_disc(args.disc)
        with tempfile.NamedTemporaryFile(delete=False, suffix=".exe") as tmp:
            tmp.write(data)
        try:
            img = load_psx_exe(tmp.name)
        finally:
            os.unlink(tmp.name)
    else:
        img = load_psx_exe(args.exe)

    funcs = discover(img)
    emit_module(img, funcs, args.out, args.shards)
    total = sum(len(f.insns) for f in funcs.values())
    print(f"recomp: {len(funcs)} functions, {total} instructions -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
