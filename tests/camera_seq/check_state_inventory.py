#!/usr/bin/env python3
# Step 4 guard (inverted in step 5): the CamTracker in src/camtracker.h must own EVERY piece of mutable camera state; since the move the extracted regions must contain none.
# Why: the step-5 code move is mechanical, and the one thing a mechanical move can silently get wrong is a missed `static` (a function-local static or a
# global that stays shared between tracker instances, i.e. state that is not in the struct). This script finds every non-const `static` variable in
# camera_extract.inc / camhold_extract.inc and requires an entry for it in the STATE-MAP block of camtracker.h - and the other way round (no stale entries).
#
# STATE-MAP line:   //   <kind> <Function::name | ::name> -> <member>      (kind: camera | camhold | stay | ext)
#   camera / camhold : a static found in that region   -> the CamTracker member it becomes   (Function = enclosing function, '' = global; camhold statics all belong to 'Frame')
#   stay             : a static found in the region that is NOT tracker state and stays a host global
#   ext              : state that is defined OUTSIDE the regions today (the test shim defines it) and moves into the tracker
# STEP 5 meaning of the STATE-MAP: provenance (old name -> member). Checked: (1) no mutable static in either extract, (2) every member exists, (3) no host file still uses an old global name.
# usage: check_state_inventory.py <dir with *_extract.inc> <camtracker.h> <cam_shim.h> <src dir>      exit 1 = mismatch
import re, sys, os

def strip_comments(line):
    out, q, i = [], False, 0
    while i < len(line):
        c = line[i]
        if c == '"' and (i == 0 or line[i - 1] != '\\'): q = not q
        if not q and line.startswith('//', i): break
        out.append(c); i += 1
    return ''.join(out)

def depth0_cut(s, stops):
    """index of the first char in `stops` at bracket depth 0 (or len(s))"""
    d = 0
    for i, c in enumerate(s):
        if c in '([{': 
            if d == 0 and c in stops: return i
            d += 1
        elif c in ')]}': d -= 1
        elif d == 0 and c in stops: return i
    return len(s)

def split_commas(s):
    parts, d, cur = [], 0, ''
    for c in s:
        if c in '([{': d += 1
        elif c in ')]}': d -= 1
        if c == ',' and d == 0: parts.append(cur); cur = ''
        else: cur += c
    parts.append(cur); return parts

def find_statics(path, region, inits=None):
    if inits is None: inits = {}
    found = []   # (context, name)
    cur = ''
    for raw in open(path, encoding='utf-8').read().split('\n'):
        if raw.startswith('// GENERATED'): continue
        s = strip_comments(raw)
        if s.startswith('}'): cur = ''
        for m in re.finditer(r'\bstatic\b', s):
            rest = s[m.end():]
            stmt = rest[:depth0_cut(rest, ';')]
            head_end = depth0_cut(stmt, '={[')
            head_probe = stmt[:depth0_cut(stmt, '={;')]
            is_func = '(' in head_probe
            col0 = (m.start() == 0)
            if is_func:
                if col0:
                    mm = re.search(r'([A-Za-z_]\w*)\s*\(', head_probe); cur = mm.group(1) if mm else ''
                continue
            ctx = 'Frame' if region == 'camhold' else (cur if not col0 else '')
            if col0: cur = ''
            decls = split_commas(stmt)
            for k, d in enumerate(decls):
                head = d[:depth0_cut(d, '={[')]
                words = re.findall(r'[A-Za-z_]\w*', head)
                if not words: continue
                if k == 0 and ('const' in words[:-1] or 'constexpr' in words[:-1]): break   # constant table, not state
                found.append((ctx, words[-1]))
                inits[(ctx, words[-1])] = d[len(head):]
    return found

def norm_init(t):
    """normalise an initialiser: '' / {} / {0} / 0 / 0.f / false are all 'zero'; numbers compare by value; whitespace and a leading '=' do not matter"""
    t = re.sub(r'\s+', '', t)
    t = re.sub(r'^(\[[^\]]*\])+', '', t).lstrip('=')   # drop array dimensions: `[4]={-1,-1}` -> `{-1,-1}`
    if t in ('', '{}', '{0}', '0', 'false', '{false}', '{0.f}', '0.f', '0.0', '{0.0}'): return '0'
    if t == 'true': return '1'
    m = re.fullmatch(r'\{?(-?\d+(?:\.\d*)?)f?\}?', t)
    if m: return str(float(m.group(1)))
    return t

def struct_body(code, name):
    i = code.index('struct ' + name + ' {') + len('struct ' + name + ' {'); d = 1; j = i
    while d and j < len(code):
        d += {'{': 1, '}': -1}.get(code[j], 0); j += 1
    return code[i:j - 1]

def member_inits(code, structs):
    out = {}
    for sn in structs:
        body = struct_body(code, sn)
        for stmt in split_top(body):
            probe = stmt[:depth0_cut(stmt, '={;')]
            if '(' in probe or not probe.strip(): continue
            decls = split_commas(stmt)
            for k, d in enumerate(decls):
                head = d[:depth0_cut(d, '={[')]; words = re.findall(r'[A-Za-z_]\w*', head)
                if words and words[-1] not in out: out[words[-1]] = d[len(head):]
    return out

