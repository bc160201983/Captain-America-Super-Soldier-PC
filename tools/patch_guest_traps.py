#!/usr/bin/env python3
"""Post-codegen patcher: no more silent aborts from REX_UNIMPLEMENTED in the generated code.

ReXGlue's codegen emits an instruction it has no builder for as

        // trap
        // UNIMPLEMENTED: trap
        REX_UNIMPLEMENTED(0x822D1368, "trap");

and cass_pch.h defines REX_UNIMPLEMENTED(addr, opcode) as `throw std::runtime_error(...)`. Nothing
catches it, so reaching one ends in std::terminate -> abort -> exception 0xc0000409 (fast fail 7)
in ucrtbase.dll, with no line in the game log.

Every REX_UNIMPLEMENTED(<addr>, "<op>") in the generated code becomes a call into
src/cass_guest_trap.cpp:

  "trap"        CassGuestTrap(<addr>, uint32_t(ctx.lr))
                tw 31,r0,r0, an unconditional debug break. Logs a WARN once per site (address +
                guest lr) and returns, so execution continues at the next instruction (console
                without a debugger, Xenia). Captain America has six, all checked against
                image.bin: 0x822CFCC4 0x822D0034 0x822D06C0 0x822D1180 0x822D1368 (behind the
                debug-breakpoint registry checks sub_822CC7E0/sub_822CC720) and 0x822D3F20, the
                body of sub_822D3F20 ("trap; blr"), the game's DebugBreak()/assert stub, which 56
                bl and 2 tail-call sites call when an assertion fails; lr names the caller.
  anything else CassGuestUnimplemented(<addr>, "<op>")
                Logs CRITICAL "[FATAL] unimplemented PPC instruction <op> at <addr>", flushes the log
                and aborts (REX_FATAL). None exist today; each one is printed as a build WARNING.

The "// UNIMPLEMENTED: <op>" comment above a site is rewritten to say what the call does, and the
two host functions are declared after the #include of each file that calls them (the same way
tools/patch_native_720p.py declares its hooks).

Usage:  python -I tools/patch_guest_traps.py [--check] <generated/default dir | cass_*.cpp ...>
        --check   only report what would be patched; write nothing

Idempotent: already patched sites are reported and left alone, and a file is rewritten only when
something in it changes. Fails (exit 1, nothing written) if any REX_UNIMPLEMENTED use cannot be
classified, i.e. it is not a plain REX_UNIMPLEMENTED(0x<hex>, "<op>") call, or it sits in a file
this script does not patch (cass_init.cpp, a header, ...).
"""
import glob
import os
import re
import sys

TOOL = 'patch_guest_traps'

# Opcodes that are a debug break and may be skipped. Only "trap" (tw 31,r0,r0) has been reviewed;
# any other opcode takes the fatal path until someone checks what it does in this game.
CONTINUE_OPCODES = {'trap'}

# Sites reviewed against out/image/image.bin (all 0x7FE00008). A trap outside this list is still
# patched (log and continue) but is reported as new so it gets looked at.
KNOWN_TRAPS = {0x822CFCC4, 0x822D0034, 0x822D06C0, 0x822D1180, 0x822D1368, 0x822D3F20}

TRAP_HOOK = 'CassGuestTrap'
FATAL_HOOK = 'CassGuestUnimplemented'

DECL_MARK = '// cass_guest_trap: host hooks (tools/patch_guest_traps.py, src/cass_guest_trap.h)'
DECL_LINES = [
    '',
    DECL_MARK,
    'extern "C" void CassGuestTrap(uint32_t guest_addr, uint32_t guest_lr);',
    'extern "C" [[noreturn]] void CassGuestUnimplemented(uint32_t guest_addr, const char* opcode);',
]
# The host header that defines the same two functions. extern "C" names link even when the
# signatures differ, so the declarations above must appear in it verbatim (checked on every run).
HOOK_HEADER = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           'src', 'cass_guest_trap.h')

TOKEN = 'REX_UNIMPLEMENTED'
SITE_RE = re.compile(r'\bREX_UNIMPLEMENTED\(\s*0x([0-9A-Fa-f]{1,8})\s*,\s*"([^"\\\r\n]+)"\s*\)')
DONE_TRAP_RE = re.compile(r'\b' + TRAP_HOOK + r'\(0x([0-9A-F]{8}), uint32_t\(ctx\.lr\)\)')
DONE_FATAL_RE = re.compile(r'\b' + FATAL_HOOK + r'\(0x([0-9A-F]{8}), "([^"\\\r\n]+)"\)')
UNIMPL_COMMENT_RE = re.compile(r'^(\s*)// UNIMPLEMENTED: (\S+)\s*$')
FN_RE = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')

