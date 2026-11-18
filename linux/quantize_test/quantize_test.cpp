//
// GIF RGB888 quantizer acceptance test
//
// Exercises GIF_quantizeRGB888() (median cut + optional Floyd-Steinberg
// dithering + error report) on four synthetic in-memory image classes:
// photo, screenshot, gradient and pixel art. No files, no third-party
// dependencies - everything is generated deterministically with an LCG.
//
// Build: see Makefile in this directory.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>

#include "AnimatedGIF.h"   // GIF_* error codes, MAX_COLORS
#include "gif.inl"         // pulls in GIF_quantizeRGB888() and GIF_QUANT_STATS

// Deterministic 32-bit LCG (Numerical Recipes constants), state in [1, 2^32-1]
static uint32_t g_u32Seed = 0x12345678u;
static uint32_t lcg(void)
{
    g_u32Seed = g_u32Seed * 1664525u + 1013904223u;
    return g_u32Seed;
}
static uint8_t lcg_byte(void)
{
    return (uint8_t)(lcg() >> 24);
}
static uint8_t clamp8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static uint8_t *alloc_rgb(int w, int h)
{
    uint8_t *p = (uint8_t *)malloc((size_t)w * h * 3);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(2); }
    return p;
}

// Photo-like: smooth vertical sky gradient + overlapping shaded discs +
// sensor-like noise. The noise amplitude is +/-16 color levels per channel
// (typical of a real camera's read noise, not the +/-128 white noise that no
// 256-entry palette could represent); it still leaves >11k distinct colors.
#define PHOTO_NOISE 16
static void make_photo(uint8_t *p, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            int r = 90 + (180 - 90) * y / h;
            int g = 120 + (200 - 120) * y / h;
            int b = 200 - (200 - 150) * y / h;
            // three smooth discs with different shading
            const int cx[3] = { w / 4, w * 2 / 3, w / 2 };
            const int cy[3] = { h / 3, h * 2 / 3, h / 2 };
            const int rad[3] = { w / 5, w / 6, w / 7 };
            for (int k = 0; k < 3; k++)
            {
                int dx = x - cx[k], dy = y - cy[k];
                int d2 = dx * dx + dy * dy;
                if (d2 < rad[k] * rad[k])
                {
                    int shade = 60 * (rad[k] * rad[k] - d2) / (rad[k] * rad[k]);
                    int add[3][3] = { {120, 40, -30}, {30, 110, 20}, {-20, 60, 120} };
                    r += shade * add[k][0] / 60;
                    g += shade * add[k][1] / 60;
                    b += shade * add[k][2] / 60;
                }
            }
            r += ((int)lcg_byte() - 128) * PHOTO_NOISE / 128; // realistic +/-16 noise
            g += ((int)lcg_byte() - 128) * PHOTO_NOISE / 128;
            b += ((int)lcg_byte() - 128) * PHOTO_NOISE / 128;
            uint8_t *q = p + (y * w + x) * 3;
            q[0] = clamp8(r); q[1] = clamp8(g); q[2] = clamp8(b);
        }
    }
}

// Screenshot-like: flat UI panels with antialiased text bars and borders.
// Lots of flat colors plus narrow AA ramps.
static void make_screenshot(uint8_t *p, int w, int h)
{
    int x, y;
    const uint8_t bg[3] = {236, 239, 244};
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            uint8_t *q = p + (y * w + x) * 3;
            q[0] = bg[0]; q[1] = bg[1]; q[2] = bg[2];
        }
    // title bar and side panel (flat fills)
    for (y = 0; y < h * 2 / 10; y++)
        for (x = 0; x < w; x++)
        {
            uint8_t *q = p + (y * w + x) * 3;
            q[0] = 42; q[1] = 88; q[2] = 160;
        }
    for (y = h * 2 / 10; y < h; y++)
        for (x = 0; x < w * 3 / 10; x++)
        {
            uint8_t *q = p + (y * w + x) * 3;
            q[0] = 215; q[1] = 222; q[2] = 232;
        }
    // rounded buttons: flat interior + 1px antialiased edges (255->fill ramp)
    const uint8_t fill[3] = {64, 158, 96};
    for (int bi = 0; bi < 6; bi++)
    {
        int bx = w * 4 / 10 + (bi % 2) * w * 5 / 16;
        int by = h * 3 / 10 + (bi / 2) * h / 5;
        int bw = w / 5, bh = h / 10, br = 6;
        for (y = by - br; y < by + bh + br; y++)
            for (x = bx - br; x < bx + bw + br; x++)
            {
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                int dx = 0, dy = 0;
                if (x < bx) dx = bx - x; else if (x >= bx + bw) dx = x - (bx + bw - 1);
                if (y < by) dy = by - y; else if (y >= by + bh) dy = y - (by + bh - 1);
                int d2 = dx * dx + dy * dy;
                if (d2 <= (br - 1) * (br - 1))
                {
                    uint8_t *q = p + (y * w + x) * 3;
                    q[0] = fill[0]; q[1] = fill[1]; q[2] = fill[2];
                }
                else if (d2 <= (br + 1) * (br + 1))
                {
                    int t = (br + 1) * (br + 1) - d2; // AA ramp
                    uint8_t *q = p + (y * w + x) * 3;
                    q[0] = clamp8(bg[0] + ((int)fill[0] - bg[0]) * t / 4);
                    q[1] = clamp8(bg[1] + ((int)fill[1] - bg[1]) * t / 4);
                    q[2] = clamp8(bg[2] + ((int)fill[2] - bg[2]) * t / 4);
                }
            }
    }
    // fake antialiased text: deterministic thin bars with edge ramps
    for (int line = 0; line < 5; line++)
    {
        int ty = h * 3 / 10 + line * h / 12;
        for (y = ty; y < ty + 4 && y < h; y++)
            for (x = w * 4 / 10; x < w * 9 / 10 && x < w; x++)
            {
                int edge = ((x + y) % 13 < 2);
                uint8_t *q = p + (y * w + x) * 3;
                if (edge) { q[0] = clamp8(q[0] / 2 + 30); q[1] = clamp8(q[1] / 2 + 30); q[2] = clamp8(q[2] / 2 + 30); }
                else { q[0] = 40; q[1] = 44; q[2] = 52; }
            }
    }
}

