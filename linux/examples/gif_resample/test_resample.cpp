// test_resample.cpp - acceptance tests for GIFResample.h on top of AnimatedGIF
//
// For each test group (down 2x, down 4x, up 2x, non-integer ratio):
//   1. decode the sequence frame by frame (COOKED RGB565 framebuffer)
//   2. resample each composited canvas to the target size
//      - "streaming": one persistent scratch buffer reused across frames
//      - "independent": a fresh scratch buffer for every frame
//      -> outputs must be pixel-identical (frame-to-frame consistency)
//   3. verify output dimensions
//   4. count distinct colors of source vs downscaled frames
//      (GIF_RESAMPLE_NEAREST must never increase the color count)
//   5. measure per-frame decode and resample time
// The 640x480 sample additionally checks resample time < 5 ms per frame.
//
// Usage: test_resample <dir_with_gifs>
#include <AnimatedGIF.h>
#include "GIFResample.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <chrono>
#include <vector>

static double now_ms()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct FrameSet {
    int w = 0, h = 0;
    std::vector<std::vector<uint16_t>> frames; // cooked RGB565 canvases
    std::vector<double> decode_ms;
};

// Decode a whole GIF file into composited RGB565 canvases.
static bool decode_all(const char *path, FrameSet &out)
{
    AnimatedGIF gif;
    gif.begin(GIF_PALETTE_RGB565_LE); // begin() resets state; call before open()
    if (!gif.open(path, nullptr)) {
        printf("  ERROR: cannot open %s\n", path);
        return false;
    }
    gif.setDrawType(GIF_DRAW_COOKED);
    int w = gif.getCanvasWidth(), h = gif.getCanvasHeight();
    // 8-bpp canvas + cooked RGB565 canvas (decoder layout, no draw callback)
    uint8_t *fb = (uint8_t *)calloc(1, (size_t)w * h * 3);
    gif.setFrameBuf(fb);
    out.w = w; out.h = h;
    for (;;) {
        int delay = 0;
        double t0 = now_ms();
        int rc = gif.playFrame(false, &delay);
        double t1 = now_ms();
        if (rc < 0) {
            printf("  ERROR: decode failed, last error %d\n", gif.getLastError());
            free(fb);
            gif.close();
            return false;
        }
        if (gif.getLastError() == GIF_SUCCESS) { // a frame was rendered
            out.decode_ms.push_back(t1 - t0);
            const uint16_t *cooked = (const uint16_t *)(fb + (size_t)w * h);
            out.frames.emplace_back(cooked, cooked + (size_t)w * h);
        }
        if (rc == 0) break; // no more frames
    }
    free(fb);
    gif.close();
    return !out.frames.empty();
}

static int count_colors(const uint16_t *p, size_t n)
{
    uint8_t seen[65536 / 8] = {0};
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t v = p[i];
        if (!(seen[v >> 3] & (1 << (v & 7)))) {
            seen[v >> 3] |= (1 << (v & 7));
            count++;
        }
    }
    return count;
}

