"""Check every native hook target against a game exe before a release (the relocation framework's offline half).

  python -I tools/check_hooks.py <unpacked exe> [native source dir]

The targets are read from the native sources themselves - every scan::FunctionUsing("anchor", "prologue") and
appearance's Hook(L"what", "anchor", "prologue", ...) - so there is one list, the code's. Each is resolved the way
scan.h does it at runtime: the exact ASCII string (NUL-terminated, NUL before it) in the non-executable sections ->
every RIP-relative disp32 in the executable sections that points at it -> the function containing that reference
(.pdata, chained unwind entries followed) -> the functions whose first bytes match the prologue. Exactly one = OK;
none = NOT FOUND; several = AMBIGUOUS (the runtime refuses those too). Exit status 1 if any target fails.

The second, independent anchor (appearance.cpp's Hook): in a file with a literal FunctionUsing target (the full
build), every Hook target must be reachable from it within two calls (E8 / E9 rel32 in each function's body: its unwind
entry + the contiguous entries chained to it) - scan::Reaches, which the runtime checks before hooking. Not reached =
ANCHORS DISAGREE.

Addresses print as file virtual addresses (image base 0x140000000), as tools/exe_dis.py / xref.py use."""
import bisect
import glob
import os
import re
import struct
import sys

import numpy as np
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
exe = sys.argv[1]
src_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', 'native', 'WaCaNative', 'src')

# --- the targets, from the sources --------------------------------------------------------------------------------
CALLS = [
    re.compile(r'FunctionUsing\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"([0-9A-Fa-f? ]+)"\s*\)'),
    re.compile(r'Hook\(\s*L"([^"]*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*,\s*"([0-9A-Fa-f? ]+)"'),
    # scan::FunctionUsingWhere("anchor", "body pattern", n): by what the body does, no prologue (2026-11-04)
    re.compile(r'FunctionUsingWhere\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"([0-9A-Fa-f? ]+)"\s*,\s*(\d+)\s*\)'),
]
targets = []   # (where, label, anchor, prologue, kind: 'hook' | 'root')
for path in sorted(glob.glob(os.path.join(src_dir, '*.cpp'))):
    text = open(path, encoding='utf-8').read()
    for m in CALLS[1].finditer(text):
        line = text.count('\n', 0, m.start()) + 1
        targets.append((f'{os.path.basename(path)}:{line}', m.group(1), m.group(2), m.group(3), 'hook'))
    # literal calls only: the generic call inside Hook() passes variables, which the regex doesn't match
    for m in CALLS[0].finditer(text):
        line = text.count('\n', 0, m.start()) + 1
        targets.append((f'{os.path.basename(path)}:{line}', m.group(1), m.group(1), m.group(2), 'root'))
    # (the prologue slot holds "body:<pattern>:<n>" for these)
    for m in CALLS[2].finditer(text):
        line = text.count('\n', 0, m.start()) + 1
        targets.append((f'{os.path.basename(path)}:{line}', m.group(1), m.group(1), f'body:{m.group(2)}:{m.group(3)}', 'root'))
if not targets:
    sys.exit(f'no hook targets found in {src_dir}')

