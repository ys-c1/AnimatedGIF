# In-library GIF disposal compositor — tests

## What this verifies

`AnimatedGIF` used to expose only `GIFDRAW.ucDisposalMethod`; actually applying
the GIF89a disposal rules was left to every caller. The library now contains an
opt-in compositor (`src/gif.inl`, the only changed library file) that maintains
the full canvas and hands back the already-rendered frame:

| New entry point (C)              | Purpose                                            |
|----------------------------------|----------------------------------------------------|
| `GIF_beginComposite(GIFIMAGE*)`  | allocate the internal canvas + restore snapshot    |
| `GIF_compositeFrame(GIFIMAGE*)`  | apply one decoded frame with its disposal method   |
| `GIF_getCompositeCanvas(GIFIMAGE*)` | pointer to the rendered canvas (1 index/pixel)  |
| `GIF_endComposite(GIFIMAGE*)`    | free compositor buffers (the GIF stays open)       |

Supported disposal methods: 0/1 keep, 2 restore-to-background, 3
restore-to-previous. Nothing existing changes behavior — the compositor hooks
are no-ops until `GIF_beginComposite()` succeeds.

## Files

- `make_gifs.py` — stdlib-only GIF89a writer (own LZW encoder) plus the
  hand-written reference compositor used as the correctness oracle.
- `test_disposal.c` — C harness: decodes each sequence through the library and
  dumps composited frames, or benchmarks 640x480. Runs both LZW paths
  (normal and Turbo).
- `run_tests.py` — one command: regenerate assets, build, compare every frame
  byte-for-byte with the reference, assert zero residuals, benchmark.

## Run

```
python3 -I run_tests.py
```

No third-party packages and no network access are required by the test itself
(PIL is used only in ad-hoc manual checks, never by the runner).

## Sequences

- `seq_nodisp`    — overlapping frames, disposal 1 (do not dispose)
- `seq_bg`        — disposal 2 (restore to background), incl. transparency
- `seq_prev`      — disposal 3 (restore to previous)
- `seq_mixed`     — moving rectangles mixing methods 1/2/3 and transparency
- `seq_interlace` — interlaced storage exercising the row de-interleave hook
- `perf_640x480`  — full-canvas frames for the < 2 ms/frame budget check