struct Group {
    const char *file;
    int dw, dh;
    const char *name;
    bool check_perf;
};

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_resample <dir_with_gifs>\n");
        return 1;
    }
    const char *dir = argv[1];
    Group groups[] = {
        { "seq_a_320x240.gif", 160, 120, "downscale 2x   (320x240 -> 160x120)", false },
        { "seq_a_320x240.gif",  80,  60, "downscale 4x   (320x240 ->  80x60)",  false },
        { "seq_b_160x120.gif", 320, 240, "upscale   2x   (160x120 -> 320x240)", false },
        { "seq_c_300x200.gif", 176, 132, "non-integer    (300x200 -> 176x132)", false },
        { "perf_640x480.gif",  320, 240, "perf down 2x   (640x480 ->  320x240)", true },
        { "perf_640x480.gif",  160, 120, "perf down 4x   (640x480 ->  160x120)", true },
        { "perf_640x480.gif", 1280, 960, "perf up   2x   (640x480 -> 1280x960)", true },
        { "perf_640x480.gif",  400, 300, "perf non-int   (640x480 ->  400x300)", true },
    };
    int failures = 0;

    for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
        Group &G = groups[g];
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir, G.file);
        FrameSet fs;
        if (!decode_all(path, fs)) { failures++; continue; }

        printf("[%s]  frames=%d  src=%dx%d  dst=%dx%d\n",
               G.name, (int)fs.frames.size(), fs.w, fs.h, G.dw, G.dh);

        // sanity: the animation must actually change between frames,
        // otherwise the frame-to-frame consistency check would be vacuous
        bool animates = false;
        for (size_t f = 1; f < fs.frames.size(); f++)
            if (fs.frames[f] != fs.frames[0]) animates = true;
        if (!animates) { printf("  FAIL: all decoded frames identical\n"); failures++; }

        for (int mode = 0; mode <= 1; mode++) {
            const char *mname = mode == GIF_RESAMPLE_AUTO ? "AUTO(area/nn)" : "NEAREST";
            int scratch_size = GIFResampleScratchSize(fs.w, fs.h, G.dw, G.dh, mode);
            uint8_t *scratch = (uint8_t *)malloc(scratch_size > 0 ? scratch_size : 1);
            std::vector<uint16_t> streaming((size_t)G.dw * G.dh);
            double rs_total = 0, rs_max = 0;
            bool consistent = true;
            int max_src_colors = 0, max_out_colors = 0;

            for (size_t f = 0; f < fs.frames.size(); f++) {
                const uint16_t *src = fs.frames[f].data();
                // streaming: persistent scratch reused for every frame
                double t0 = now_ms();
                GIFResample565(src, fs.w, fs.h, streaming.data(), G.dw, G.dh, mode, scratch);
                double t1 = now_ms();
                rs_total += t1 - t0;
                if (t1 - t0 > rs_max) rs_max = t1 - t0;
                // independent: fresh scratch for this frame only
                uint8_t *scratch2 = (uint8_t *)malloc(scratch_size > 0 ? scratch_size : 1);
                std::vector<uint16_t> indep((size_t)G.dw * G.dh);
                GIFResample565(src, fs.w, fs.h, indep.data(), G.dw, G.dh, mode, scratch2);
                free(scratch2);
                if (memcmp(streaming.data(), indep.data(), (size_t)G.dw * G.dh * 2) != 0)
                    consistent = false;
                int sc = count_colors(src, (size_t)fs.w * fs.h);
                int oc = count_colors(streaming.data(), (size_t)G.dw * G.dh);
                if (sc > max_src_colors) max_src_colors = sc;
                if (oc > max_out_colors) max_out_colors = oc;
            }
            free(scratch);

            double dec_total = 0, dec_max = 0;
            for (double d : fs.decode_ms) { dec_total += d; if (d > dec_max) dec_max = d; }
            size_t nf = fs.frames.size();
            printf("  mode=%-14s out=%dx%d  frames=%d  pixel-identical=%s\n",
                   mname, G.dw, G.dh, (int)nf, consistent ? "YES" : "NO");
            printf("    colors: src(max/frame)=%d  resampled(max/frame)=%d%s\n",
                   max_src_colors, max_out_colors,
                   (mode == GIF_RESAMPLE_NEAREST && max_out_colors > max_src_colors)
                       ? "  <-- FAIL: palette pollution" : "");
            printf("    decode/frame:   avg=%.3f ms  max=%.3f ms\n",
                   dec_total / nf, dec_max);
            printf("    resample/frame: avg=%.3f ms  max=%.3f ms  (scratch=%d bytes)\n",
                   rs_total / nf, rs_max, scratch_size);

            if (!consistent) { printf("    FAIL: streaming != independent\n"); failures++; }
            if (mode == GIF_RESAMPLE_NEAREST && max_out_colors > max_src_colors) failures++;
            if (G.check_perf && rs_max >= 5.0) {
                printf("    FAIL: resample max %.3f ms >= 5 ms on 640x480 source\n", rs_max);
                failures++;
            }
        }
    }

    printf("\n%s\n", failures == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}
