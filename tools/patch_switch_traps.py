#!/usr/bin/env python3
# Adapted from santiquiroz/gears-of-war-2-pc-port tools/patch_switch_traps.py (BSD-3-Clause, Copyright (c) 2026 Santiago Quiroz Upegui and the gow2-recomp contributors).
# The idea (dispatch on the guest CTR instead of trapping in the default of a generated jump-table
# switch, and goto when the target is a label of the same function) comes from that script; the
# parsing, checks, label fallback and host hooks below are written for this project.
"""Post-codegen patcher: no silent ud2 in the default of a generated jump table.

ReXGlue turns each guest jump table (`mtctr rN; bctr`) into `switch (ctx.rI.u32) { case k: goto
loc_X; ... default: __builtin_trap(); // Switch case out of range }`. __builtin_trap is a ud2, so
an index codegen did not list kills the game with 0xC000001D and nothing in the log. Every such
default is rewritten to follow what the console does, a branch to CTR:

    default:
        // cass_switch_trap begin: bctr 0x<site> in <func>, index ctx.rI.u32, <verified|unverified> mtctr
        CassSwitchTrapHit(0x<site>u, "<func>", ctx.rI.u32, ctx.ctr.u32);
        switch (ctx.ctr.u32 & ~3u) {
        case 0x<L>u: goto loc_<L>;      // every top-level label of <func>
        ...
        default: break;
        }
        CASS_SWITCH_TRAP_TAIL(0x<site>u, "<func>", ctx.rI.u32);
        return;
        // cass_switch_trap end

  * CTR is a label of the same function -> goto it (exact guest semantics, the case codegen missed);
  * CTR is a registered function entry -> tail call, as codegen emits every other bctr;
  * anything else -> src/cass_switch_trap.cpp logs CRITICAL (site, function, index, CTR, LR), flushes
    and aborts through REX_FATAL.

Checks (any failure writes nothing and exits 1):
  * the trap is the `default:` of a `switch (ctx.rN.u32) {` that holds only `case k: goto loc_X;`,
    directly after the `// bctr` comment, at brace depth 2 of a DEFINE_REX_FUNC body, and every
    goto of that switch names a label of the same body;
  * the site address (counted from the function start and labels, one `\\t// insn` comment per
    guest instruction) is known; with --image it must hold `bctr` and the mtctr must match;
  * CTR is written through ctx.ctr somewhere before the switch in that function (so ctx.ctr.u32 is
    the guest CTR); "verified" means the instruction right before bctr is `mtctr rN` and its code is
    `ctx.ctr.u64 = ctx.rN.u64;`;
  * after patching: no trap left, labels unchanged, one begin/end block per site.

Idempotent: an existing block is taken back to the trap line and regenerated, so a re-run reports
"already patched" (or "updated" when this script changed its output). "bound-checked" means a
`cmplwi crX,rI,K` + `bgt crX` (no label, no write to rI in between) guards the table and the case
list covers 0..K, i.e. the default is unreachable from that path; it is informational.

Usage:  python -I tools/patch_switch_traps.py [--check] [--verbose] [--image out/image/image.bin]
                                              <generated/default dir | cass_recomp.*.cpp ...>
        --check    verify every site, report, write nothing
        --verbose  one line per site (sites not proven bound-checked are always listed)
        --image    also check every site address against the decrypted image (base 0x82000000)
"""
import glob
import os
import re
import sys

TRAP = '\t\t__builtin_trap(); // Switch case out of range'
BEGIN = '\t\t// cass_switch_trap begin: '
END = '\t\t// cass_switch_trap end'
DECL_BEGIN = '// cass_switch_trap: host hooks (tools/patch_switch_traps.py)'
DECL = [DECL_BEGIN, '#include "cass_switch_trap.h"']

FN_RE = re.compile(r'^DEFINE_REX_FUNC\((\w+)\) \{$')
LBL_RE = re.compile(r'^loc_([0-9A-F]{8}):$')
INS_RE = re.compile(r'^\t// (.*?)\s*$')
SWITCH_RE = re.compile(r'^\tswitch \((ctx\.(r\d+)\.u32)\) \{$')
CASE_RE = re.compile(r'^\tcase (\d+):$')
GOTO_RE = re.compile(r'^\t\tgoto loc_([0-9A-F]{8});$')
MTCTR_RE = re.compile(r'^mtctr (r\d+)$')
STRIP_RE = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'')

IMAGE_BASE = 0x82000000
BCTR = 0x4E800420


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


def brace_delta(line):
    if '{' not in line and '}' not in line:
        return 0
    code = STRIP_RE.sub('""', line)
    cut = code.find('//')
    if cut >= 0:
        code = code[:cut]
    return code.count('{') - code.count('}')