# The macro itself is defined here; it is not a use.
SKIP_FILES = {'cass_pch.h'}


class PatchError(Exception):
    pass


def check_header():
    """Fail unless src/cass_guest_trap.h declares the hooks exactly as DECL_LINES does."""
    try:
        with open(HOOK_HEADER, encoding='utf-8') as f:
            have = {l.strip() for l in f}
    except OSError as e:
        raise PatchError('cannot read %s to check the hook declarations: %s' % (HOOK_HEADER, e))
    for d in DECL_LINES:
        if d.startswith('extern') and d not in have:
            raise PatchError('%s does not declare\n    %s\n  verbatim; keep DECL_LINES in '
                             'tools/patch_guest_traps.py and the header in sync' % (HOOK_HEADER, d))


def collect(args):
    """Return (files to patch, files to scan only)."""
    patch, scan = set(), set()
    for a in args:
        if os.path.isdir(a):
            for f in glob.glob(os.path.join(a, '*.cpp')) + glob.glob(os.path.join(a, '*.h')):
                f = os.path.normpath(f)
                b = os.path.basename(f)
                if b in SKIP_FILES:
                    continue
                (patch if b.startswith('cass_recomp.') and b.endswith('.cpp') else scan).add(f)
        else:
            for f in glob.glob(a) or [a]:
                if not os.path.isfile(f):
                    raise PatchError('%s: no such file' % f)
                patch.add(os.path.normpath(f))
    if not patch:
        raise PatchError('no cass_recomp.*.cpp files given')
    return sorted(patch), sorted(scan - patch)


def code_part(line):
    """The line without a trailing // comment, or '' for a comment line (good enough for the
    generated code, which never puts // inside a string on a line that uses the macro)."""
    s = line.lstrip()
    if s.startswith('//'):
        return ''
    k = line.find('//')
    return line if k < 0 else line[:k]


def has_use(line):
    """True if the macro name is on a line that is not a whole-line // comment. Deliberately
    stricter than code_part(): a REX_UNIMPLEMENTED after a '//' inside a string, or in a trailing
    or /* */ comment, cannot be told apart from code, so it must fail loudly, not be skipped."""
    return TOKEN in line and not line.lstrip().startswith('//')


def replacement(addr, op):
    if op in CONTINUE_OPCODES:
        return '%s(0x%08X, uint32_t(ctx.lr))' % (TRAP_HOOK, addr)
    return '%s(0x%08X, "%s")' % (FATAL_HOOK, addr, op)


def comment_for(op):
    if op in CONTINUE_OPCODES:
        return ('// %s: guest debug break -> %s, logged once per site and skipped '
                '(tools/patch_guest_traps.py)' % (op, TRAP_HOOK))
    return ('// %s: no codegen builder -> %s, logs [FATAL] and aborts (tools/patch_guest_traps.py)'
            % (op, FATAL_HOOK))


def add_decls(lines):
    if any(l.startswith(DECL_MARK) for l in lines):
        return False
    eol = '\r\n' if lines and lines[0].endswith('\r\n') else '\n'
    last_inc = -1
    for k, l in enumerate(lines):
        if l.startswith('#include'):
            last_inc = k
        elif l.strip() and not l.startswith('#'):
            break
    if last_inc < 0:
        raise PatchError('no #include at the top of the file to put the declarations after')
    lines[last_inc + 1:last_inc + 1] = [d + eol for d in DECL_LINES]
    return True


def patch_file(path, results, unclassified):
    with open(path, encoding='utf-8', newline='') as f:
        lines = f.readlines()
    func = None
    changed = False
    uses_hooks = False
    for k, raw in enumerate(lines):
        m = FN_RE.match(raw)
        if m:
            func = 'sub_' + m.group(1)  # and scan the line itself too, so nothing is skipped
        line = raw.rstrip('\r\n')
        eol = raw[len(line):]
        code = code_part(line)
        for m in DONE_TRAP_RE.finditer(code):
            results.append((int(m.group(1), 16), 'trap', func, path, k + 1, 'already patched'))
            uses_hooks = True
        for m in DONE_FATAL_RE.finditer(code):
            results.append((int(m.group(1), 16), m.group(2), func, path, k + 1, 'already patched'))
            uses_hooks = True
        if not has_use(line):
            continue
        sites = []

        def sub(m):
            addr, op = int(m.group(1), 16), m.group(2)
            sites.append((addr, op))
            return replacement(addr, op)

        new_line = SITE_RE.sub(sub, code) + line[len(code):]
        if has_use(new_line):
            unclassified.append('%s:%d: %s' % (os.path.basename(path), k + 1, line.strip()))
            continue
        lines[k] = new_line + eol
        changed = uses_hooks = True
        for addr, op in sites:
            results.append((addr, op, func, path, k + 1, 'patched'))
        # Rewrite the codegen's "// UNIMPLEMENTED: <op>" note right above a lone site.
        if len(sites) == 1 and k > 0:
            c = UNIMPL_COMMENT_RE.match(lines[k - 1].rstrip('\r\n'))
            if c and c.group(2) == sites[0][1]:
                prev_eol = lines[k - 1][len(lines[k - 1].rstrip('\r\n')):]
                lines[k - 1] = c.group(1) + comment_for(sites[0][1]) + prev_eol
    if uses_hooks and add_decls(lines):
        changed = True
    return lines if changed else None