def split_top(body):
    parts, d, cur, opens = [], 0, '', []
    for c in body:
        cur += c
        if c in '([{':
            if c == '{': opens.append(len(cur) - 1)
            d += 1
        elif c in ')]}':
            d -= 1
            if c == '}':
                o = opens.pop(); before = cur[:o].rstrip()
                if d == 0 and (before.endswith(')') or before.endswith('const')): cur = ''   # end of a function body (inline member function): not a data member
        if c == ';' and d == 0: parts.append(cur.strip().rstrip(';')); cur = ''
    return [p for p in parts if p]

def struct_text(code, name):
    m = re.search(r'struct ' + name + r' \{.*?\};', code, re.S)
    return re.sub(r'\s+', ' ', m.group(0)) if m else None

def parse_map(hdr):
    txt = open(hdr, encoding='utf-8').read()
    m = re.search(r'STATE-MAP-BEGIN(.*?)STATE-MAP-END', txt, re.S)
    if not m: print('check_state_inventory: ERROR: no STATE-MAP block in ' + hdr); sys.exit(1)
    ent = []
    for l in m.group(1).split('\n'):
        mm = re.match(r'//\s+(camera|camhold|stay|ext)\s+(\S*::\S*|\S+)\s+->\s+(\S+)', l)
        if mm: ent.append(mm.groups())
        elif l.strip().startswith('//') and '->' in l and not l.strip().startswith('// ('): print('check_state_inventory: WARNING: unparsed map line: ' + l.strip())
    return ent, txt

def main():
    d, hdr, shim, srcdir = sys.argv[1:5]
    ent, htxt = parse_map(hdr)
    stxt = open(shim, encoding='utf-8').read()
    code = '\n'.join(strip_comments(l) for l in re.sub(r'STATE-MAP-BEGIN.*?STATE-MAP-END', '', htxt, flags=re.S).split('\n'))
    bad = 0
    # 1. STEP 5 RULE: the extracted member functions hold NO mutable static any more. A function-local `static int x` that creeps back in (or a new global in a region) would be
    #    state shared between tracker instances / invisible to CamTracker - exactly what the step-4 layout was made to prevent.
    for region, fn in (('camera', 'camera_extract.inc'), ('camhold', 'camhold_extract.inc')):
        for ctx, nm in sorted(find_statics(os.path.join(d, fn), region)):
            print('MUTABLE STATIC in %s: %s::%s  - tracker state must be a CamTracker member (add it to camtracker.h + the STATE-MAP)' % (fn, ctx or '(global)', nm)); bad += 1
    # 2. every STATE-MAP entry that moved into the tracker is a declared member; 'ext' symbols are still provided by the shim (tests use them through macros)
    legacy = []
    for kind, k, member in ent:
        if kind == 'stay': continue
        root = re.match(r'[A-Za-z_]\w*', member).group(0)
        if not re.search(r'\b' + re.escape(root) + r'\b', code): print('MEMBER NOT DECLARED in camtracker.h: %s (map entry %s)' % (member, k)); bad += 1
        nm = k.split('::')[1]
        if kind == 'ext' and not re.search(r'\b' + re.escape(nm) + r'\b', stxt): print('EXT symbol not defined by cam_shim.h: ' + nm); bad += 1
        if nm.startswith('g_'): legacy.append(nm)
    # 3. no consumer in the host still uses a legacy global (main.cpp + *.inc, comments ignored); the 'stay' globals are still declared there, OUTSIDE both regions
    host = {}
    for fn in sorted(os.listdir(srcdir)):
        if fn.endswith('.cpp') or fn.endswith('.inc'): host[fn] = [strip_comments(l) for l in open(os.path.join(srcdir, fn), encoding='utf-8').read().split('\n')]
    for fn, lines in host.items():
        for i, l in enumerate(lines):
            for nm in legacy:
                if re.search(r'\b' + re.escape(nm) + r'\b', l): print('LEGACY GLOBAL still used: %s:%d  %s' % (fn, i + 1, nm)); bad += 1
    for kind, k, member in ent:
        if kind != 'stay': continue
        nm = k.split('::')[1]
        if not any(re.search(r'\bstatic\b.*\b' + re.escape(nm) + r'\b', l) for l in host['main.cpp']): print('STAY global %s is not declared in main.cpp' % nm); bad += 1
        for fn in ('camera_extract.inc', 'camhold_extract.inc'):
            if re.search(r'\b' + re.escape(nm) + r'\b', '\n'.join(strip_comments(l) for l in open(os.path.join(d, fn), encoding='utf-8').read().split('\n'))): print('STAY global %s leaked into %s (ProbeView statistics are not tracker state)' % (nm, fn)); bad += 1
    n = len(ent)
    if bad: print('state inventory: %d problem(s)' % bad); sys.exit(1)
    print('state inventory ok: %d map entries (%d camera, %d camhold, %d stay, %d ext) - no mutable static left in either region, every member declared, no legacy global used by the host' % (n, *[sum(1 for e in ent if e[0] == k) for k in ('camera', 'camhold', 'stay', 'ext')]))
main()
