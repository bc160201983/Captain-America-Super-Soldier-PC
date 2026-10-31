#!/usr/bin/env python3
# Adapted from gow3-recomp tools/patch_game_patches.py (BSD-3-Clause, Copyright (c) 2026 Flippage and the gow3-recomp contributors).
# Only the approach (post-codegen rewrite of generated C++ into calls to host hooks) is shared; the
# site table, address mapping and checks below are written for this game.
"""Post-codegen patcher: optional native 1280x720 render size for Captain America: Super Soldier.

The game renders its 3D scene at 1152x648 and the console upscales that to 1280x720. The size is
written by `li` immediates into the renderer config at 0x82908778 (+0x30 width, +0x34 height, the
same words the size-provider getters sub_82325558/sub_823255A8 return through the live copy at
0x82A06970), into a second copy at 0x82A0BE5C, into the "Main Clr Target"/"Main Depth Target"
descriptors, and is passed as 1152.0f/648.0f (float pool 0x820EB0B4/0x820EB0B8) to the
render-space ortho matrix (sub_8230A870 on 0x828E5C50+0x50). The half-size and quarter-size depth
buffers sit at hard-coded EDRAM tile bases that only fit the 1152x648-derived targets, so those two
bases are moved as well. Half/quarter/sixteenth colour targets are derived from the Main Set by
sub_8225AB78 (divw) and need no patch.

Guest memory carve. sub_822B5868 (memory-system init) physically allocates the "Permanent" bump
arena (object 0x82A0E974, never freed; Alloc sub_8230B420 asserts and still returns the
out-of-range pointer when full) with `lis 0x12C` = 0x012C0000 (18.75 MiB), then the "Standard"
heap with `lis 0x1AC4` = 0x1AC40000. The frontbuffer/Color and frontbuffer/Color2 textures
(sub_822B9E28) and two screen-scaled post-FX textures (sub_8282EFC0) are created in that arena at
the render size, so at 1280x720 it overflows on the first level load (0x3A9804-byte pool,
sub_82244390). The two arena-size sites (alloc 0x822B59A4, end = base + r29 at 0x822B5984) become
calls too, and the host grows the arena. The heap keeps the game's size: it already has to hold
the larger 720p render targets, and the extra arena bytes come from guest physical memory outside
the carve (512 MB here, with no console OS reserve and the XEX image outside it).

Every site below becomes a call into src/cass_render_size.cpp, which returns the original value
unless the cvar cass_native_720p is true. Nothing else in the guest code changes.

Usage:  python -I tools/patch_native_720p.py [--check] <generated/default dir | cass_recomp.*.cpp ...>
        --check   only verify that every site resolves and matches (patched or not); write nothing

Idempotent: an already patched site is reported and left alone. If any site does not match exactly,
the script writes nothing and exits with code 1.
"""
import bisect
import glob
import os
import re
import sys

# (guest address, kind, instruction comment ReXGlue writes, comment of the instruction before it)
# kind: 'w'/'h' integer render width/height (li); 'wf'/'hf' float render width/height (lfs);
#       'edram' EDRAM tile base of a derived depth buffer (li);
#       'arena' Permanent arena size in bytes (lis, value = imm << 16).
SITES = [
    # sub_822A98E8: boot-time renderer config init (0x82908778), then device creation.
    (0x822A999C, 'w', 'li r10,1152', 'addi r3,r9,-30856'),
    (0x822A99A8, 'h', 'li r9,648', 'stw r11,-30856(r9)'),
    # sub_82250158: config re-init + "Main Clr Target"/"Main Depth Target" descriptors.
    (0x822501AC, 'w', 'li r10,1152', 'stw r11,-30856(r9)'),
    (0x822501B0, 'h', 'li r9,648', 'li r10,1152'),
    # sub_82251368: config re-init + render-space ortho matrix.
    (0x82251380, 'w', 'li r10,1152', 'addi r6,r8,-30856'),
    (0x82251384, 'h', 'li r9,648', 'li r10,1152'),
    (0x822513C8, 'hf', 'lfs f2,1000(r8)', 'addi r3,r11,80'),
    (0x822513CC, 'wf', 'lfs f1,996(r8)', 'lfs f2,1000(r8)'),
    # function containing 0x8225181C: config re-init on the display-mode path.
    (0x8225181C, 'w', 'li r10,1152', 'addi r6,r8,-30856'),
    (0x82251820, 'h', 'li r9,648', 'li r10,1152'),
    # sub_8218A4B8: lazily initialised copy at 0x82A0BE5C (screen-space point mapping).
    (0x8218A4EC, 'w', 'li r6,1152', 'lis r7,70'),
    (0x8218A4F0, 'h', 'li r8,648', 'li r6,1152'),
    (0x8218A570, 'w', 'li r10,1152', 'stw r9,40(r11)'),
    (0x8218A574, 'h', 'li r9,648', 'li r10,1152'),
    # sub_8225AC88: "Half-size depth buffer" EDRAM base (1264 = 936 + 328 tiles of 576x648).
    (0x8225ACBC, 'edram', 'li r8,1264', 'lwz r11,4(r31)'),
    # sub_8225ADC8: "Quarter-size depth buffer" EDRAM base.
    (0x8225AED8, 'edram', 'li r8,972', 'lwz r10,40(r27)'),
    # sub_822B5868: memory-system init. These are the only two lis 0x12C in the code (0x8289BC2C
    # is .data). Both must agree: r3 sizes the physical block, r29 sets the arena end.
    (0x822B5984, 'arena', 'lis r29,300', 'li r10,752'),       # arena size: stack +0x74 (dead), end = base + r29
    (0x822B59A4, 'arena', 'lis r3,300', 'stw r8,168(r1)'),    # arena physical alloc (sub_824A61C8)
]

