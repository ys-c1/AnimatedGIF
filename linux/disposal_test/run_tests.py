#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
One-command test runner for the disposal compositor.

  1. (re)generate the small GIF assets + reference canvases (make_gifs.py)
  2. compile the C harness against the in-tree library
  3. decode every sequence with the normal and the Turbo LZW path and
     compare each composited frame byte-for-byte with the hand-written
     reference compositor
  4. explicitly assert that disposal-2 leaves zero residual pixels and that
     untouched background pixels stay 0
  5. benchmark 640x480 compositing (budget: < 2 ms/frame)

Python standard library only.
"""
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "assets")
BUILD = os.path.join(HERE, "build")
SRC = os.path.join(HERE, "..", "..", "src")
BG = 0

failures = []


def fail(msg):
    failures.append(msg)
    print("  FAIL: " + msg)


def run(cmd, **kw):
    print("$ " + " ".join(cmd))
    return subprocess.run(cmd, check=True, cwd=HERE, **kw)


def residual_checks(name, info, dumps):
    """Explicit acceptance checks beyond byte-exact reference equality.

    Residual rule (disposal method 2): after a frame with disposal 2 is
    cleared, every pixel in its rectangle that the *following* frame does not
    repaint -- either because it lies outside that frame, or because the
    frame marks it transparent -- must be the background index (0). The
    reference dumps already encode this, but we assert it explicitly.
    """
    width, height = info["width"], info["height"]
    frames = info["frames"]
    for idx in range(1, len(dumps)):
        prev = frames[idx - 1]
        if prev["disp"] != 2:
            continue
        cur = frames[idx]
        dump = dumps[idx]
        bad = []
        for yy in range(prev["y"], prev["y"] + prev["h"]):
            for xx in range(prev["x"], prev["x"] + prev["w"]):
                in_cur = (cur["x"] <= xx < cur["x"] + cur["w"] and
                          cur["y"] <= yy < cur["y"] + cur["h"])
                painted = False
                if in_cur:
                    lx = xx - cur["x"]
                    ly = yy - cur["y"]
                    if not cur["transparent"]:
                        painted = True
                    else:
                        # transparent index keeps whatever was already on the
                        # canvas, so that cell is not repainted
                        painted = cur["rows"][ly][lx] != 3
                if not painted and dump[yy * width + xx] != BG:
                    bad.append((xx, yy, dump[yy * width + xx]))
        if bad:
            fail("%s frame %d: %d residual pixel(s) after disposal 2 "
                 "(first=%s)" % (name, idx, len(bad), bad[0]))
        # border pixels are never inside any frame, must always stay 0
        for xx in range(width):
            for yy in (0, height - 1):
                if dumps[idx][yy * width + xx] != BG:
                    fail("%s frame %d: border pixel (%d,%d) != 0"
                         % (name, idx, xx, yy))


def main():
    os.makedirs(BUILD, exist_ok=True)
    # 1. assets
    run([sys.executable, "-I", os.path.join(HERE, "make_gifs.py")])
    with open(os.path.join(ASSETS, "manifest.json")) as f:
        manifest = json.load(f)

    # 2. compile
    bin_path = os.path.join(BUILD, "test_disposal")
    cc = os.environ.get("CC", "gcc")
    run([cc, "-D__LINUX__", "-O2", "-Wall", "-Wextra", "-I" + SRC,
         os.path.join(HERE, "test_disposal.c"), "-o", bin_path, "-lm"])

    # 3+4. correctness on both LZW decoder paths
    for turbo in (0, 1):
        print("\n=== correctness, turbo=%d ===" % turbo)
        for name, info in manifest.items():
            if "expected" not in info:
                continue  # perf asset handled below
            gif = os.path.join(HERE, info["gif"])
            out = os.path.join(BUILD, "%s.t%d.bin" % (name, turbo))
            run([bin_path, gif, out, str(turbo)])
            with open(out, "rb") as f:
                got = f.read()
            with open(os.path.join(HERE, info["expected"]), "rb") as f:
                want = f.read()
            nframes = len(info["frames"])
            frame_bytes = info["width"] * info["height"]
            if len(got) != nframes * frame_bytes:
                fail("%s turbo=%d: got %d bytes, expected %d"
                     % (name, turbo, len(got), nframes * frame_bytes))
                continue
            if got != want:
                first = next(i for i in range(len(got)) if got[i] != want[i])
                fr, off = divmod(first, frame_bytes)
                fail("%s turbo=%d: pixel mismatch at frame %d offset %d "
                     "(got %d want %d)"
                     % (name, turbo, fr, off, got[first], want[first]))
                continue
            dumps = [want[i * frame_bytes:(i + 1) * frame_bytes]
                     for i in range(nframes)]
            residual_checks(name, info, dumps)
            print("  OK   %-14s %2d frames, %d pixels/frame byte-exact"
                  % (name, nframes, frame_bytes))

    # 5. performance, both paths; 3 loops for stable averages
    print("\n=== performance 640x480 (budget < 2.0000 ms/frame) ===")
    perf_gif = os.path.join(HERE, manifest["perf_640x480"]["gif"])
    for turbo in (0, 1):
        print("-- turbo=%d" % turbo)
        proc = subprocess.run([bin_path, perf_gif, "perf", str(turbo), "3"],
                              check=True, cwd=HERE, capture_output=True,
                              text=True)
        sys.stdout.write(proc.stdout)
        for line in proc.stdout.splitlines():
            if line.startswith("composite_ms/frame"):
                avg = float(line.split("avg=")[1].split()[0])
                mx = float(line.split("max=")[1])
                if avg >= 2.0:
                    fail("perf turbo=%d: avg %.4f ms >= 2 ms budget"
                         % (turbo, avg))
                if mx >= 2.0:
                    fail("perf turbo=%d: max %.4f ms >= 2 ms budget"
                         % (turbo, mx))

    print("\n=== summary ===")
    if failures:
        print("%d check(s) FAILED" % len(failures))
        return 1
    print("ALL CHECKS PASSED")
    print(" - 5 sequences x 2 decoder paths (normal + Turbo), "
          "incl. interlaced storage")
    print(" - byte-exact match vs hand-written reference compositor")
    print(" - zero residual pixels after disposal 2; border pixels stay 0")
    print(" - 640x480 compositing under the 2 ms/frame budget")
    return 0


if __name__ == "__main__":
    sys.exit(main())
