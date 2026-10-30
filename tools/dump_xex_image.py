"""Decrypt and decompress a retail XEX2 (basic compression) into its raw memory image.

Writes <out>/image.bin (loaded at the XEX load address) and <out>/blocks.tsv
(one line per page range: name, start, size, r, w, x), the layout tools/harvest_hints.py reads.

Usage: python dump_xex_image.py game/default.xex out/image
"""
import struct
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
FILE_FORMAT_INFO = 0x000003FF


def aes(key: bytes, data: bytes, mode) -> bytes:
    d = Cipher(algorithms.AES(key), mode).decryptor()
    return d.update(data) + d.finalize()


def main(xex_path: str, out_dir: str) -> None:
    x = Path(xex_path).read_bytes()
    if x[:4] != b"XEX2":
        sys.exit("not an XEX2 file")
    _flags, pe_off, _res, sec_off, nhdr = struct.unpack_from(">IIIII", x, 4)
    headers = dict(struct.unpack_from(">II", x, 24 + i * 8) for i in range(nhdr))

    image_size = struct.unpack_from(">I", x, sec_off + 4)[0]
    image_flags, load_addr = struct.unpack_from(">II", x, sec_off + 0x10C)
    enc_key = x[sec_off + 0x150: sec_off + 0x160]
    page_count = struct.unpack_from(">I", x, sec_off + 0x180)[0]
    page_size = 0x1000 if image_flags & 0x10000000 else 0x10000

    ffi = headers[FILE_FORMAT_INFO]
    info_size, enc_type, comp_type = struct.unpack_from(">IHH", x, ffi)
    data = x[pe_off:]
    if enc_type == 1:
        session = aes(RETAIL_KEY, enc_key, modes.ECB())
        data = aes(session, data[: len(data) // 16 * 16], modes.CBC(b"\0" * 16))
    elif enc_type != 0:
        sys.exit(f"unsupported encryption {enc_type}")

    if comp_type == 1:  # basic: (data_size, zero_size) blocks
        out = bytearray()
        pos = 0
        for i in range((info_size - 8) // 8):
            dsz, zsz = struct.unpack_from(">II", x, ffi + 8 + i * 8)
            out += data[pos: pos + dsz]
            out += b"\0" * zsz
            pos += dsz
        image = bytes(out)
    elif comp_type == 0:
        image = data
    else:
        sys.exit(f"unsupported compression {comp_type} (LZX not handled)")
    image = image[:image_size].ljust(image_size, b"\0")
    if image[:2] != b"MZ":
        sys.exit("decrypted image has no MZ header: wrong key or layout")

    od = Path(out_dir)
    od.mkdir(parents=True, exist_ok=True)
    (od / "image.bin").write_bytes(image)

    # Page descriptors: value = (pages << 4) | info; info 1 = code, 2 = data, 3 = read-only.
    rows, addr = [], load_addr
    names = {1: ("code", "true", "false", "true"), 2: ("data", "true", "true", "false"),
             3: ("rdata", "true", "false", "false")}
    for i in range(page_count):
        v = struct.unpack_from(">I", x, sec_off + 0x184 + i * 24)[0]
        pages, info = v >> 4, v & 0xF
        size = pages * page_size
        name, r, w, ex = names.get(info, (f"type{info}", "true", "false", "false"))
        if rows and rows[-1][0] == name and rows[-1][1] + rows[-1][2] == addr:
            rows[-1][2] += size
        else:
            rows.append([name, addr, size, r, w, ex])
        addr += size
    with open(od / "blocks.tsv", "w") as f:
        f.write("# name\tstart\tsize\tr\tw\tx\n")
        for name, start, size, r, w, ex in rows:
            f.write(f"{name}\t{start:08X}\t{size:X}\t{r}\t{w}\t{ex}\n")
    # PE sections, one file each: <out>/sections/blocks.tsv + <ADDR>.bin (tools/harvest_hints.py input).
    sd = od / "sections"
    sd.mkdir(exist_ok=True)
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    nsec = struct.unpack_from("<H", image, pe + 6)[0]
    sec = pe + 24 + struct.unpack_from("<H", image, pe + 20)[0]
    with open(sd / "blocks.tsv", "w") as f:
        f.write("# name\tstart\tsize\tr\tw\tx\n")
        for i in range(nsec):
            name, vsize, va = struct.unpack_from("<8sII", image, sec + i * 40)
            chars = struct.unpack_from("<I", image, sec + i * 40 + 36)[0]
            name = name.rstrip(b"\0").decode("latin-1")
            start = load_addr + va
            (sd / f"{start:08X}.bin").write_bytes(image[va: va + vsize])
            ex = "true" if chars & 0x20000000 else "false"
            w = "true" if chars & 0x80000000 else "false"
            f.write(f"{name}\t{start:08X}\t{vsize:X}\ttrue\t{w}\t{ex}\n")

    print(f"image {len(image):#x} bytes at {load_addr:#x}, page {page_size:#x}")
    for row in rows:
        print(f"  {row[0]:6} {row[1]:08X} +{row[2]:X} x={row[5]}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
