#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
M2 Particle Texture Extractor
-----------------------------
Reads a retail (Cataclysm and newer, version >= 272) World of Warcraft .m2
model and writes a CSV with the textures used by every particle emitter,
including the 3 textures of multitextured emitters.

Works on Python 2.6+ and Python 3 (Tkinter UI, or command line).

GUI:   python m2_particle_textures.py
CLI:   python m2_particle_textures.py model.m2 [listfile.csv] [output_dir]

Output: <modelname>.csv next to the model (or in the chosen output folder)

Columns:
  1  emitter ID              (index of the emitter in the M2 particle array,
                              or the stored ID field - see USE_STORED_ID)
  2  particle texture ID     (0, 1 or 2; non multitextured emitters = 0)
  3  full texture path       (or FileDataID / texture type if no path exists)
  4  multitextured           (Yes / No)
  5  blending                (number 0-4, optional, see INCLUDE_BLENDING;
                              legend is shown in the UI)
"""
from __future__ import print_function

import os
import sys
import csv
import struct

# ---------------------------------------------------------------- settings
INCLUDE_BLENDING = True   # add a 5th column with the emitter blending mode
USE_STORED_ID = False     # True = use the emitter's own ID field (often -1)

PY3 = sys.version_info[0] >= 3

PARTICLE_STRIDE = 492     # size of one particle emitter record (version >= 272)
FLAG_MULTITEXTURE = 0x10000000

BLENDING = {
    0: "Opaque (no blend, no alpha test)",
    1: "Blend SRC_COLOR, ONE",
    2: "Alpha blend (SRC_ALPHA, ONE_MINUS_SRC_ALPHA)",
    3: "Alpha test (no blend)",
    4: "Additive (SRC_ALPHA, ONE)",
}


def blending_legend():
    keys = sorted(BLENDING.keys())
    return "\n".join(["  %d = %s" % (k, BLENDING[k]) for k in keys])

TEXTURE_TYPES = {
    1: "Skin", 2: "Object skin", 3: "Weapon blade", 4: "Weapon handle",
    5: "Environment", 6: "Hair", 7: "Facial hair", 8: "Skin extra",
    9: "UI skin", 10: "Tauren mane", 11: "Monster skin 1",
    12: "Monster skin 2", 13: "Monster skin 3", 14: "Item icon",
}


# ---------------------------------------------------------------- helpers
def to_text(raw):
    if PY3:
        return raw.decode("utf-8", "replace")
    return raw


def read_cstring(data, offset, length):
    raw = data[offset:offset + length]
    zero = raw.find(b"\x00")
    if zero >= 0:
        raw = raw[:zero]
    return to_text(raw)


def u32(data, pos):
    return struct.unpack_from("<I", data, pos)[0]


def load_listfile(path, wanted_ids):
    """Stream a community listfile ('id;path' per line), keeping only the
    FileDataIDs we care about, so huge listfiles stay cheap."""
    found = {}
    if not path or not wanted_ids:
        return found
    f = open(path, "rb")
    try:
        for line in f:
            sep = line.find(b";")
            if sep < 0:
                continue
            try:
                fid = int(line[:sep])
            except ValueError:
                continue
            if fid in wanted_ids:
                found[fid] = to_text(line[sep + 1:].strip())
                if len(found) == len(wanted_ids):
                    break
    finally:
        f.close()
    return found


# ---------------------------------------------------------------- M2 parsing
def parse_m2(path):
    """Returns (textures, txid, emitters).
       textures: list of dicts {type, flags, name}
       txid:     list of FileDataIDs (empty if no TXID chunk)
       emitters: list of dicts {index, id, flags, multi, tex, blending}"""
    f = open(path, "rb")
    try:
        data = f.read()
    finally:
        f.close()

    if len(data) < 16:
        raise ValueError("File is too small to be an M2 model.")

    magic = data[0:4]
    txid = []
    if magic == b"MD21":
        base = 8                       # offsets are relative to MD21 chunk data
        md21_size = u32(data, 4)
        pos = 8 + md21_size
        while pos + 8 <= len(data):    # read the trailing chunks
            tag = data[pos:pos + 4]
            size = u32(data, pos + 4)
            if tag == b"TXID":
                count = size // 4
                txid = list(struct.unpack_from("<%dI" % count, data, pos + 8))
            pos += 8 + size
    elif magic == b"MD20":
        base = 0
    else:
        raise ValueError("Not an M2 file (bad magic).")

    version = u32(data, base + 4)
    if version < 272:
        raise ValueError(
            "M2 version %d is not supported (needs Cataclysm+ / version >= 272)."
            % version)

    # ---- textures (header 0x50)
    n_tex, ofs_tex = struct.unpack_from("<II", data, base + 0x50)
    textures = []
    for i in range(n_tex):
        p = base + ofs_tex + i * 16
        ttype, tflags, nlen, nofs = struct.unpack_from("<IIII", data, p)
        name = ""
        if nlen > 1:
            name = read_cstring(data, base + nofs, nlen)
        textures.append({"type": ttype, "flags": tflags, "name": name})

    # ---- particle emitters (header 0x128)
    n_par, ofs_par = struct.unpack_from("<II", data, base + 0x128)
    if n_par and base + ofs_par + n_par * PARTICLE_STRIDE > len(data):
        raise ValueError("Particle block runs past the end of the file; "
                         "unexpected M2 layout.")
    emitters = []
    for i in range(n_par):
        p = base + ofs_par + i * PARTICLE_STRIDE
        eid = struct.unpack_from("<i", data, p)[0]
        flags = u32(data, p + 4)
        tex_bits = struct.unpack_from("<H", data, p + 22)[0]
        blend = struct.unpack_from("<B", data, p + 40)[0]
        multi = bool(flags & FLAG_MULTITEXTURE)
        tex = [tex_bits & 0x1F, (tex_bits >> 5) & 0x1F, (tex_bits >> 10) & 0x1F]
        emitters.append({"index": i, "id": eid, "flags": flags,
                         "multi": multi, "tex": tex, "blending": blend})
    return textures, txid, emitters


def describe_texture(index, textures, txid, listfile_paths):
    if index >= len(textures):
        return "(texture index %d out of range, model has %d textures)" % (
            index, len(textures))
    t = textures[index]
    if t["type"] != 0:
        return "(texture type %d: %s)" % (
            t["type"], TEXTURE_TYPES.get(t["type"], "unknown"))
    if t["name"]:
        return t["name"]
    if index < len(txid):
        fid = txid[index]
        if fid in listfile_paths:
            return listfile_paths[fid]
        return "FileDataID %d" % fid
    return "(no path stored)"


def build_rows(textures, txid, emitters, listfile_path=None):
    wanted = set()
    for e in emitters:
        used = e["tex"] if e["multi"] else e["tex"][:1]
        for ti in used:
            if ti < len(textures) and textures[ti]["type"] == 0 \
                    and not textures[ti]["name"] and ti < len(txid):
                wanted.add(txid[ti])
    listfile_paths = load_listfile(listfile_path, wanted)

    rows = []
    for e in emitters:
        eid = e["id"] if USE_STORED_ID else e["index"]
        used = e["tex"] if e["multi"] else e["tex"][:1]
        for slot, ti in enumerate(used):
            row = [eid, slot,
                   describe_texture(ti, textures, txid, listfile_paths),
                   "Yes" if e["multi"] else "No"]
            if INCLUDE_BLENDING:
                row.append(e["blending"])
            rows.append(row)
    return rows


def write_csv(path, rows):
    header = ["Emitter ID", "Particle texture ID", "Texture path",
              "Multitextured"]
    if INCLUDE_BLENDING:
        header.append("Blending")
    if PY3:
        f = open(path, "w", newline="")
    else:
        f = open(path, "wb")
    try:
        w = csv.writer(f)
        w.writerow(header)
        for r in rows:
            w.writerow(r)
    finally:
        f.close()


def process(m2_path, listfile_path=None, out_dir=None):
    textures, txid, emitters = parse_m2(m2_path)
    rows = build_rows(textures, txid, emitters, listfile_path)
    name = os.path.splitext(os.path.basename(m2_path))[0]
    if not out_dir:
        out_dir = os.path.dirname(os.path.abspath(m2_path))
    out_path = os.path.join(out_dir, name + ".csv")
    write_csv(out_path, rows)
    return out_path, rows, len(emitters)


# ---------------------------------------------------------------- GUI
def run_gui():
    try:
        import Tkinter as tk
        import tkFileDialog as filedialog
        import tkMessageBox as messagebox
    except ImportError:
        import tkinter as tk
        from tkinter import filedialog, messagebox

    root = tk.Tk()
    root.title("M2 Particle Texture Extractor")

    m2_var = tk.StringVar()
    list_var = tk.StringVar()
    out_var = tk.StringVar()

    def browse_m2():
        p = filedialog.askopenfilename(
            title="Select M2 model",
            filetypes=[("M2 models", "*.m2"), ("All files", "*.*")])
        if p:
            m2_var.set(p)
            if not out_var.get():
                out_var.set(os.path.dirname(p))

    def browse_list():
        p = filedialog.askopenfilename(
            title="Select listfile (optional, 'id;path' format)",
            filetypes=[("Listfile", "*.csv *.txt"), ("All files", "*.*")])
        if p:
            list_var.set(p)

    def browse_out():
        p = filedialog.askdirectory(title="Select output folder")
        if p:
            out_var.set(p)

    def log(text):
        output.config(state="normal")
        output.delete("1.0", "end")
        output.insert("end", text)
        output.config(state="disabled")

    def extract():
        m2 = m2_var.get().strip()
        if not m2 or not os.path.isfile(m2):
            messagebox.showerror("Error", "Please select a valid .m2 file.")
            return
        lf = list_var.get().strip() or None
        if lf and not os.path.isfile(lf):
            messagebox.showerror("Error", "Listfile not found.")
            return
        out_dir = out_var.get().strip() or None
        try:
            out_path, rows, n = process(m2, lf, out_dir)
        except Exception as e:
            log("ERROR: %s" % e)
            messagebox.showerror("Error", str(e))
            return
        lines = ["Saved: %s" % out_path,
                 "Emitters: %d    Rows: %d" % (n, len(rows)), ""]
        for r in rows:
            lines.append(" | ".join([str(x) for x in r]))
        log("\n".join(lines))

    tk.Label(root, text="M2 model:").grid(row=0, column=0, sticky="e")
    tk.Entry(root, textvariable=m2_var, width=60).grid(row=0, column=1)
    tk.Button(root, text="Browse...", command=browse_m2).grid(row=0, column=2)

    tk.Label(root, text="Listfile (optional):").grid(row=1, column=0, sticky="e")
    tk.Entry(root, textvariable=list_var, width=60).grid(row=1, column=1)
    tk.Button(root, text="Browse...", command=browse_list).grid(row=1, column=2)

    tk.Label(root, text="Output folder:").grid(row=2, column=0, sticky="e")
    tk.Entry(root, textvariable=out_var, width=60).grid(row=2, column=1)
    tk.Button(root, text="Browse...", command=browse_out).grid(row=2, column=2)

    tk.Button(root, text="Extract to CSV", command=extract).grid(
        row=3, column=1, pady=6)

    legend = tk.LabelFrame(root, text="Blending legend (last CSV column)")
    legend.grid(row=3, column=0, sticky="w", padx=4, pady=4)
    tk.Label(legend, text=blending_legend(), justify="left",
             anchor="w").pack(side="left", padx=4)

    frame = tk.Frame(root)
    frame.grid(row=4, column=0, columnspan=3, sticky="nsew")
    scroll = tk.Scrollbar(frame)
    scroll.pack(side="right", fill="y")
    output = tk.Text(frame, width=100, height=18, state="disabled",
                     yscrollcommand=scroll.set, wrap="none")
    output.pack(side="left", fill="both", expand=True)
    scroll.config(command=output.yview)

    root.mainloop()


# ---------------------------------------------------------------- entry
def main(argv):
    if len(argv) >= 2:
        listfile = argv[2] if len(argv) > 2 else None
        out_dir = argv[3] if len(argv) > 3 else None
        out_path, rows, n = process(argv[1], listfile, out_dir)
        print("Saved %s (%d emitters, %d rows)" % (out_path, n, len(rows)))
        print("Blending legend:\n" + blending_legend())
        for r in rows:
            print(" | ".join([str(x) for x in r]))
    else:
        run_gui()


if __name__ == "__main__":
    main(sys.argv)