def scan_only(path, unclassified):
    with open(path, encoding='utf-8', errors='replace', newline='') as f:
        for k, raw in enumerate(f):
            if has_use(raw.rstrip('\r\n')):
                unclassified.append('%s:%d: %s (file not patched by this script)'
                                    % (os.path.basename(path), k + 1, raw.strip()))


def main(argv):
    check_only = '--check' in argv
    args = [a for a in argv if a != '--check']
    if not args:
        print(__doc__)
        return 2
    results, unclassified, rewritten = [], [], {}
    try:
        check_header()
        patch, scan = collect(args)
        for path in patch:
            new = patch_file(path, results, unclassified)
            if new is not None:
                rewritten[path] = new
        for path in scan:
            scan_only(path, unclassified)
        # Verify the rewritten text: no use of the macro may survive.
        for path, lines in rewritten.items():
            for k, raw in enumerate(lines):
                if has_use(raw.rstrip('\r\n')):
                    unclassified.append('%s:%d: still present after patching: %s'
                                        % (os.path.basename(path), k + 1, raw.strip()))
    except (PatchError, OSError, UnicodeDecodeError) as e:
        print('%s: ERROR: %s' % (TOOL, e), file=sys.stderr)
        print('%s: nothing was written.' % TOOL, file=sys.stderr)
        return 1
    if unclassified:
        print('%s: ERROR: %d REX_UNIMPLEMENTED use(s) could not be classified; each one would '
              'throw an uncaught exception (silent abort) if reached:' % (TOOL, len(unclassified)),
              file=sys.stderr)
        for u in unclassified:
            print('%s:   %s' % (TOOL, u), file=sys.stderr)
        print('%s: nothing was written. Teach tools/patch_guest_traps.py the new form.' % TOOL,
              file=sys.stderr)
        return 1

    traps = fatal = 0
    for addr, op, func, path, line, status in sorted(results, key=lambda r: (r[0], r[3], r[4])):
        if check_only and status == 'patched':
            status = 'would patch'
        if op in CONTINUE_OPCODES:
            traps += 1
            kind = 'trap -> %s (log once, continue)' % TRAP_HOOK
            note = '' if addr in KNOWN_TRAPS else '  NEW: not in the reviewed list, check it'
        else:
            fatal += 1
            kind = '%s -> %s (log [FATAL], abort)' % (op, FATAL_HOOK)
            note = ''
        print('%s: 0x%08X %-8s %s %s:%d %s%s'
              % (TOOL, addr, op, func or '?', os.path.basename(path), line, status, note))
        if op not in CONTINUE_OPCODES:
            print('%s: WARNING: 0x%08X "%s" has no codegen builder; the game aborts (with a log '
                  'line) if it ever gets there' % (TOOL, addr, op))
    missing = sorted(KNOWN_TRAPS - {r[0] for r in results if r[1] in CONTINUE_OPCODES})
    if missing:
        print('%s: note: reviewed trap site(s) %s not found (the codegen may now handle "trap" '
              'itself)' % (TOOL, ' '.join('0x%08X' % a for a in missing)))
    if check_only:
        print('%s: --check: %d trap site(s), %d fatal site(s), %d file(s) would be rewritten'
              % (TOOL, traps, fatal, len(rewritten)))
        return 0
    for path, lines in rewritten.items():
        tmp = path + '.gtrap.tmp'
        with open(tmp, 'w', encoding='utf-8', newline='') as f:
            f.writelines(lines)
        os.replace(tmp, path)
    print('%s: %d trap site(s), %d fatal site(s), 0 unclassified, %d file(s) rewritten'
          % (TOOL, traps, fatal, len(rewritten)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