# --- the exe -------------------------------------------------------------------------------------------------------
pe = pefile.PE(exe, fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
IB = pe.OPTIONAL_HEADER.ImageBase
image = pe.get_memory_mapped_image()
mem = np.frombuffer(image, dtype=np.uint8)
EXEC = 0x20000000
sections = [(s.VirtualAddress, s.Misc_VirtualSize, bool(s.Characteristics & EXEC)) for s in pe.sections]

entries = sorted((e.struct.BeginAddress, e.struct.EndAddress, e.struct.UnwindData)
                 for e in getattr(pe, 'DIRECTORY_ENTRY_EXCEPTION', []))
starts = [e[0] for e in entries]


def function_start(rva):
    """scan::FunctionStart: the containing entry, chained unwind info followed to the primary entry."""
    i = bisect.bisect_right(starts, rva) - 1
    if i < 0 or not (entries[i][0] <= rva < entries[i][1]):
        return None
    begin, _, info = entries[i]
    for _ in range(16):
        flags = image[info] >> 3
        if not flags & 0x4:
            return begin
        codes = image[info + 2]
        at = info + 4 + ((codes + 1) & ~1) * 2
        begin, _, info = struct.unpack_from('<III', image, at)
    return None


def find_ascii(s):
    needle = s.encode('ascii') + b'\0'
    out = []
    for va, size, ex in sections:
        if ex:
            continue
        blob = image[va:va + size]
        at = blob.find(needle)
        while at != -1:
            if at == 0 or blob[at - 1] == 0:
                out.append(va + at)
            at = blob.find(needle, at + 1)
    return out


CHUNK = 1 << 22


def referencing_functions(target_rva):
    """scan::FunctionsReferencing: every 4-byte window whose rel32 (from its end) lands on the target."""
    found = []
    for va, size, ex in sections:
        if not ex:
            continue
        for c0 in range(0, size, CHUNK):
            n = min(CHUNK, size - c0)
            lo = va + c0
            seg = mem[lo:lo + n + 3].astype(np.int64)
            if len(seg) < 4:
                continue
            disp = seg[:-3] | (seg[1:-2] << 8) | (seg[2:-1] << 16) | (seg[3:] << 24)
            disp = np.where(disp >= 1 << 31, disp - (1 << 32), disp)
            pos = np.arange(lo, lo + len(disp), dtype=np.int64)
            for p in np.nonzero(pos + 4 + disp == target_rva)[0]:
                fn = function_start(int(pos[p]))
                if fn is not None and fn not in found:
                    found.append(fn)
    return found


def prologue_matches(fn, prologue):
    for k, tok in enumerate(prologue.split()):
        if tok.startswith('?'):
            continue
        if image[fn + k] != int(tok, 16):
            return False
    return True


def primary_range(fn):
    """A function's body: its entry plus the contiguous entries chained back to it (a big function's later parts)."""
    i = bisect.bisect_right(starts, fn) - 1
    if i < 0 or entries[i][0] != fn:
        return None
    end = entries[i][1]
    while i + 1 < len(entries) and entries[i + 1][0] == end and function_start(entries[i + 1][0]) == fn:
        i += 1
        end = entries[i][1]
    return fn, end


def reaches(src, dst, depth):
    """scan::Reaches: src calls / jumps to dst within depth levels."""
    r = primary_range(src)
    if not r:
        return False
    callees = []
    for p in range(r[0], r[1] - 4):
        if image[p] not in (0xE8, 0xE9):
            continue
        t = p + 5 + struct.unpack_from('<i', image, p + 1)[0]
        if t == dst:
            return True
        if depth > 1 and primary_range(t) and t not in callees:
            callees.append(t)
    return any(reaches(c, dst, depth - 1) for c in callees)


def body_count(fn, pattern):
    r = primary_range(fn)
    if not r:
        return 0
    toks = pattern.split()
    n = 0
    for p in range(r[0], r[1] - len(toks) + 1):
        if all(t.startswith('?') or image[p + k] == int(t, 16) for k, t in enumerate(toks)):
            n += 1
    return n


failed = 0
resolved = {}   # (file, kind) -> [(label, fn)]
for where, label, anchor, prologue, kind in targets:
    strings = find_ascii(anchor.encode().decode('unicode_escape'))
    candidates, refs = [], 0
    for s in strings:
        fns = referencing_functions(s)
        refs += len(fns)
        if prologue.startswith('body:'):
            _, pat, n = prologue.split(':')
            candidates += [f for f in fns if body_count(f, pat) >= int(n) and f not in candidates]
        else:
            candidates += [f for f in fns if prologue_matches(f, prologue) and f not in candidates]
    if len(candidates) == 1:
        verdict = f'OK        0x{IB + candidates[0]:X}'
        resolved.setdefault((where.split(':')[0], kind), []).append((label, candidates[0]))
    elif not candidates:
        verdict = 'NOT FOUND'
        failed += 1
    else:
        verdict = 'AMBIGUOUS ' + ' '.join(f'0x{IB + c:X}' for c in candidates)
        failed += 1
    print(f'{verdict:28} {label:34} anchor "{anchor}" ({len(strings)} string(s), {refs} referencing function(s)), '
          f'prologue {prologue}   [{where}]')

print()
chain_failed = 0
for (file, kind), hooks in sorted(resolved.items()):
    roots = resolved.get((file, 'root'), [])
    if kind != 'hook' or len(roots) != 1:
        continue
    root_label, root = roots[0]
    for label, fn in hooks:
        ok = reaches(root, fn, 2)
        chain_failed += not ok
        print(f'{"CONFIRMED" if ok else "ANCHORS DISAGREE":28} {label:34} reached from {root_label} (0x{IB + root:X}) '
              f'within 2 calls   [{file}]')

# Body anchors on a string-found function held in a variable (camera.cpp): `auto* v = scan::FunctionUsing("a", ...)`
# then scan::BodyContains(v, "pattern", n) / scan::CalleeWhere(v, "pattern") - same checks as scan.h
VAR = re.compile(r'auto\*\s+(\w+)\s*=\s*scan::FunctionUsing(?:Where)?\(\s*"((?:[^"\\]|\\.)*)"')
BODY = re.compile(r'scan::BodyContains\(\s*(\w+)\s*,\s*"([0-9A-Fa-f? ]+)"\s*,\s*(\d+)\s*\)')
CALLEE = re.compile(r'scan::CalleeWhere\(\s*(\w+)\s*,\s*"([0-9A-Fa-f? ]+)"\s*\)')
for path in sorted(glob.glob(os.path.join(src_dir, '*.cpp'))):
    file = os.path.basename(path)
    text = open(path, encoding='utf-8').read()
    found = dict(resolved.get((file, 'root'), []))
    roots = {m.group(1): found.get(m.group(2)) for m in VAR.finditer(text)}
    for m in BODY.finditer(text):
        fn = roots.get(m.group(1))
        if fn is None:
            continue
        n = body_count(fn, m.group(2))
        ok = n >= int(m.group(3))
        chain_failed += not ok
        print(f'{"CONFIRMED" if ok else "ANCHORS DISAGREE":28} {m.group(1):34} body holds {m.group(2)} x{n} '
              f'(needs {m.group(3)})   [{file}]')
    for m in CALLEE.finditer(text):
        fn = roots.get(m.group(1))
        if fn is None:
            continue
        r = primary_range(fn)
        hits = set()
        for p in range(r[0], r[1] - 4):
            if image[p] == 0xE8:
                t = p + 5 + struct.unpack_from('<i', image, p + 1)[0]
                if primary_range(t) and body_count(t, m.group(2)) >= 1:
                    hits.add(t)
        ok = len(hits) == 1
        chain_failed += not ok
        print(f'{"CONFIRMED" if ok else "ANCHORS DISAGREE":28} {m.group(1):34} calls '
              f'{" ".join(hex(IB + h) for h in sorted(hits)) or "nothing"} holding {m.group(2)}   [{file}]')

# UFunction anchors (saves.cpp): STR("/Script/<Module>.<Class>:<Func>") + a call-layout pattern (scan::Parse("..."))
# - offline, the exec thunk comes from the exe's native function tables ({const char* name, thunk} pairs, which UE
# registers per class): of every entry named <Func>, exactly one thunk may hold the pattern, and the call after it
# must land on a function start (what saves.cpp checks at runtime through the UFunction).
UFUNC = re.compile(r'STR\("/Script/[\w.]+:(\w+)"\)')
PARSE = re.compile(r'scan::Parse\("([0-9A-Fa-f? ]+)"\)')
for path in sorted(glob.glob(os.path.join(src_dir, '*.cpp'))):
    file = os.path.basename(path)
    text = open(path, encoding='utf-8').read()
    funcs, pats = UFUNC.findall(text), PARSE.findall(text)
    if not funcs or not pats:
        continue
    for func in funcs:
        name_vas = {IB + a for a in find_ascii(func)}
        thunks = set()
        for va, size, ex in sections:
            if ex:
                continue
            for off in range(va - va % 8, va + size - 16, 8):
                if struct.unpack_from('<Q', image, off)[0] in name_vas:
                    t = struct.unpack_from('<Q', image, off + 8)[0] - IB
                    if 0 < t < len(image):
                        thunks.add(t)
        for pat in pats:
            toks = pat.split()
            matching = []
            for t in thunks:
                for p in range(t, t + 0x200):
                    if all(image[p + k] == int(x, 16) for k, x in enumerate(toks)):
                        if toks[-1].upper() == 'E8':
                            target = (p + len(toks) - 1) + 5 + struct.unpack_from('<i', image, p + len(toks))[0]
                            matching.append((t, target))
                        break
            ok = len(matching) == 1 and primary_range(matching[0][1]) is not None
            chain_failed += not ok
            detail = ' '.join(f'thunk 0x{IB + t:X} -> 0x{IB + c:X}' for t, c in matching) or 'no thunk holds it'
            print(f'{"CONFIRMED" if ok else "ANCHORS DISAGREE":28} {func:34} UFunction thunk + {pat}: {detail} '
                  f'({len(thunks)} "{func}" entries)   [{file}]')

# UFunction + virtual-slot anchors (prey.cpp, hunt.cpp): the thunk must call the hooked vtable slot
# (scan::BodyContains(thunk, "FF 90 <slot>", n)) - of the entries named <Func>, exactly one thunk holds it n times
THUNK_BODY = re.compile(r'scan::BodyContains\(\w*[Tt]hunk, "([0-9A-Fa-f? ]+)", (\d+)\)')
for path in sorted(glob.glob(os.path.join(src_dir, '*.cpp'))):
    file = os.path.basename(path)
    text = open(path, encoding='utf-8').read()
    funcs, bodies = UFUNC.findall(text), THUNK_BODY.findall(text)
    if not funcs or not bodies:
        continue
    # several UFunctions in one file (areas.cpp): the n-th function goes with the n-th thunk pattern
    paired = len(funcs) == len(bodies) and len(funcs) > 1
    for fi, func in enumerate(funcs):
        name_vas = {IB + a for a in find_ascii(func)}
        thunks = set()
        for va, size, ex in sections:
            if ex:
                continue
            for off in range(va - va % 8, va + size - 16, 8):
                if struct.unpack_from('<Q', image, off)[0] in name_vas:
                    t = struct.unpack_from('<Q', image, off + 8)[0] - IB
                    if 0 < t < len(image):
                        thunks.add(t)
        for pat, n in ([bodies[fi]] if paired else bodies):
            matching = [t for t in thunks if primary_range(t) is not None and body_count(t, pat) == int(n)]
            ok = len(matching) == 1
            chain_failed += not ok
            detail = ' '.join(f'thunk 0x{IB + t:X}' for t in matching) or 'no thunk holds it'
            print(f'{"CONFIRMED" if ok else "ANCHORS DISAGREE":28} {func:34} UFunction thunk calls slot {pat} x{n}: {detail} '
                  f'({len(thunks)} "{func}" entries)   [{file}]')

print(f'\n{len(targets) - failed} of {len(targets)} hook targets resolve uniquely' +
      (f', {chain_failed} cross-check(s) failed' if chain_failed else '') +
      ('' if not failed and not chain_failed else ' - FIX BEFORE RELEASE'))
failed += chain_failed
sys.exit(1 if failed else 0)
