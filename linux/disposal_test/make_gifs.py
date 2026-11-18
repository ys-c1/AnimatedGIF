#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate small GIF89a test sequences for the in-library disposal compositor.

Pure standard library: its own GIF writer + LZW encoder, no third-party
modules, no network access. It also contains the hand-written reference
compositor and writes the expected per-frame canvas dumps that the C test
is compared against.

Color table (4 entries, index 3 doubles as the transparent index):
  0 = black (also the logical-screen background color)
  1 = red
  2 = green
  3 = blue / transparent
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")

PALETTE = [
    (0, 0, 0),       # 0 black / background
    (224, 32, 32),   # 1 red
    (32, 200, 32),   # 2 green
    (32, 32, 224),   # 3 blue (transparent when transparency flag is set)
]
# Pad to a full 256-entry color table, shipped as a per-frame LOCAL table.
# This library's parser only accepts a full 256-entry *global* table inside
# its fixed parse window; local tables have no such restriction. A 256-entry
# table means an 8-bit LZW minimum code size, which is the common real-world
# configuration and what the decoder is optimized for. Frame pixels only use
# indices 0..3.
while len(PALETTE) < 256:
    v = len(PALETTE)
    PALETTE.append(((v * 37) & 0xFF, (v * 73) & 0xFF, (v * 151) & 0xFF))
BG_INDEX = 0
TRANS_INDEX = 3
MIN_CODE = 8  # 256-entry local table -> 8-bit LZW
# local color-table-size field: log2(256)-1 = 7
LCT_BITS = 7


# --------------------------------------------------------------------------
# LZW encoder (GIF variant, LSB-first packed codes, clear-on-table-full)
# --------------------------------------------------------------------------
def lzw_encode(indices, min_code_size):
    # Deliberately simple, spec-correct GIF LZW encoder (byte dictionary,
    # LSB-first packed codes). A clear code is emitted after a small, fixed
    # number of dictionary additions, so the code width never grows beyond
    # the initial (min_code_size+1); this removes every code-width transition
    # edge case and is valid for any conforming GIF decoder. Compression is
    # modest, but the test assets are tiny and decode speed is what matters.
    clear = 1 << min_code_size
    end = clear + 1
    bits = []
    bit_count = 0
    bit_buf = 0
    # stay strictly inside the initial width's code range
    max_entries = (1 << (min_code_size + 1)) - 2

    def emit(code, size):
        nonlocal bit_buf, bit_count
        bit_buf |= code << bit_count
        bit_count += size
        while bit_count >= 8:
            bits.append(bit_buf & 0xFF)
            bit_buf >>= 8
            bit_count -= 8

    def new_dict():
        return {bytes([i]): i for i in range(clear)}

    code_size = min_code_size + 1
    table = new_dict()
    next_code = end + 1
    emit(clear, code_size)

    prefix = b""
    for k in indices:
        k = bytes([k])
        if prefix + k in table:
            prefix += k
            continue
        emit(table[prefix], code_size)
        if next_code > max_entries:
            # periodic clear: width stays at the initial code_size forever
            emit(clear, code_size)
            table = new_dict()
            next_code = end + 1
            prefix = k
            continue
        table[prefix + k] = next_code
        next_code += 1
        prefix = k
    if prefix:
        emit(table[prefix], code_size)
    emit(end, code_size)
    if bit_count:
        bits.append(bit_buf & 0xFF)
    return bytes(bits)


def subblocks(data):
    out = bytearray()
    i = 0
    while i < len(data):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
        i += 255
    out.append(0)  # block terminator
    return bytes(out)


# --------------------------------------------------------------------------
# GIF89a writer
# --------------------------------------------------------------------------
def write_gif(path, width, height, frames, interlace=False):
    """frames: list of dicts(x, y, w, h, disp, transparent(bool), rows=...)"""
    out = bytearray()
    out += b"GIF89a"
    # logical screen descriptor with NO global color table; each frame carries
    # its own local color table instead (this library's parser constraint).
    out += struct.pack("<HH", width, height)
    out += bytes([0x00, BG_INDEX, 0])

    for fr in frames:
        packed_gce = (int(fr["disp"]) << 2) | (1 if fr["transparent"] else 0)
        out += bytes([0x21, 0xF9, 4, packed_gce])
        out += struct.pack("<H", 10)  # delay 100 ms (irrelevant to tests)
        out += bytes([TRANS_INDEX, 0])
        out += b","
        out += struct.pack("<HHHH", fr["x"], fr["y"], fr["w"], fr["h"])
        # image flags: local color table present (+ interlace if requested)
        flags = 0x80 | LCT_BITS | (0x40 if interlace else 0)
        out.append(flags)
        for r, g, b in PALETTE:
            out += bytes([r, g, b])
        out.append(MIN_CODE)
        flat = [px for row in fr["rows"] for px in row]
        out += subblocks(lzw_encode(flat, MIN_CODE))
    out += b";"
    with open(path, "wb") as f:
        f.write(out)


