"""Extract an Xbox / Xbox 360 disc image (XDVDFS / XGD) into a folder.

Usage: python extract_iso.py <image.iso> <out_dir> [--only default.xex]
"""
import argparse
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: rebuilt image, XGD1, XGD2, XGD3.
PARTITION_OFFSETS = [0, 0x18300000, 0xFD90000, 0x2080000]


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    sys.exit("No XDVDFS volume found; not an Xbox disc image?")


def walk_dir(f, base, sector, size, path, out, only):
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue  # padding / empty directory
        name = data[off + 14: off + 14 + nlen].decode("latin-1")
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
        rel = os.path.join(path, name)
        if attr & 0x10:
            os.makedirs(os.path.join(out, rel), exist_ok=True)
            walk_dir(f, base, start, length, rel, out, only)
            continue
        if only and name.lower() != only.lower():
            continue
        dest = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        pos = f.tell()
        f.seek(base + start * SECTOR)
        remaining = length
        with open(dest, "wb") as o:
            while remaining:
                chunk = f.read(min(remaining, 8 << 20))
                if not chunk:
                    raise IOError(f"Unexpected end of image reading {rel}")
                o.write(chunk)
                remaining -= len(chunk)
        f.seek(pos)
        print(f"{length:>12}  {rel}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--only", help="extract only files with this name")
    a = ap.parse_args()
    with open(a.image, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        print(f"partition offset 0x{base:X}, root sector {root_sector}, size {root_size}")
        os.makedirs(a.out, exist_ok=True)
        walk_dir(f, base, root_sector, root_size, "", a.out, a.only)


if __name__ == "__main__":
    main()