HOOK = {'w': 'CassRenderWidth', 'h': 'CassRenderHeight', 'edram': 'CassRenderEdramBase',
        'wf': 'CassRenderWidthF', 'hf': 'CassRenderHeightF', 'arena': 'CassRenderArenaSize'}

DECL_MARK = '// cass_native_720p: host hooks (tools/patch_native_720p.py)'
DECL_LINES = [
    '',
    DECL_MARK,
    'extern "C" int CassRenderWidth(int orig);',
    'extern "C" int CassRenderHeight(int orig);',
    'extern "C" int CassRenderEdramBase(int orig);',
    'extern "C" float CassRenderWidthF(float orig);',
    'extern "C" float CassRenderHeightF(float orig);',
    'extern "C" uint32_t CassRenderArenaSize(uint32_t orig);',
]

FN_RE = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
LBL_RE = re.compile(r'^loc_([0-9A-F]{8}):')
INS_RE = re.compile(r'^\t// (.*\S)\s*$')


class PatchError(Exception):
    pass


def collect_files(args):
    files = []
    for a in args:
        if os.path.isdir(a):
            files += glob.glob(os.path.join(a, 'cass_recomp.*.cpp'))
        else:
            files += glob.glob(a) or [a]
    files = sorted({os.path.normpath(f) for f in files
                    if os.path.basename(f).startswith('cass_recomp.') and f.endswith('.cpp')})
    if not files:
        raise PatchError('no cass_recomp.*.cpp files given')
    return files


def index_functions(files):
    funcs = []
    for path in files:
        with open(path, encoding='utf-8', newline='') as f:
            for line in f:
                m = FN_RE.match(line)
                if m:
                    funcs.append((int(m.group(1), 16), path))
    funcs.sort()
    return funcs


def locate(lines, func_start, addr):
    """Return (line index of the '\\t// insn' comment for addr, comment of the previous insn).

    ReXGlue writes one '\\t// mnemonic operands' comment per guest instruction, in address order,
    and a 'loc_XXXXXXXX:' label at branch targets. Count from the nearest anchor at or below addr.
    """
    hdr = 'DEFINE_REX_FUNC(sub_%08X)' % func_start
    start = next((k for k, l in enumerate(lines) if l.startswith(hdr)), None)
    if start is None:
        raise PatchError('function sub_%08X not found' % func_start)
    cur = func_start
    prev = None
    for k in range(start + 1, len(lines)):
        l = lines[k]
        if FN_RE.match(l):
            break
        m = LBL_RE.match(l)
        if m:
            lab = int(m.group(1), 16)
            if lab <= addr and lab != cur:
                cur = lab
            continue
        m = INS_RE.match(l)
        if not m:
            continue
        if cur == addr:
            return k, prev
        prev = m.group(1)
        cur += 4
        if cur > addr:
            break
    raise PatchError('0x%08X: no instruction comment at this address in sub_%08X' % (addr, func_start))