def unpatch(lines, base):
    """Take every existing block back to the trap line; return (lines, old blocks in order)."""
    out, old = [], []
    k = 0
    while k < len(lines):
        l = lines[k]
        if l.startswith(BEGIN):
            end = next((j for j in range(k + 1, len(lines))
                        if lines[j] == END or lines[j].startswith(BEGIN)), None)
            if end is None or lines[end] != END:
                raise PatchError('%s:%d: cass_switch_trap block without its end marker' % (base, k + 1))
            if not out or out[-1] != '\tdefault:' or end + 1 >= len(lines) or lines[end + 1] != '\t}':
                raise PatchError('%s:%d: cass_switch_trap block is not the default of a switch'
                                 % (base, k + 1))
            old.append(lines[k:end + 1])
            out.append(TRAP)
            k = end + 1
            continue
        out.append(l)
        k += 1
    # Declaration block: ['', DECL...] right after the first #include run.
    for k in range(len(out)):
        if out[k] == DECL_BEGIN:
            if out[k:k + len(DECL)] != DECL or k == 0 or out[k - 1] != '':
                raise PatchError('%s:%d: malformed cass_switch_trap declaration block' % (base, k + 1))
            del out[k - 1:k + len(DECL)]
            break
    return out, old


def scan(lines, base):
    """Function bodies with depth per line, top-level labels and guest instruction addresses."""
    funcs = []          # dict(name, start, end, labels:set, insns:[(addr, text, line, after_label)])
    owner = [None] * len(lines)
    depth_at = [0] * len(lines)
    k = 0
    n = len(lines)
    while k < n:
        m = FN_RE.match(lines[k])
        if not m:
            k += 1
            continue
        name = m.group(1)
        fm = re.match(r'sub_([0-9A-F]{8})$', name)
        cur = int(fm.group(1), 16) if fm else None
        f = {'name': name, 'start': k, 'labels': set(), 'insns': []}
        depth = 1
        after_label = True
        j = k + 1
        while j < n:
            l = lines[j]
            depth_at[j] = depth
            owner[j] = len(funcs)
            if l == '}' and depth == 1:
                break
            lm = LBL_RE.match(l)
            if lm:
                cur = int(lm.group(1), 16)
                if depth == 1:          # only top-level labels are safe goto targets
                    f['labels'].add(lm.group(1))
                after_label = True
            else:
                im = INS_RE.match(l)
                if im:
                    f['insns'].append((cur, im.group(1), j, after_label))
                    after_label = False
                    if cur is not None:
                        cur += 4
                depth += brace_delta(l)
                if depth < 1:
                    raise PatchError('%s:%d: unbalanced braces in %s' % (base, j + 1, name))
            j += 1
        if j >= n:
            raise PatchError('%s:%d: body of %s never closes' % (base, k + 1, name))
        f['end'] = j
        funcs.append(f)
        k = j + 1
    return funcs, owner, depth_at


def written_reg(text):
    """Destination register of a simple integer instruction, or None."""
    parts = text.split(None, 1)
    if len(parts) < 2:
        return None
    mn = parts[0].rstrip('.')
    if mn.startswith(('st', 'cmp', 'b', 'mt', 'dcb', 'icb', 'tw', 'td', 'sync', 'eieio', 'nop')):
        return None
    return parts[1].split(',')[0].strip()


def bound_checked(insns, bidx, reg, cases):
    """True if a cmplwi/bgt on `reg` right before the table proves cases cover every index."""
    lo = max(0, bidx - 16)
    window = insns[lo:bidx + 1]             # ends with the bctr itself
    for i in range(len(window) - 2, -1, -1):
        a, t, _, after_label = window[i]
        m = re.match(r'cmplwi (cr\d),(r\d+),(\d+)$', t)
        if m and m.group(2) == reg:
            cr, k = m.group(1), int(m.group(3))
            rest = window[i + 1:]
            if any(x[3] for x in rest):          # a label: other paths reach the table unchecked
                return False
            if any(written_reg(x[1]) == reg for x in rest):
                return False
            # `ble crX,L; b out` is not accepted: L is a label any other path may also reach.
            guarded = any(x[1].startswith('bgt %s,' % cr) or x[1] == 'bgtlr %s' % cr for x in rest)
            return guarded and all(c in cases for c in range(k + 1))
        if after_label:
            return False
    return False


