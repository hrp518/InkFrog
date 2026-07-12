#!/usr/bin/env python3
"""
Build .l1glyf compact glyph cache for FontExp tiny_ttf L1 preload.

Usage:
  python build_l1glyf_cache.py path/to/SourceHanSerifCN-Regular.ttf
  python build_l1glyf_cache.py path/to/font.ttf -o path/to/font.l1glyf

Requires: pip install fonttools
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

L1GLYF_MAGIC = 0x3146474C  # "LGF1" little-endian
L1GLYF_VERSION = 1

ASCII_START = 0x20
ASCII_END = 0x7E

PRIO_CJK_PUNCT = [
    0x3002, 0x3001, 0x3000, 0x3010, 0x3011, 0x2014, 0x201C, 0x201D,
    0x2026, 0x00B7, 0xFF0C, 0xFF0E, 0xFF01, 0xFF1F, 0xFF1B, 0xFF1A,
    0xFF08, 0xFF09, 0xFF3B, 0xFF3D, 0x300A, 0x300B, 0x2018, 0x2019,
]

PRIO_ASCII_PUNCT = list(b'.,;:!?\"\'()[]-_+=*/\\@#$%&<>{}|~`^')


def parse_level1_chars(level1_c: Path) -> list[int]:
    text = level1_c.read_text(encoding="utf-8", errors="ignore")
    nums = re.findall(r"0x[0-9A-Fa-f]+", text)
    return [int(x, 16) for x in nums]


def build_preload_list(level1: list[int]) -> list[int]:
    out: list[int] = []
    seen: set[int] = set()

    def add(cp: int) -> None:
        if cp not in seen:
            seen.add(cp)
            out.append(cp)

    for c in range(ord("0"), ord("9") + 1):
        add(c)
    for c in range(0xFF10, 0xFF19 + 1):
        add(c)
    for cp in PRIO_ASCII_PUNCT:
        add(cp)
    for cp in PRIO_CJK_PUNCT:
        add(cp)
    for c in range(ASCII_START, ASCII_END + 1):
        add(c)
    for cp in level1:
        add(cp)
    return out


def build_l1glyf(ttf_path: Path, level1_c: Path, out_path: Path) -> dict:
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("ERROR: fontTools not installed. Run: pip install fonttools", file=sys.stderr)
        sys.exit(1)

    level1 = parse_level1_chars(level1_c)
    unicodes = build_preload_list(level1)

    ttf_bytes = ttf_path.read_bytes()
    ttf_size = len(ttf_bytes)
    ttf_crc32 = zlib.crc32(ttf_bytes) & 0xFFFFFFFF

    font = TTFont(str(ttf_path))
    cmap = font.getBestCmap()
    glyf = font["glyf"]
    hmtx = font["hmtx"]

    entries = []
    glyf_blob = bytearray()
    not_found = 0
    empty = 0

    for u in unicodes:
        gn = cmap.get(u)
        if gn is None:
            not_found += 1
            continue
        if isinstance(gn, str):
            gid = font.getGlyphID(gn)
        else:
            gid = int(gn)
        gname = font.getGlyphName(gid)
        g = glyf[gname]
        if g.numberOfContours == 0:
            empty += 1
            continue
        data = g.compile(glyf)
        if not data:
            empty += 1
            continue
        off = len(glyf_blob)
        sz = len(data)
        glyf_blob.extend(data)
        entries.append((int(u), gid, off, sz))

    # Sort by glyph_index for device binary search
    entries.sort(key=lambda e: e[1])

    lookup_count = len(entries)
    lookup_bytes = lookup_count * 16  # l1glyf_entry_t: u32 unicode, u16 gid, u16 pad, u32 off, u32 size
    header_size = 32
    lookup_offset = header_size + 4  # after header_crc32
    data_offset = lookup_offset + lookup_bytes

    header_wo_crc = struct.pack(
        "<IHHIIIIII",
        L1GLYF_MAGIC,
        L1GLYF_VERSION,
        header_size,
        ttf_size,
        ttf_crc32,
        lookup_count,
        len(glyf_blob),
        lookup_offset,
        data_offset,
    )
    header_crc32 = zlib.crc32(header_wo_crc) & 0xFFFFFFFF
    header = header_wo_crc + struct.pack("<I", header_crc32)

    lookup = bytearray()
    for u, gid, off, sz in entries:
        lookup.extend(struct.pack("<IHHII", u, gid, 0, off, sz))

    out_path.write_bytes(header + bytes(lookup) + bytes(glyf_blob))

    stats = {
        "ttf_path": str(ttf_path),
        "out_path": str(out_path),
        "ttf_size": ttf_size,
        "ttf_crc32": f"0x{ttf_crc32:08X}",
        "unicode_listed": len(unicodes),
        "lookup_count": lookup_count,
        "not_found": not_found,
        "empty": empty,
        "glyf_bytes": len(glyf_blob),
        "lookup_bytes": len(lookup),
        "total_bytes": out_path.stat().st_size,
    }
    return stats


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    level1_c = root / "level1_data.c"

    ap = argparse.ArgumentParser(description="Build .l1glyf cache from TTF + level1_data.c")
    ap.add_argument("ttf", type=Path, help="Input .ttf path")
    ap.add_argument("-o", "--output", type=Path, default=None, help="Output .l1glyf path")
    args = ap.parse_args()

    ttf_path = args.ttf.resolve()
    if not ttf_path.is_file():
        print(f"ERROR: TTF not found: {ttf_path}", file=sys.stderr)
        sys.exit(1)

    out_path = args.output
    if out_path is None:
        out_path = ttf_path.with_suffix(".l1glyf")
    else:
        out_path = out_path.resolve()

    stats = build_l1glyf(ttf_path, level1_c, out_path)

    print(f"TTF:     {stats['ttf_path']}")
    print(f"        size={stats['ttf_size']:,} bytes  crc32={stats['ttf_crc32']}")
    print(f"Output:  {stats['out_path']}")
    print(f"        total={stats['total_bytes']:,} bytes ({stats['total_bytes']/1024/1024:.3f} MB)")
    print(f"        lookup={stats['lookup_count']} entries ({stats['lookup_bytes']:,} bytes)")
    print(f"        glyf_data={stats['glyf_bytes']:,} bytes ({stats['glyf_bytes']/1024/1024:.3f} MB)")
    print(f"        listed={stats['unicode_listed']} not_found={stats['not_found']} empty={stats['empty']}")


if __name__ == "__main__":
    main()