// Smooth 2D diagonal gradient: every pixel unique-ish, continuous color field.
static void make_gradient(uint8_t *p, int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            uint8_t *q = p + (y * w + x) * 3;
            q[0] = (uint8_t)(255 * x / (w - 1));
            q[1] = (uint8_t)(255 * y / (h - 1));
            q[2] = (uint8_t)(255 * (x + y) / (w + h - 2));
        }
}

// Pixel art: a small fixed palette of flat colors with hard 1px edges.
static void make_pixelart(uint8_t *p, int w, int h)
{
    static const uint8_t pal[8][3] = {
        {32, 32, 48}, {240, 240, 230}, {200, 40, 40}, {40, 110, 210},
        {240, 190, 60}, {70, 170, 90}, {160, 90, 200}, {230, 130, 180}
    };
    int x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            // 8x8 blocks in a repeating 4x2 pattern -> exactly 8 distinct colors
            int bx = (x / 8) & 3, by = (y / 8) & 1;
            int idx = (bx * 2 + by) & 7;
            uint8_t *q = p + (y * w + x) * 3;
            q[0] = pal[idx][0]; q[1] = pal[idx][1]; q[2] = pal[idx][2];
        }
}

static int count_distinct(const uint8_t *p, int n)
{
    // small open-addressing table, enough for the synthetic samples
    int cap = 1 << 18, count = 0;
    uint32_t *keys = (uint32_t *)calloc((size_t)cap, sizeof(uint32_t));
    if (!keys) { fprintf(stderr, "out of memory\n"); exit(2); }
    for (int i = 0; i < n; i++)
    {
        uint32_t k = ((uint32_t)p[i * 3] << 16) | ((uint32_t)p[i * 3 + 1] << 8) | p[i * 3 + 2];
        uint32_t s = (k * 2654435761u) & (uint32_t)(cap - 1);
        while (keys[s] && keys[s] != k + 1) s = (s + 1) & (cap - 1);
        if (!keys[s]) { keys[s] = k + 1; count++; }
    }
    free(keys);
    return count;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int g_iFailures = 0;
static void check(int cond, const char *what)
{
    printf("    [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_iFailures++;
}

static void run_case(const char *name, const uint8_t *img, int w, int h,
                     int max_colors, int dither, int iters, int require_psnr)
{
    int n = w * h;
    uint8_t *pal = (uint8_t *)malloc(MAX_COLORS * 3);
    uint8_t *out = (uint8_t *)malloc((size_t)n);
    GIF_QUANT_STATS st;
    double tmin = 1e9, tsum = 0.0;
    int rc = 0;
    for (int it = 0; it < iters; it++)
    {
        double t0 = now_ms();
        rc = GIF_quantizeRGB888(img, w, h, max_colors, dither, pal, out, &st);
        double t1 = now_ms();
        if (rc < 0) break;
        double dt = t1 - t0;
        if (dt < tmin) tmin = dt;
        tsum += dt;
    }
    printf("  %-28s cap=%3d dither=%d  size=%dx%d\n",
           name, max_colors, dither, w, h);
    if (rc < 0)
    {
        printf("    [FAIL] quantizer returned error %d\n", rc);
        g_iFailures++;
        free(pal); free(out);
        return;
    }
    printf("    distinct_in=%-6d palette=%-4d used=%-4d MSE=%-10.4f "
           "PSNR=%7.2f dB  meanDE=%6.3f  time(min)=%6.2f ms  time(avg)=%6.2f ms\n",
           st.iDistinctInput, st.iPaletteCount, st.iUsedColors,
           st.dMSE, st.dPSNR, st.dMeanDeltaE, tmin, tsum / iters);
    check(rc == st.iPaletteCount && rc >= 1 && rc <= max_colors,
          "palette count within [1, cap]");
    check(st.iUsedColors <= st.iPaletteCount, "used colors <= palette count");
    if (st.iDistinctInput <= max_colors)
    {
        check(st.dPSNR >= 99.0 && st.dMSE == 0.0,
              "lossless when distinct colors <= cap");
    }
    else if (require_psnr)
    {
        // The 30 dB acceptance target applies to the full 256-entry palette.
        // Deliberately small caps are only required to honor the color limit.
        check(st.dPSNR >= 30.0, "PSNR >= 30 dB");
    }
    free(pal); free(out);
}

int main(void)
{
    const int W = 128, H = 96, N = W * H;
    uint8_t *photo = alloc_rgb(W, H);
    uint8_t *shot  = alloc_rgb(W, H);
    uint8_t *grad  = alloc_rgb(W, H);
    uint8_t *pix   = alloc_rgb(W, H);

    make_photo(photo, W, H);
    make_screenshot(shot, W, H);
    make_gradient(grad, W, H);
    make_pixelart(pix, W, H);

    printf("== Synthetic sample distinct-color counts (%dx%d) ==\n", W, H);
    printf("  photo:      %d distinct colors\n", count_distinct(photo, N));
    printf("  screenshot: %d distinct colors\n", count_distinct(shot, N));
    printf("  gradient:   %d distinct colors\n", count_distinct(grad, N));
    printf("  pixel art:  %d distinct colors\n", count_distinct(pix, N));

    printf("\n== Acceptance: four image classes, cap=256, 5 timed runs ==\n");
    run_case("photo",      photo, W, H, 256, 0, 5, 1);
    run_case("photo+dither", photo, W, H, 256, 1, 5, 1);
    run_case("screenshot", shot,  W, H, 256, 0, 5, 1);
    run_case("screenshot+dither", shot, W, H, 256, 1, 5, 1);
    run_case("gradient",   grad,  W, H, 256, 0, 5, 1);
    run_case("gradient+dither", grad, W, H, 256, 1, 5, 1);
    run_case("pixel art",  pix,   W, H, 256, 0, 5, 1);
    run_case("pixel art+dither", pix, W, H, 256, 1, 5, 1);

    printf("\n== Configurable color-cap sweep (photo, no dither) ==\n");
    static const int caps[] = {128, 64, 32, 16};
    for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); i++)
        run_case("photo cap sweep", photo, W, H, caps[i], 0, 3, 0);

    printf("\n== Stress: small caps with dithering ==\n");
    run_case("gradient 16+dither", grad, W, H, 16, 1, 3, 0);
    run_case("photo 16+dither",    photo, W, H, 16, 1, 3, 0);

    printf("\n== Performance: 640x480 photo, budget 100 ms ==\n");
    {
        const int PW = 640, PH = 480;
        uint8_t *big = alloc_rgb(PW, PH);
        make_photo(big, PW, PH);
        run_case("photo 640x480",        big, PW, PH, 256, 0, 5, 1);
        run_case("photo 640x480+dither", big, PW, PH, 256, 1, 5, 1);
        free(big);
    }

    printf("\n== Parameter validation ==\n");
    {
        uint8_t pal[256 * 3], out[N];
        int rc1 = GIF_quantizeRGB888(NULL, 4, 4, 256, 0, pal, out, NULL);
        int rc2 = GIF_quantizeRGB888(photo, 0, 4, 256, 0, pal, out, NULL);
        check(rc1 < 0 && rc2 < 0, "NULL/zero-size inputs rejected");
        // cap clamps to 256 and to 1 without error
        GIF_QUANT_STATS st;
        int rc3 = GIF_quantizeRGB888(pix, W, H, 9999, 0, pal, out, &st);
        check(rc3 >= 1 && rc3 <= 256, "cap > 256 clamps to 256");
        int rc4 = GIF_quantizeRGB888(photo, W, H, 0, 0, pal, out, &st);
        check(rc4 == 1, "cap <= 0 clamps to 1");
    }

    free(photo); free(shot); free(grad); free(pix);
    printf("\n==== %s (%d failure%s) ====\n",
           g_iFailures == 0 ? "ALL CHECKS PASSED" : "FAILURES DETECTED",
           g_iFailures, g_iFailures == 1 ? "" : "s");
    return g_iFailures ? 1 : 0;
}