def patch_site(lines, idx, addr, kind, comment, prev, prev_seen):
    got = INS_RE.match(lines[idx]).group(1)
    if got != comment:
        raise PatchError('0x%08X: expected "// %s", found "// %s"' % (addr, comment, got))
    if prev_seen != prev:
        raise PatchError('0x%08X: previous instruction expected "// %s", found "// %s"'
                         % (addr, prev, prev_seen))
    hook = HOOK[kind]
    if kind in ('w', 'h', 'edram'):
        reg, imm = re.match(r'li (r\d+),(-?\d+)$', comment).groups()
        k = idx + 1
        orig = '\tctx.%s.s64 = %s;' % (reg, imm)
        new = '\tctx.%s.s64 = %s(%s);' % (reg, hook, imm)
    elif kind == 'arena':
        reg, imm = re.match(r'lis (r\d+),(-?\d+)$', comment).groups()
        value = int(imm) << 16
        # The hooks take and return uint32_t; a positive value keeps s64 bit-identical.
        if not 0 < value < 0x80000000:
            raise PatchError('0x%08X: lis value %d does not fit the uint32_t size hook' % (addr, value))
        k = idx + 1
        orig = '\tctx.%s.s64 = %d;' % (reg, value)
        new = '\tctx.%s.s64 = %s(%d);' % (reg, hook, value)
    else:
        freg = re.match(r'lfs (f\d+),(-?\d+)\((r\d+)\)$', comment).group(1)
        k = None
        for j in range(idx + 1, min(idx + 6, len(lines))):
            if INS_RE.match(lines[j]):
                break
            if lines[j].lstrip().startswith('ctx.%s.f64 =' % freg):
                k = j
                break
        if k is None:
            raise PatchError('0x%08X: no "ctx.%s.f64 = ..." after "// %s"' % (addr, freg, comment))
        orig = '\tctx.%s.f64 = double(temp.f32);' % freg
        new = '\tctx.%s.f64 = double(%s(temp.f32));' % (freg, hook)
    line = lines[k].rstrip('\r\n')
    eol = lines[k][len(line):] or '\n'
    if line == new:
        return 'already patched'
    if line != orig:
        raise PatchError('0x%08X: expected "%s", found "%s"' % (addr, orig.strip(), line.strip()))
    lines[k] = new + eol
    return 'patched'


def add_decls(lines):
    eol = '\r\n' if lines and lines[0].endswith('\r\n') else '\n'
    mark = next((k for k, l in enumerate(lines) if l.startswith(DECL_MARK)), None)
    if mark is not None:
        # Block written by an older version of this script: append the declarations it lacks
        # right after the block, so a re-run on already patched code stays a no-op afterwards.
        end = mark + 1
        while end < len(lines) and lines[end].startswith('extern "C" '):
            end += 1
        have = {l.rstrip('\r\n') for l in lines[mark + 1:end]}
        missing = [d for d in DECL_LINES[2:] if d not in have]
        lines[end:end] = [d + eol for d in missing]
        return bool(missing)
    last_inc = -1
    for k, l in enumerate(lines):
        if l.startswith('#include'):
            last_inc = k
        elif l.strip() and not l.startswith('#'):
            break
    lines[last_inc + 1:last_inc + 1] = [d + eol for d in DECL_LINES]
    return True


def main(argv):
    check_only = '--check' in argv
    args = [a for a in argv if a != '--check']
    if not args:
        print(__doc__)
        return 2
    try:
        files = collect_files(args)
        funcs = index_functions(files)
        starts = [s for s, _ in funcs]
        by_file = {}
        for addr, kind, comment, prev in SITES:
            i = bisect.bisect_right(starts, addr) - 1
            if i < 0:
                raise PatchError('0x%08X: no function covers this address' % addr)
            fstart, path = funcs[i]
            by_file.setdefault(path, []).append((addr, kind, comment, prev, fstart))
        results = []
        rewritten = {}
        for path, sites in by_file.items():
            with open(path, encoding='utf-8', newline='') as f:
                lines = f.readlines()
            changed = False
            for addr, kind, comment, prev, fstart in sites:
                idx, prev_seen = locate(lines, fstart, addr)
                status = patch_site(lines, idx, addr, kind, comment, prev, prev_seen)
                results.append((addr, kind, comment, 'sub_%08X' % fstart,
                                os.path.basename(path), idx + 1, status))
                changed |= status == 'patched'
            if add_decls(lines):
                changed = True
            if changed:
                rewritten[path] = lines
    except PatchError as e:
        print('patch_native_720p: ERROR: %s' % e, file=sys.stderr)
        print('patch_native_720p: nothing was written. The generated code no longer matches the '
              'known sites; re-verify them against out/image/image.bin.', file=sys.stderr)
        return 1
    if len(results) != len(SITES):
        print('patch_native_720p: ERROR: resolved %d of %d sites' % (len(results), len(SITES)),
              file=sys.stderr)
        return 1
    for addr, kind, comment, fn, base, line, status in sorted(results):
        if check_only and status == 'patched':
            status = 'matches (not patched yet)'
        print('patch_native_720p: 0x%08X %-5s %-16s %s %s:%d %s'
              % (addr, kind, comment, fn, base, line, status))
    if check_only:
        return 0
    for path, lines in rewritten.items():
        tmp = path + '.n720.tmp'
        with open(tmp, 'w', encoding='utf-8', newline='') as f:
            f.writelines(lines)
        os.replace(tmp, path)
    print('patch_native_720p: %d sites ok, %d file(s) rewritten' % (len(results), len(rewritten)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