def patch_file(path, image, results):
    base = os.path.basename(path)
    with open(path, encoding='utf-8', newline='') as fh:
        text = fh.read()
    eol = '\r\n' if '\r\n' in text else '\n'
    orig_lines = text.split(eol)
    lines, old_blocks = unpatch(orig_lines, base)
    sites = [k for k, l in enumerate(lines) if l == TRAP]
    if len(sites) != len(old_blocks) + sum(1 for l in orig_lines if l == TRAP):
        raise PatchError('%s: trap/block count mismatch after unpatch' % base)
    stray = [k for k, l in enumerate(lines) if 'Switch case out of range' in l and l != TRAP]
    if stray:
        raise PatchError('%s:%d: unexpected trap form "%s"' % (base, stray[0] + 1, lines[stray[0]].strip()))
    if not sites:
        return None
    funcs, owner, depth_at = scan(lines, base)
    blocks = {}
    for t in sites:
        fi = owner[t]
        if fi is None:
            raise PatchError('%s:%d: trap outside any DEFINE_REX_FUNC body' % (base, t + 1))
        f = funcs[fi]
        if depth_at[t] != 2:
            raise PatchError('%s:%d: trap at brace depth %d, expected 2' % (base, t + 1, depth_at[t]))
        if lines[t - 1] != '\tdefault:' or lines[t + 1] != '\t}':
            raise PatchError('%s:%d: trap is not the last statement of a switch default' % (base, t + 1))
        # Walk back over `case k:` / `goto loc_X;` pairs to the switch header.
        h = t - 2
        cases = {}
        while h > f['start'] and not lines[h].startswith('\tswitch ('):
            g = GOTO_RE.match(lines[h])
            c = CASE_RE.match(lines[h - 1])
            if not g or not c:
                raise PatchError('%s:%d: unexpected line in switch body: "%s"'
                                 % (base, h + 1, lines[h].strip()))
            if g.group(1) not in f['labels']:
                raise PatchError('%s:%d: case goto loc_%s has no label in %s'
                                 % (base, h + 1, g.group(1), f['name']))
            cases[int(c.group(1))] = g.group(1)
            h -= 2
        sm = SWITCH_RE.match(lines[h])
        if not sm or depth_at[h] != 1:
            raise PatchError('%s:%d: switch header not found or not at function top level'
                             % (base, t + 1))
        index_expr, index_reg = sm.group(1), sm.group(2)
        # The guest instruction that produced the switch: `// bctr` right above it.
        bidx = next((i for i, x in enumerate(f['insns']) if x[2] == h - 1), None)
        if bidx is None or f['insns'][bidx][1] != 'bctr':
            raise PatchError('%s:%d: switch is not directly after a "// bctr" comment' % (base, h + 1))
        site = f['insns'][bidx][0]
        if site is None:
            raise PatchError('%s:%d: cannot place the bctr of %s (no address anchor)'
                             % (base, h, f['name']))
        verified = False
        mt_reg = None
        if bidx > 0:
            mm = MTCTR_RE.match(f['insns'][bidx - 1][1])
            if mm:
                mt_reg = mm.group(1)
                code = lines[f['insns'][bidx - 1][2] + 1]
                verified = code == '\tctx.ctr.u64 = ctx.%s.u64;' % mt_reg
        if not verified and not any(lines[i].startswith('\tctx.ctr.u64 = ')
                                    for i in range(f['start'], h)):
            raise PatchError('%s:%d: no write to ctx.ctr before the switch in %s; ctx.ctr.u32 would '
                             'not be the guest CTR' % (base, h + 1, f['name']))
        if image is not None:
            off = site - IMAGE_BASE
            if off < 0 or off + 4 > len(image) or int.from_bytes(image[off:off + 4], 'big') != BCTR:
                raise PatchError('%s:%d: 0x%08X is not a bctr in the image (address count is off)'
                                 % (base, h, site))
            if mt_reg is not None:
                want = 0x7C0903A6 | (int(mt_reg[1:]) << 21)
                if int.from_bytes(image[off - 4:off], 'big') != want:
                    raise PatchError('%s:%d: 0x%08X is not mtctr %s in the image'
                                     % (base, h, site - 4, mt_reg))
        bounded = bound_checked(f['insns'], bidx, index_reg, cases)
        labels = sorted(f['labels'])
        func_lit = '"%s"' % f['name']
        blk = [BEGIN + 'bctr 0x%08X in %s, index %s, %s mtctr (tools/patch_switch_traps.py)'
               % (site, f['name'], index_expr, 'verified' if verified else 'unverified'),
               '\t\tCassSwitchTrapHit(0x%08Xu, %s, %s, ctx.ctr.u32);' % (site, func_lit, index_expr),
               '\t\tswitch (ctx.ctr.u32 & ~3u) {']
        blk += ['\t\tcase 0x%su: goto loc_%s;' % (lab, lab) for lab in labels]
        blk += ['\t\tdefault: break;',
                '\t\t}',
                '\t\tCASS_SWITCH_TRAP_TAIL(0x%08Xu, %s, %s);' % (site, func_lit, index_expr),
                '\t\treturn;',
                END]
        blocks[t] = blk
        results.append({'site': site, 'func': f['name'], 'file': base, 'line': h + 1,
                        'index': index_expr, 'cases': len(cases), 'verified': verified,
                        'bounded': bounded, 'labels': len(labels)})
    new = []
    for k, l in enumerate(lines):
        new.extend(blocks[k] if k in blocks else [l])
    last_inc = -1
    for k, l in enumerate(new):
        if l.startswith('#include'):
            last_inc = k
        elif l.strip() and not l.startswith('#'):
            break
    if last_inc < 0:
        raise PatchError('%s: no #include line to put the declarations after' % base)
    new[last_inc + 1:last_inc + 1] = [''] + DECL
    # Post-conditions.
    if any(l == TRAP for l in new):
        raise PatchError('%s: a trap survived patching' % base)
    if sorted(l for l in new if LBL_RE.match(l)) != sorted(l for l in orig_lines if LBL_RE.match(l)):
        raise PatchError('%s: the set of labels changed' % base)
    if sum(1 for l in new if l.startswith(BEGIN)) != len(sites) or new.count(END) != len(sites):
        raise PatchError('%s: begin/end marker count does not match %d sites' % (base, len(sites)))
    # Per-site status. unpatch() turned each old block into one trap line in place, so the sites
    # and the original traps/blocks are in the same file order.
    old_iter = iter(old_blocks)
    orig_order = [None if l == TRAP else next(old_iter)
                  for l in orig_lines if l == TRAP or l.startswith(BEGIN)]
    first = len(results) - len(sites)
    for n, t in enumerate(sites):
        prev = orig_order[n]
        results[first + n]['status'] = ('patched' if prev is None else
                                        'already patched' if prev == blocks[t] else 'updated')
    new_text = eol.join(new)
    return (path, new_text) if new_text != text else None


