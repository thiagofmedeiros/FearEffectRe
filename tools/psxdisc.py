#!/usr/bin/env python3
"""Read PlayStation disc images (BIN/CUE, MODE2/2352 or 2048-byte ISO).

    psxdisc.py list    <image>                 list every file with LBA and size
    psxdisc.py extract <image> <outdir> [glob] extract files (2048-byte user data)
    psxdisc.py cat     <image> <path>          write one file to stdout

Files flagged as XA/form 2 (movies, streamed audio) are extracted as raw
2352-byte sectors when --raw-xa is given, otherwise as their 2048-byte view.
"""
from __future__ import annotations

import argparse
import fnmatch
import os
import struct
import sys
from dataclasses import dataclass

SYNC = bytes([0x00] + [0xFF] * 10 + [0x00])


@dataclass
class Entry:
    path: str
    lba: int
    size: int
    is_dir: bool
    xa_attr: int  # CD-XA attribute word from the system use area, 0 if absent

    @property
    def is_form2(self) -> bool:
        return bool(self.xa_attr & 0x1000) or bool(self.xa_attr & 0x4000)


class Disc:
    def __init__(self, path: str):
        if path.lower().endswith(".cue"):
            path = self._bin_from_cue(path)
        self.path = path
        self.f = open(path, "rb")
        head = self.f.read(16)
        self.raw = head[:12] == SYNC
        self.sector_size = 2352 if self.raw else 2048
        self.f.seek(0, os.SEEK_END)
        self.sectors = self.f.tell() // self.sector_size

    @staticmethod
    def _bin_from_cue(cue: str) -> str:
        with open(cue, "r", encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if line.upper().startswith("FILE"):
                    name = line.split('"')[1]
                    return os.path.join(os.path.dirname(cue), name)
        raise ValueError(f"no FILE entry in {cue}")

    def raw_sector(self, lba: int) -> bytes:
        self.f.seek(lba * self.sector_size)
        return self.f.read(self.sector_size)

    def sector(self, lba: int) -> bytes:
        """2048 bytes of user data."""
        data = self.raw_sector(lba)
        if not self.raw:
            return data
        mode = data[15]
        if mode == 2:
            return data[24:24 + 2048]
        return data[16:16 + 2048]

    def read(self, lba: int, size: int) -> bytes:
        out = bytearray()
        count = (size + 2047) // 2048
        for i in range(count):
            out += self.sector(lba + i)
        return bytes(out[:size])

    def read_raw(self, lba: int, size: int) -> bytes:
        count = (size + 2047) // 2048
        self.f.seek(lba * self.sector_size)
        return self.f.read(count * self.sector_size)

    # ---- ISO9660 -------------------------------------------------------
    def volume(self) -> dict:
        pvd = self.sector(16)
        if pvd[1:6] != b"CD001":
            raise ValueError("no ISO9660 primary volume descriptor")
        return {
            "system_id": pvd[8:40].decode("ascii", "replace").strip(),
            "volume_id": pvd[40:72].decode("ascii", "replace").strip(),
            "volume_sectors": struct.unpack_from("<I", pvd, 80)[0],
            "root_lba": struct.unpack_from("<I", pvd, 156 + 2)[0],
            "root_size": struct.unpack_from("<I", pvd, 156 + 10)[0],
        }

    def walk(self) -> list[Entry]:
        vol = self.volume()
        out: list[Entry] = []
        self._walk_dir(vol["root_lba"], vol["root_size"], "", out)
        return out

    def _walk_dir(self, lba: int, size: int, prefix: str, out: list[Entry]) -> None:
        data = self.read(lba, size)
        pos = 0
        while pos < len(data):
            length = data[pos]
            if length == 0:
                # records never straddle a sector; skip to the next one
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + length]
            pos += length
            ext_lba, ext_size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
            flags = rec[25]
            name_len = rec[32]
            name = rec[33:33 + name_len]
            if name in (b"\x00", b"\x01"):
                continue
            su = 33 + name_len + (1 - name_len % 2)
            xa_attr = 0
            if len(rec) >= su + 14 and rec[su + 6:su + 8] == b"XA":
                xa_attr = struct.unpack_from(">H", rec, su + 4)[0]
            text = name.decode("ascii", "replace").split(";")[0]
            path = f"{prefix}{text}"
            is_dir = bool(flags & 2)
            out.append(Entry(path, ext_lba, ext_size, is_dir, xa_attr))
            if is_dir:
                self._walk_dir(ext_lba, ext_size, path + "/", out)

    def find(self, path: str) -> Entry:
        want = path.replace("\\", "/").lstrip("/").upper()
        for e in self.walk():
            if e.path.upper() == want:
                return e
        raise FileNotFoundError(path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("image")
    p = sub.add_parser("extract")
    p.add_argument("image")
    p.add_argument("outdir")
    p.add_argument("glob", nargs="?", default="*")
    p.add_argument("--raw-xa", action="store_true", help="keep 2352-byte sectors for XA/form 2 files")
    p = sub.add_parser("cat")
    p.add_argument("image")
    p.add_argument("path")
    args = ap.parse_args()

    disc = Disc(args.image)
    if args.cmd == "list":
        vol = disc.volume()
        print(f"# {os.path.basename(disc.path)}  sectors={disc.sectors} raw={disc.raw} "
              f"system='{vol['system_id']}' volume='{vol['volume_id']}'")
        for e in disc.walk():
            kind = "D" if e.is_dir else ("X" if e.is_form2 else "F")
            print(f"{kind} {e.lba:7d} {e.size:10d} {e.xa_attr:04x} {e.path}")
        return 0
    if args.cmd == "cat":
        e = disc.find(args.path)
        sys.stdout.buffer.write(disc.read(e.lba, e.size))
        return 0
    if args.cmd == "extract":
        for e in disc.walk():
            if e.is_dir or not fnmatch.fnmatch(e.path.upper(), args.glob.upper()):
                continue
            dest = os.path.join(args.outdir, *e.path.split("/"))
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            data = disc.read_raw(e.lba, e.size) if (args.raw_xa and e.is_form2 and disc.raw) else disc.read(e.lba, e.size)
            with open(dest, "wb") as fh:
                fh.write(data)
            print(f"{e.size:10d} {e.path}")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