# --------------------------------------------------------------------------
# Hand-written reference compositor (the semantic oracle for the C library)
# --------------------------------------------------------------------------
def reference_canvases(width, height, frames):
    canvas = [[BG_INDEX] * width for _ in range(height)]
    restore = [[BG_INDEX] * width for _ in range(height)]
    prev = None  # (x, y, w, h, disp)
    dumps = []
    for fr in frames:
        x, y, w, h = fr["x"], fr["y"], fr["w"], fr["h"]
        if prev is not None:
            px, py, pw, ph, pdisp = prev
            if pdisp == 2:  # restore to background
                for yy in range(py, py + ph):
                    for xx in range(px, px + pw):
                        canvas[yy][xx] = BG_INDEX
            elif pdisp == 3:  # restore to previous
                for yy in range(py, py + ph):
                    for xx in range(px, px + pw):
                        canvas[yy][xx] = restore[yy][xx]
        if fr["disp"] == 3:  # snapshot before rendering this frame
            for yy in range(y, y + h):
                for xx in range(x, x + w):
                    restore[yy][xx] = canvas[yy][xx]
        for ry in range(h):
            for rx in range(w):
                pxval = fr["rows"][ry][rx]
                if fr["transparent"] and pxval == TRANS_INDEX:
                    continue  # leave current canvas pixel untouched
                canvas[y + ry][x + rx] = pxval
        prev = (x, y, w, h, fr["disp"])
        dumps.append(bytes(px for row in canvas for px in row))
    return dumps


def frame_rows(w, h, fn):
    return [[fn(x, y) for x in range(w)] for y in range(h)]


# --------------------------------------------------------------------------
# Test sequence definitions (16 x 12 canvas)
# --------------------------------------------------------------------------
W, H = 16, 12


def seq_nodisp():
    # Every frame disposal 1 (do not dispose); frames overlap at (2,2) 8x6.
    x, y, w, h = 2, 2, 8, 6
    f0 = dict(x=x, y=y, w=w, h=h, disp=1, transparent=False,
              rows=frame_rows(w, h, lambda x, y: 1))                 # red block
    f1 = dict(x=x, y=y, w=w, h=h, disp=1, transparent=True,
              rows=frame_rows(w, h, lambda x, y: 2 if x < 4 else 3))  # green left
    f2 = dict(x=x, y=y, w=w, h=h, disp=1, transparent=False,
              rows=frame_rows(w, h, lambda x, y: 3 if y < 3 else 1))  # blue/red
    f3 = dict(x=x, y=y, w=w, h=h, disp=1, transparent=True,
              rows=frame_rows(w, h, lambda x, y: 2 if x == 0 else 3))  # green col
    return [f0, f1, f2, f3]


def seq_bg():
    # Every frame disposal 2 (restore to background); frame at (4,3) 8x6.
    x, y, w, h = 4, 3, 8, 6
    f0 = dict(x=x, y=y, w=w, h=h, disp=2, transparent=False,
              rows=frame_rows(w, h, lambda x, y: 1))
    f1 = dict(x=x, y=y, w=w, h=h, disp=2, transparent=False,
              rows=frame_rows(w, h, lambda x, y: 2))
    f2 = dict(x=x, y=y, w=w, h=h, disp=2, transparent=True,
              rows=frame_rows(w, h, lambda x, y: 1 if x < 4 else 3))
    f3 = dict(x=x, y=y, w=w, h=h, disp=2, transparent=False,
              rows=frame_rows(w, h, lambda x, y: 3))
    return [f0, f1, f2, f3]


def seq_prev():
    # Every frame disposal 3 (restore to previous); frame at (3,2) 10x8.
    x, y, w, h = 3, 2, 10, 8
    f0 = dict(x=x, y=y, w=w, h=h, disp=3, transparent=False,
              rows=frame_rows(w, h, lambda x, y: 1))
    f1 = dict(x=x, y=y, w=w, h=h, disp=3, transparent=True,
              rows=frame_rows(w, h, lambda x, y: 2 if x < 5 else 3))
    f2 = dict(x=x, y=y, w=w, h=h, disp=3, transparent=True,
              rows=frame_rows(w, h, lambda x, y: 2 if y < 4 else 3))
    f3 = dict(x=x, y=y, w=w, h=h, disp=3, transparent=False,
              rows=frame_rows(w, h, lambda x, y: 2))
    f4 = dict(x=x, y=y, w=w, h=h, disp=3, transparent=True,
              rows=frame_rows(w, h,
                              lambda x, y: 1 if 4 <= x < 6 and 3 <= y < 5 else 3))
    return [f0, f1, f2, f3, f4]


