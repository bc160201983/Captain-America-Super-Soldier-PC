"""Find function entry points that codegen missed and write them as manifest hints.

Method follows the Gears of War 2 port's harvester (santiquiroz/gears-of-war-2-pc-port):
candidate addresses come from .pdata (the exception-unwind function table), base relocations,
pointers stored in data sections (vtables, callback tables) and lis/addi pairs in code.
A candidate is kept when it is in an executable section, is not already a registered function,
is not a label inside a recompiled function, and starts right after a code boundary.

Usage: python tools/harvest_hints.py [--image out/image/sections] [--generated generated/default]
                                     [--out hints/harvested.toml] [--report out/harvest_report.txt]
Needs: python tools/dump_xex_image.py game/default.xex out/image  (writes out/image/sections)
"""
import argparse
import bisect
import re
import struct
from pathlib import Path

BASE = 0x82000000
BLR, BCTR = 0x4E800020, 0x4E800420
SOURCES = ("pdata", "reloc", "datascan", "codeptr", "thunk", "gap")


class Section:
    def __init__(self, name, start, ex, data):
        self.name, self.start, self.ex, self.data = name, start, ex, data
        self.end = start + len(data)

    def word(self, addr):
        off = addr - self.start
        if 0 <= off <= len(self.data) - 4:
            return struct.unpack_from(">I", self.data, off)[0]
        return None


def load_sections(d: Path):
    out = []
    for line in (d / "blocks.tsv").read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        name, start, _size, _r, _w, ex = line.split("\t")
        a = int(start, 16)
        out.append(Section(name, a, ex == "true", (d / f"{a:08X}.bin").read_bytes()))
    return out


def code_section(secs, addr):
    for s in secs:
        if s.ex and s.start <= addr < s.end - 3:
            return s
    return None


def from_pdata(secs):
    p = next(s for s in secs if s.name == ".pdata")
    found = []
    for off in range(0, len(p.data) - 7, 8):
        begin = struct.unpack_from(">I", p.data, off)[0]
        if begin == 0:
            break
        found.append(begin)
    return found


