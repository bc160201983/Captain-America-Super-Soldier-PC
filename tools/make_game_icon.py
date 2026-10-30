"""Build cass.exe's icon from the game's own dashboard tile.

Decrypts default.xex (tools/dump_xex_image.py), finds the title's XDBF/SPA resource and takes its
title image (namespace 2, id 0x8000: the 64x64 PNG the Xbox 360 dashboard shows), then writes a
multi-size .ico. Nothing from the game is committed: this runs at build time from your own copy.
If anything fails, the fallback icon is copied instead so the build never breaks.

Usage: python tools/make_game_icon.py <default.xex> <out.ico> <fallback.ico>
"""
import io
import shutil
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dump_xex_image  # noqa: E402

RESOURCE_INFO = 0x000002FF
XDBF_IMAGE_NS = 2
TITLE_IMAGE_ID = 0x8000
SIZES = [16, 24, 32, 48, 64, 128, 256]


def title_png(xex_path: str) -> bytes:
    x = Path(xex_path).read_bytes()
    image, load_addr = dump_xex_image.decrypt_image(x)
    nhdr = struct.unpack_from(">I", x, 0x14)[0]
    headers = dict(struct.unpack_from(">II", x, 24 + i * 8) for i in range(nhdr))
    off = headers[RESOURCE_INFO]
    size = struct.unpack_from(">I", x, off)[0]
    for i in range((size - 4) // 16):
        _name, addr, length = struct.unpack_from(">8sII", x, off + 4 + i * 16)
        d = image[addr - load_addr: addr - load_addr + length]
        if d[:4] != b"XDBF":
            continue
        _ver, tlen, count, flen, _fcount = struct.unpack_from(">IIIII", d, 4)
        base = 0x18 + tlen * 18 + flen * 8
        for e in range(count):
            ns, eid, eoff, elen = struct.unpack_from(">HQII", d, 0x18 + e * 18)
            if ns == XDBF_IMAGE_NS and eid == TITLE_IMAGE_ID:
                png = d[base + eoff: base + eoff + elen]
                if png[:8] == b"\x89PNG\r\n\x1a\n":
                    return png
    raise ValueError("no title image in the XEX resources")


def main() -> int:
    xex, out, fallback = sys.argv[1:4]
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    try:
        png = title_png(xex)
        from PIL import Image
        src = Image.open(io.BytesIO(png)).convert("RGBA")  # 64x64 dashboard tile
        # Pillow only writes sizes up to the base image, so start from a 256 px LANCZOS upscale;
        # the 64 px entry keeps the original pixels. Explorer picks the closest size per view.
        big = src.resize((256, 256), Image.LANCZOS)
        frames = [src if s == 64 else big.resize((s, s), Image.LANCZOS) for s in SIZES]
        big.save(out, format="ICO", sizes=[(s, s) for s in SIZES], append_images=frames)
        print(f"make_game_icon: game title icon -> {out}")
    except Exception as e:  # missing XEX, no Pillow, odd resource: keep the build going
        shutil.copyfile(fallback, out)
        print(f"make_game_icon: using fallback icon ({e})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