def seq_mixed():
    # Moving rectangles, mixed disposal methods, overlapping and not.
    frames = []
    frames.append(dict(x=1, y=1, w=6, h=4, disp=1, transparent=False,
                       rows=frame_rows(6, 4, lambda x, y: 1)))           # red
    frames.append(dict(x=8, y=6, w=6, h=4, disp=2, transparent=False,
                       rows=frame_rows(6, 4, lambda x, y: 2)))           # green
    frames.append(dict(x=4, y=3, w=7, h=5, disp=3, transparent=True,
                       rows=frame_rows(7, 5,
                                       lambda x, y: 2 if x == 0 else 3)))  # col
    frames.append(dict(x=2, y=2, w=4, h=3, disp=1, transparent=False,
                       rows=frame_rows(4, 3, lambda x, y: 3)))           # blue
    frames.append(dict(x=10, y=8, w=4, h=3, disp=2, transparent=True,
                       rows=frame_rows(4, 3,
                                       lambda x, y: 1 if x == 0 and y == 0 else 3)))
    frames.append(dict(x=4, y=3, w=7, h=5, disp=1, transparent=True,
                       rows=frame_rows(7, 5,
                                       lambda x, y: 1 if x == 6 else 3)))
    return frames


def seq_perf(width, height, nframes=10):
    # Full-canvas frames with long horizontal runs (keeps the file small),
    # cycling disposal methods and alternating transparency.
    frames = []
    disps = [1, 2, 3, 2, 1, 3, 1, 2, 3, 1]
    for f in range(nframes):
        transparent = (f % 2 == 0)

        def px(x, y, f=f, transparent=transparent):
            base = (x // 16 + f) % 4
            if transparent and (x // 16) == 5:
                return TRANS_INDEX
            return base

        frames.append(dict(x=0, y=0, w=width, h=height,
                           disp=disps[f % len(disps)],
                           transparent=transparent,
                           rows=frame_rows(width, height, px)))
    return frames


def main():
    os.makedirs(ASSETS, exist_ok=True)
    sequences = {
        "seq_nodisp": (seq_nodisp(), False),
        "seq_bg": (seq_bg(), False),
        "seq_prev": (seq_prev(), False),
        "seq_mixed": (seq_mixed(), False),
        "seq_interlace": (seq_bg(), True),  # same semantics, interlaced storage
    }
    manifest = {}
    for name, (frames, interlace) in sequences.items():
        gif_path = os.path.join(ASSETS, name + ".gif")
        write_gif(gif_path, W, H, frames, interlace=interlace)
        dumps = reference_canvases(W, H, frames)
        exp_path = os.path.join(ASSETS, name + ".expected.bin")
        with open(exp_path, "wb") as f:
            for d in dumps:
                f.write(d)
        manifest[name] = {
            "gif": os.path.relpath(gif_path, HERE),
            "expected": os.path.relpath(exp_path, HERE),
            "width": W, "height": H,
            "frames": [
                {"x": fr["x"], "y": fr["y"], "w": fr["w"], "h": fr["h"],
                 "disp": fr["disp"], "transparent": fr["transparent"],
                 "rows": fr["rows"]}
                for fr in frames
            ],
            "interlaced": interlace,
        }

    pw, ph = 640, 480
    perf_frames = seq_perf(pw, ph)
    perf_path = os.path.join(ASSETS, "perf_640x480.gif")
    write_gif(perf_path, pw, ph, perf_frames)
    manifest["perf_640x480"] = {
        "gif": os.path.relpath(perf_path, HERE),
        "width": pw, "height": ph, "frames": len(perf_frames),
    }
    with open(os.path.join(ASSETS, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)

    for name, info in manifest.items():
        gif = os.path.join(HERE, info["gif"])
        print("%-14s %6d bytes  %d frame(s)  %dx%d%s" % (
            name, os.path.getsize(gif),
            len(info["frames"]) if isinstance(info["frames"], list) else info["frames"],
            info["width"], info["height"],
            "  interlaced" if info.get("interlaced") else ""))


if __name__ == "__main__":
    sys.exit(main())
