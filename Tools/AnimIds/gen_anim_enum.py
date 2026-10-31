#!/usr/bin/env python3
"""Regenerates the animation enumerators in WoW/Source/Object/ObjectClient/Types.h from AnimationNames.txt.

  python3 Tools/AnimIds/gen_anim_enum.py            rewrite the generated block and ANIM_COUNT
  python3 Tools/AnimIds/gen_anim_enum.py --check    verify only (exit 1 when Types.h is out of date)

The names file lists animation names by AnimationData row id (see its header; '@unnamed A B' reserves ids whose
names are not known yet as ANIM_UNNAMED_<id>). Ids that already have a hand-written
enumerator in Types.h (everything before the generated block) are only VERIFIED: the name derived from the file must
equal the existing enumerator, which is what guarantees the new names follow the existing naming style. Ids after
them become the generated block, and ANIM_COUNT is set to the highest id + 1.

Naming: ANIM_ + the CamelCase name in UPPER_SNAKE (Carry2H -> ANIM_CARRY_2H, WAWalk -> ANIM_WA_WALK,
PetBattleCustom0 -> ANIM_PET_BATTLE_CUSTOM_0), the same rule the existing enumerators follow.
"""
import argparse
import os
import re
import sys

BEGIN = "    // BEGIN GENERATED ANIMATION IDS (Tools/AnimIds/gen_anim_enum.py - edit AnimationNames.txt, not this block)"
END = "    // END GENERATED ANIMATION IDS"


def enumerator(name):
    s = re.sub(r'(?<=[a-z])(?=[A-Z])', '_', name)          # fooBar   -> foo_Bar
    s = re.sub(r'(?<=[A-Z0-9])(?=[A-Z][a-z])', '_', s)      # ABCd     -> AB_Cd (acronym, then a word)
    s = re.sub(r'(?<=[A-Za-z])(?=[0-9])', '_', s)           # letters|digits
    return 'ANIM_' + s.upper()


def read_names(path):
    ids, nxt = {}, None
    for raw in open(path, encoding='utf-8'):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('@first'):
            nxt = int(line.split()[1])
            continue
        if line.startswith('@unnamed'):
            # '@unnamed A B': ids A..B exist but their names are not known; they become ANIM_UNNAMED_<id>
            parts = line.split()
            first, last = int(parts[1]), int(parts[2])
            if last < first:
                sys.exit(f"{path}: '{line}' is an empty range")
            for i in range(first, last + 1):
                if i in ids:
                    sys.exit(f"{path}: id {i} listed twice")
                ids[i] = f"Unnamed{i}"
            nxt = last + 1
            continue
        m = re.match(r'^(\d+)\s+(\S+)$', line)
        if m:
            nxt = int(m.group(1)); line = m.group(2)
        if nxt is None:
            sys.exit(f"{path}: a name before any '@first N' or explicit id")
        if not re.match(r'^[A-Za-z][A-Za-z0-9]*$', line):
            sys.exit(f"{path}: '{line}' is not a usable name")
        if nxt in ids:
            sys.exit(f"{path}: id {nxt} listed twice")
        ids[nxt] = line
        nxt += 1
    return ids


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, '..', '..'))
    ap = argparse.ArgumentParser()
    ap.add_argument('--types', default=os.path.join(root, 'WoW/Source/Object/ObjectClient/Types.h'))
    ap.add_argument('--names', default=os.path.join(here, 'AnimationNames.txt'))
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()

    raw = open(a.types, 'rb').read().decode('utf-8')
    eol = '\r\n' if '\r\n' in raw else '\n'
    text = raw.replace('\r\n', '\n')

    # the enum: everything up to (not including) the generated block or ANIM_COUNT
    m = re.search(r'enum ANIMATION_ID[^\n]*\{\n(.*?)\n\};', text, re.S)
    if not m:
        sys.exit("could not find the animation enum in " + a.types)
    body = m.group(1)
    cut = body.find(BEGIN)
    if cut < 0:
        cut = body.find('    ANIM_COUNT')
    hand = body[:cut].rstrip('\n')

    existing = {}
    for l in hand.split('\n'):
        mm = re.match(r'\s*(ANIM_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+|\d+),', l)
        if mm:
            existing[int(mm.group(2), 0)] = mm.group(1)
    last_hand = max(existing)

    names = read_names(a.names)

    # verify the overlap, so the naming rule is proven against the real enumerators every run
    bad = [(i, names[i], enumerator(names[i]), existing[i]) for i in sorted(names) if i in existing and enumerator(names[i]) != existing[i]]
    if bad:
        for i, n, got, want in bad:
            print(f"id {i}: {n} -> {got}, but Types.h has {want}", file=sys.stderr)
        sys.exit("names file disagrees with the hand-written enumerators; fix the file (numbering or names)")
    overlap = sum(1 for i in names if i in existing)

    new_ids = sorted(i for i in names if i > last_hand)
    if not new_ids:
        sys.exit("no ids past the existing enum in the names file")
    if new_ids[0] != last_hand + 1:
        sys.exit(f"gap: the enum ends at {last_hand} but the names file resumes at {new_ids[0]}")
    for p, q in zip(new_ids, new_ids[1:]):
        if q != p + 1:
            sys.exit(f"gap in the names file between {p} and {q}")

    used = set(existing.values())
    lines = []
    for i in new_ids:
        e = enumerator(names[i])
        if e in used:
            sys.exit(f"id {i}: {names[i]} would be {e}, which already exists")
        used.add(e)
        lines.append(f"    {e} = 0x{i:X},")
    count = new_ids[-1] + 1

    block = BEGIN + '\n' + '\n'.join(lines) + '\n' + END + f"\n    ANIM_COUNT = 0x{count:X},"
    new_body = hand + '\n' + block
    new_text = text[:m.start(1)] + new_body + text[m.end(1):]
    out = new_text.replace('\n', eol)

    if a.check:
        if out != raw:
            print("Types.h is out of date: run Tools/AnimIds/gen_anim_enum.py", file=sys.stderr)
            sys.exit(1)
        print(f"up to date: {len(new_ids)} generated enumerators, ANIM_COUNT = {count} (0x{count:X}), {overlap} overlapping ids verified")
        return
    open(a.types, 'wb').write(out.encode('utf-8'))
    print(f"wrote {len(new_ids)} enumerators (ids {new_ids[0]}..{new_ids[-1]}), ANIM_COUNT = {count} (0x{count:X}), {overlap} overlapping ids verified")


if __name__ == '__main__':
    main()
