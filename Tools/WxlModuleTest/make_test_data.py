#!/usr/bin/env python3
"""Writes data/item/objectcomponents/head/plume_test.phys and .phy2 for host_test: a plume of three dynamic
segments hanging from one kinematic anchor, in the retail .phys layout; the .phy2 is made by
Tools/m2_extract_phys.py, the same converter the client's files come from.

    python make_test_data.py          (run it in the folder host_test is run from)
"""
import os
import struct
import subprocess
import sys

def chunk(tag, payload):
    return tag[::-1] + struct.pack('<I', len(payload)) + payload   # tags are stored byte-reversed

def body(typ, bone, pos, first, count, grav, inertia, lin, ang, carry, group):
    b = struct.pack('<HH3f', typ, bone, *pos) + struct.pack('<H', first) + b'\0\0' + struct.pack('<i', count)
    b += struct.pack('<5f', grav, inertia, lin, ang, carry) + struct.pack('<H', group) + b'\0\0'
    assert len(b) == 0x30
    return b

def shape(st, si, friction, restitution, density):
    return struct.pack('<hh', st, si) + b'\0' * 4 + struct.pack('<3f', friction, restitution, density) + b'\0' * 12

SEGS, L = 3, 0.25
bodies = body(0, 1, (0, 0, 0), 0, 1, 0, 1, 0, 0, 0.9, 0)          # kinematic anchor on bone 1
shapes = shape(2, 0, 0.5, 0.0, 1.0)                                 # sphere
sphs = struct.pack('<4f', 0, 0, 0, 0.05)
caps = joints = shj = b''
for i in range(SEGS):
    bodies += body(1, 2 + i, (0, 0, -L * i), 1 + i, 1, 1.0, 1.0, 2.0, 2.0, 0.0, 0)   # dynamic segment on bone 2+i
    shapes += shape(1, i, 0.5, 0.0, 1.0)
    caps += struct.pack('<7f', 0, 0, 0, 0, 0, -L, 0.03)
    frame = (1, 0, 0, 0, 1, 0, 0, 0, 1)
    anchor = (0, 0, 0) if i == 0 else (0, 0, -L)
    rec = struct.pack('<9f', *frame) + struct.pack('<3f', *anchor) + struct.pack('<9f', *frame) + struct.pack('<3f', 0, 0, 0)
    rec += struct.pack('<4f', -45, 45, 45, 0) + b'\0' * 4 + struct.pack('<2f', 0, 0)
    assert len(rec) == 0x7C
    shj += rec
    joints += struct.pack('<II', i, i + 1) + b'\0' * 4 + struct.pack('<hh', 1, i)

blob = (chunk(b'PHYS', struct.pack('<I', 6)) + chunk(b'BDY4', bodies) + chunk(b'SHP2', shapes) + chunk(b'CAPS', caps) +
        chunk(b'SPHS', sphs) + chunk(b'JOIN', joints) + chunk(b'SHJ2', shj))
folder = os.path.join('data', 'item', 'objectcomponents', 'head')
os.makedirs(folder, exist_ok=True)
phys = os.path.join(folder, 'plume_test.phys')
open(phys, 'wb').write(blob)
tool = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'm2_extract_phys.py')
sys.exit(subprocess.call([sys.executable, tool, phys, '--overwrite']))