def main(argv):
    check_only = '--check' in argv
    verbose = '--verbose' in argv
    args = [a for a in argv if a not in ('--check', '--verbose')]
    image = None
    if '--image' in args:
        i = args.index('--image')
        if i + 1 >= len(args):
            print('patch_switch_traps: --image needs a path', file=sys.stderr)
            return 2
        with open(args[i + 1], 'rb') as fh:
            image = fh.read()
        del args[i:i + 2]
    if not args:
        print(__doc__)
        return 2
    results = []
    pending = []
    try:
        for path in collect_files(args):
            r = patch_file(path, image, results)
            if r:
                pending.append(r)
    except PatchError as e:
        print('patch_switch_traps: ERROR: %s' % e, file=sys.stderr)
        print('patch_switch_traps: nothing was written. The generated jump tables no longer look '
              'the way this script expects; inspect the site above before building.', file=sys.stderr)
        return 1
    for r in sorted(results, key=lambda r: r['site']):
        if verbose or not r['bounded'] or not r['verified']:
            print('patch_switch_traps: 0x%08X %s %s:%d switch(%s) %d cases, %d labels, %s mtctr, %s, %s'
                  % (r['site'], r['func'], r['file'], r['line'], r['index'], r['cases'], r['labels'],
                     'verified' if r['verified'] else 'UNVERIFIED',
                     'bound-checked' if r['bounded'] else 'bounds not proven',
                     'matches (not patched yet)' if check_only and r['status'] == 'patched'
                     else r['status']))
    n = len(results)
    count = lambda key, val: sum(1 for r in results if r[key] == val)
    print('patch_switch_traps: %d sites: %d patched, %d already patched, %d updated; %d verified mtctr, '
          '%d unverified; %d bound-checked, %d bounds not proven; %d label fallback cases; '
          '%d file(s) %s%s'
          % (n, count('status', 'patched'), count('status', 'already patched'),
             count('status', 'updated'), count('verified', True), count('verified', False),
             count('bounded', True), count('bounded', False), sum(r['labels'] for r in results),
             len(pending), 'would be rewritten (--check)' if check_only else 'rewritten',
             ', image-checked' if image is not None else ''))
    if check_only:
        return 0
    for path, new_text in pending:
        tmp = path + '.swtrap.tmp'
        with open(tmp, 'w', encoding='utf-8', newline='') as fh:
            fh.write(new_text)
        os.replace(tmp, path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