def from_reloc(secs):
    """IMAGE_REL_BASED_HIGHLOW slots that live in data sections: read the pointer stored there."""
    r = next(s for s in secs if s.name == ".reloc")
    found, off = [], 0
    while off + 8 <= len(r.data):
        va, size = struct.unpack_from(">II", r.data, off)
        if size < 8:
            break
        for i in range((size - 8) // 2):
            if off + 10 + 2 * i > len(r.data):  # .reloc is cut short at the end of the loaded image
                break
            e = struct.unpack_from(">H", r.data, off + 8 + 2 * i)[0]
            if e >> 12 != 3:
                continue
            slot = BASE + va + (e & 0xFFF)
            for s in secs:
                if not s.ex and s.start <= slot < s.end - 3:
                    found.append(s.word(slot))
                    break
        off += size
    return found


def from_datascan(secs):
    found = []
    for s in secs:
        if s.ex or s.name in (".pdata", ".reloc"):
            continue
        n = len(s.data) // 4
        for v in struct.unpack(f">{n}I", s.data[: 4 * n]):
            if v % 4 == 0 and code_section(secs, v):
                found.append(v)
    return found


def from_codeptr(secs):
    """lis rX,hi followed (within 8 instructions) by addi/ori rY,rX,lo forming a code address."""
    found = []
    for s in secs:
        if not s.ex:
            continue
        n = len(s.data) // 4
        w = struct.unpack(f">{n}I", s.data[: 4 * n])
        for i, x in enumerate(w):
            if x >> 26 != 15 or (x >> 16) & 31:
                continue
            rd, hi = (x >> 21) & 31, x & 0xFFFF
            for j in range(i + 1, min(i + 9, n)):
                y = w[j]
                if y >> 26 == 15 and not (y >> 16) & 31 and (y >> 21) & 31 == rd:
                    break
                if y >> 26 == 14 and (y >> 16) & 31 == rd:
                    lo = y & 0xFFFF
                    v = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                elif y >> 26 == 24 and (y >> 21) & 31 == rd:
                    v = (hi << 16) | (y & 0xFFFF)
                else:
                    continue
                if v % 4 == 0 and code_section(secs, v):
                    found.append(v)
    return found


def from_thunks(secs):
    """C++ adjustor thunks: addi r3,r3,N ; b target."""
    found = []
    for s in secs:
        if not s.ex:
            continue
        for off in range(0, len(s.data) - 7, 4):
            a, b = struct.unpack_from(">II", s.data, off)
            if a & 0xFFFF0000 == 0x38630000 and b >> 26 == 18 and b & 3 == 0:
                found.append(s.start + off)
    return found


def is_uncond_exit(word):
    """blr, bctr, or an unconditional b/ba without link: control never falls through."""
    return word in (BLR, BCTR) or (word >> 26 == 18 and word & 1 == 0)


def decodes_to_exit(md, s, addr, limit, max_len=4096):
    """True if addr..exit decodes as valid PPC with no zero word, ending in an unconditional exit
    before `limit`. Guards the gap scan against data (tables, floats) placed in .text."""
    a = addr
    while a < limit and a - addr < max_len:
        word = s.word(a)
        if word is None or word == 0:
            return False
        if not list(md.disasm(s.data[a - s.start: a - s.start + 4], a)):
            return False
        if is_uncond_exit(word):
            return True
        a += 4
    return False


def from_gaps(secs, registered, labels):
    """Code no recompiled function covers. For each registered function, its body runs at least to
    its last internal label; from there the first unconditional exit ends it. Whatever valid code
    follows before the next registered start was missed by every pointer-based source (e.g. runs of
    vtable-forwarding thunks reached through computed addresses) and becomes a candidate."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    starts = sorted(registered)
    label_list = sorted(labels)
    found = []
    for i, start in enumerate(starts):
        s = code_section(secs, start)
        if s is None:
            continue
        nxt = starts[i + 1] if i + 1 < len(starts) else s.end
        nxt = min(nxt, s.end)
        # furthest internal label of this function (labels between start and the next start)
        j = bisect.bisect_left(label_list, nxt) - 1
        a = label_list[j] if j >= 0 and label_list[j] > start else start
        while a < nxt:  # walk to this function's first unconditional exit after its last label
            word = s.word(a)
            if word is None:
                break
            a += 4
            if is_uncond_exit(word):
                break
        while a < nxt:  # consecutive orphan blocks up to the next registered function
            word = s.word(a)
            if word is None:
                break
            if word == 0:  # alignment padding
                a += 4
                continue
            if not decodes_to_exit(md, s, a, nxt):
                break  # data or something we can't classify: stop, never guess
            found.append(a)
            while a < nxt:
                word = s.word(a)
                a += 4
                if is_uncond_exit(word):
                    break
    return found


def load_generated(gen: Path, project: str):
    registered = {int(m, 16) for m in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+)",
                                                  (gen / f"{project}_register.cpp").read_text())}
    labels, spans = set(), []
    func_re = re.compile(r"DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]+)\)")
    lab_re = re.compile(r"^loc_([0-9A-Fa-f]{8}):", re.M)
    for f in sorted(gen.glob(f"{project}_recomp.*.cpp")):
        text = f.read_text()
        ms = list(func_re.finditer(text))
        for i, m in enumerate(ms):
            body = text[m.end(): ms[i + 1].start() if i + 1 < len(ms) else len(text)]
            ls = [int(x, 16) for x in lab_re.findall(body)]
            labels.update(ls)
            start = int(m.group(1), 16)
            if ls and max(ls) > start:
                spans.append((start, max(ls)))
    spans.sort()
    return registered, labels, spans


def is_terminator(word):
    return word in (BLR, BCTR, 0) or (word >> 26 == 18 and word & 1 == 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default="out/image/sections")
    ap.add_argument("--generated", default="generated/default")
    ap.add_argument("--project", default="cass")
    ap.add_argument("--out", default="hints/harvested.toml")
    ap.add_argument("--report", default="out/harvest_report.txt")
    ap.add_argument("--deny", default="hints/denylist.txt")
    a = ap.parse_args()

    secs = load_sections(Path(a.image))
    registered, labels, spans = load_generated(Path(a.generated), a.project)
    span_starts = [s for s, _ in spans]
    deny = set()
    dp = Path(a.deny)
    if dp.exists():
        for line in dp.read_text().splitlines():
            code = line.split("#", 1)[0].strip()
            if code:
                deny.add(int(code.split()[0], 16))

    cands = {}
    for src, fn in (("pdata", from_pdata), ("reloc", from_reloc), ("datascan", from_datascan),
                    ("codeptr", from_codeptr), ("thunk", from_thunks),
                    ("gap", lambda ss: from_gaps(ss, registered, labels))):
        for v in fn(secs):
            cands.setdefault(v, set()).add(src)

    accepted, rejects = {}, {k: 0 for k in
                             ("denied", "not_code", "registered", "label", "inside_span", "zero", "mid_function")}
    for addr, srcs in cands.items():
        s = code_section(secs, addr) if addr % 4 == 0 else None
        if addr in deny:
            rejects["denied"] += 1
        elif s is None:
            rejects["not_code"] += 1
        elif addr in registered:
            rejects["registered"] += 1
        elif addr in labels:
            rejects["label"] += 1
        elif "pdata" not in srcs and (i := bisect.bisect_right(span_starts, addr) - 1) >= 0 \
                and addr <= spans[i][1]:
            rejects["inside_span"] += 1
        elif s.word(addr) == 0:
            rejects["zero"] += 1
        elif "pdata" in srcs or addr == s.start or is_terminator(s.word(addr - 4)):
            accepted[addr] = srcs
        else:
            rejects["mid_function"] += 1

    # Keep earlier hints: once codegen registers them they would otherwise be dropped as "registered".
    new_count = len(accepted)
    out = Path(a.out)
    if out.exists():
        for m in re.finditer(r"^0x([0-9A-Fa-f]{8}) = \{\}\s*#\s*(\S*)", out.read_text(), re.M):
            accepted.setdefault(int(m.group(1), 16), set(m.group(2).split(",")) - {""})

    order = lambda ss: ",".join(x for x in SOURCES if x in ss)
    lines = ["# Generated by tools/harvest_hints.py - do not edit by hand.",
             "# Missed function entry points found by scanning the decrypted image.", "[functions]"]
    lines += [f"0x{x:08X} = {{}}  # {order(ss)}" for x, ss in sorted(accepted.items())]
    Path(a.out).write_text("\n".join(lines) + "\n")
    counts = ", ".join(f"{x}={sum(1 for v in cands.values() if x in v)}" for x in SOURCES)
    summary = (f"registered functions: {len(registered)}\ncandidates: {counts} (unique={len(cands)})\n"
               f"new: {new_count}, total in {a.out}: {len(accepted)}\nrejected: " + ", ".join(f"{k}={v}" for k, v in rejects.items()))
    print(summary)
    Path(a.report).parent.mkdir(parents=True, exist_ok=True)
    Path(a.report).write_text(summary + "\n" + "\n".join(
        f"0x{x:08X}: {order(ss)}" for x, ss in sorted(accepted.items())) + "\n")


if __name__ == "__main__":
    main()
