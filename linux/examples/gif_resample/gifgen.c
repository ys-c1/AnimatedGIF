// gifgen.c - generate small animated GIF89a test samples (no dependencies).
//
// Writes uncompressed-LZW GIFs: 256-color global palette, full-canvas
// frames, disposal method 1, no transparency. Content is procedural and
// animated (moving block + high-frequency detail) so that resampling
// behavior differs meaningfully between frames and between filter modes.
//
// Usage: gifgen <output_dir>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void put16(FILE *f, int v)
{
    fputc(v & 0xff, f);
    fputc((v >> 8) & 0xff, f);
}

// 9-bit fixed-size LZW code stream (clear every 250 literals so the
// decoder's dictionary never grows past the initial code size)
typedef struct {
    FILE *f;
    uint8_t buf[255];
    int len;       // bytes used in current sub-block
    uint32_t bits; // bit accumulator
    int nbits;     // bits in accumulator
    int lit;       // literals since last clear
} LZW;

static void lzw_code(LZW *p, int code)
{
    p->bits |= (uint32_t)code << p->nbits;
    p->nbits += 9;
    while (p->nbits >= 8) {
        if (p->len == 255) { // flush a full sub-block
            fputc(255, p->f);
            fwrite(p->buf, 1, 255, p->f);
            p->len = 0;
        }
        p->buf[p->len++] = (uint8_t)(p->bits & 0xff);
        p->bits >>= 8;
        p->nbits -= 8;
    }
}

static void lzw_flush(LZW *p)
{
    if (p->nbits > 0) {
        if (p->len == 255) {
            fputc(255, p->f);
            fwrite(p->buf, 1, 255, p->f);
            p->len = 0;
        }
        p->buf[p->len++] = (uint8_t)(p->bits & 0xff);
        p->bits = 0;
        p->nbits = 0;
    }
    if (p->len) {
        fputc(p->len, p->f);
        fwrite(p->buf, 1, p->len, p->f);
        p->len = 0;
    }
    fputc(0, p->f); // block terminator
}

// animated test pattern: gradient + moving block + 1-pixel checker detail
static uint8_t pixel_at(int x, int y, int f, int w, int h, int seed)
{
    int idx = (x * 3 + y * 5 + f * 17 + seed * 29) & 0x7f; // smooth gradient, 128 colors
    idx += 32;
    if (((x ^ y) & 1) && (x % 3 == 0)) // high frequency detail
        idx = 16 + ((x + y + f * 9) & 0x3f);
    // moving block (position depends on frame)
    int bx = (f * 23 + seed * 7) % (w > 40 ? w - 40 : 1);
    int by = (f * 11 + seed * 3) % (h > 40 ? h - 40 : 1);
    if (x >= bx && x < bx + 40 && y >= by && y < by + 40)
        idx = 200 + ((x + y) & 0x37);
    return (uint8_t)idx;
}

static int write_gif(const char *path, int w, int h, int nframes, int seed)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("GIF89a", 1, 6, f);
    put16(f, w); put16(f, h);
    fputc(0xf7, f); // GCT present, 8-bit color resolution, 256 entries
    fputc(0, f);    // background color index
    fputc(0, f);    // aspect
    for (int i = 0; i < 256; i++) { // fixed pseudo-random palette
        fputc((i * 73 + 41) & 0xff, f);
        fputc((i * 151 + 87) & 0xff, f);
        fputc((i * 199 + 23) & 0xff, f);
    }
    // NETSCAPE looping extension (loop forever)
    fwrite("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 1, 19, f);
    for (int fr = 0; fr < nframes; fr++) {
        // graphic control extension: disposal 1 (do not dispose), no transparency
        fwrite("\x21\xf9\x04\x04", 1, 4, f);
        put16(f, 10); // delay 100ms
        fputc(0, f);  // transparent index (unused)
        fputc(0, f);
        // image descriptor: full canvas, no local palette
        fputc(0x2c, f);
        put16(f, 0); put16(f, 0); put16(f, w); put16(f, h);
        fputc(0, f);
        fputc(8, f); // LZW minimum code size
        LZW lz = { f, {0}, 0, 0, 0, 0 };
        lzw_code(&lz, 256); // clear
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (lz.lit == 250) {
                    lzw_code(&lz, 256); // clear before dictionary grows
                    lz.lit = 0;
                }
                lzw_code(&lz, pixel_at(x, y, fr, w, h, seed));
                lz.lit++;
            }
        }
        lzw_code(&lz, 257); // end of image
        lzw_flush(&lz);
    }
    fputc(0x3b, f); // trailer
    long size = ftell(f);
    fclose(f);
    return (int)size;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: gifgen <output_dir>\n");
        return 1;
    }
    struct { const char *name; int w, h, frames, seed; } jobs[] = {
        { "seq_a_320x240.gif", 320, 240, 8, 1 }, // -> down 2x (160x120), down 4x (80x60)
        { "seq_b_160x120.gif", 160, 120, 8, 2 }, // -> up 2x (320x240)
        { "seq_c_300x200.gif", 300, 200, 8, 3 }, // -> non-integer (176x132)
        { "perf_640x480.gif",  640, 480, 4, 4 }, // -> performance measurement
    };
    for (int i = 0; i < 4; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", argv[1], jobs[i].name);
        int size = write_gif(path, jobs[i].w, jobs[i].h, jobs[i].frames, jobs[i].seed);
        if (size < 0) {
            fprintf(stderr, "failed to write %s\n", path);
            return 1;
        }
        printf("wrote %s (%dx%d, %d frames, %d bytes)\n",
               path, jobs[i].w, jobs[i].h, jobs[i].frames, size);
    }
    return 0;
}
