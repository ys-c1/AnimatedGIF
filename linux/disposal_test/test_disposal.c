//
// Correctness + performance harness for the in-library disposal compositor.
// Standard library only; includes the decoder directly like the existing
// linux/examples/gif_on_lcd demo.
//
// Usage:
//   test_disposal <file.gif> <out.bin|perf> <turbo 0|1> [perf loops]
//
#define _POSIX_C_SOURCE 199309L // clock_gettime under strict -std=cNN
#include "AnimatedGIF.h"
#include "gif.inl"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void GIFDrawEmpty(GIFDRAW *pDraw)
{
    (void)pDraw; // no user-side drawing: compositing happens inside the library
}

// The C entry point GIF_begin() leaves the 16-byte-aligned line buffer pointer
// unset (only the C++ AnimatedGIF::begin() initializes it). A C caller must
// point it at an aligned offset inside ucLineBuf, exactly as the C++ wrapper
// does, before decoding through the non-Turbo LZW path.
static void initLineBufAligned(GIFIMAGE *pGif)
{
    uint8_t *p = &pGif->ucLineBuf[0];
    uintptr_t u = (uintptr_t)p & 15;
    if (u)
        p += (16 - u);
    pGif->pLineBufAligned = p;
}

static uint8_t *read_file(const char *szPath, long *pSize)
{
    FILE *f = fopen(szPath, "rb");
    uint8_t *pData;
    long iSize;
    if (!f) {
        fprintf(stderr, "cannot open %s\n", szPath);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    iSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    pData = (uint8_t *)malloc(iSize);
    if (!pData) {
        fclose(f);
        return NULL;
    }
    if ((long)fread(pData, 1, iSize, f) != iSize) {
        free(pData);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *pSize = iSize;
    return pData;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

int main(int argc, char **argv)
{
    GIFIMAGE gif;
    uint8_t *pData, *pCanvas, *pTurbo = NULL;
    long iSize;
    int iTurbo, iLoops = 1, iFrame, iCanvasSize;
    const char *szOut;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <file.gif> <out.bin|perf> <turbo 0|1> [loops]\n",
                argv[0]);
        return 2;
    }
    iTurbo = atoi(argv[3]);
    if (argc >= 5)
        iLoops = atoi(argv[4]);
    szOut = argv[2];

    pData = read_file(argv[1], &iSize);
    if (!pData)
        return 2;

    GIF_begin(&gif, GIF_PALETTE_RGB565_LE);
    initLineBufAligned(&gif);
    if (!GIF_openRAM(&gif, pData, (int)iSize, GIFDrawEmpty)) {
        fprintf(stderr, "GIF_openRAM failed: %d\n", gif.iError);
        return 1;
    }
    iCanvasSize = gif.iCanvasWidth * gif.iCanvasHeight;
    if (iTurbo) {
        pTurbo = (uint8_t *)malloc(TURBO_BUFFER_SIZE + iCanvasSize);
        if (!pTurbo) {
            fprintf(stderr, "turbo alloc failed\n");
            return 1;
        }
        gif.pTurboBuffer = pTurbo;
    }
    if (GIF_beginComposite(&gif) != GIF_SUCCESS) {
        fprintf(stderr, "GIF_beginComposite failed\n");
        return 1;
    }
    pCanvas = GIF_getCompositeCanvas(&gif);
    if (!pCanvas) {
        fprintf(stderr, "GIF_getCompositeCanvas returned NULL\n");
        return 1;
    }
    // The fresh canvas must be entirely the logical-screen background color
    for (int i = 0; i < iCanvasSize; i++) {
        if (pCanvas[i] != gif.ucBackground) {
            fprintf(stderr, "initial canvas not background at %d\n", i);
            return 1;
        }
    }

    if (strcmp(szOut, "perf") == 0) {
        double tCompTotal = 0.0, tDecTotal = 0.0, tMin = 1e9, tMax = 0.0;
        int iComposites = 0, iDelay;
        for (int loop = 0; loop < iLoops; loop++) {
            GIF_reset(&gif);
            // GIF_playFrame returns 0 for the final frame (still decoded),
            // so composite on every successfully processed frame, not only
            // while the return value is non-zero.
            while (1) {
                int rc = GIF_playFrame(&gif, &iDelay, NULL);
                if (rc < 0) {
                    fprintf(stderr, "playFrame error %d\n", gif.iError);
                    return 1;
                }
                if (gif.iError == GIF_EMPTY_FRAME)
                    break; // trailing non-image data, no frame produced
                double t0 = now_ms();
                GIF_compositeFrame(&gif);
                double t1 = now_ms();
                double dt = t1 - t0;
                tCompTotal += dt;
                if (dt < tMin) tMin = dt;
                if (dt > tMax) tMax = dt;
                iComposites++;
                if (rc == 0)
                    break; // this was the last frame
            }
        }
        // decode-only context measurement (compositor stays on, hooks active)
        double dDec = 0.0;
        int iDecFrames = 0;
        GIF_reset(&gif);
        int iDelay2;
        while (1) {
            double a = now_ms();
            int rc = GIF_playFrame(&gif, &iDelay2, NULL);
            double b = now_ms();
            if (rc < 0 || gif.iError == GIF_EMPTY_FRAME)
                break;
            dDec += (b - a);
            iDecFrames++;
            GIF_compositeFrame(&gif);
            if (rc == 0)
                break;
        }
        (void)tDecTotal;
        printf("canvas=%dx%d turbo=%d composites=%d\n",
               gif.iCanvasWidth, gif.iCanvasHeight, iTurbo, iComposites);
        printf("composite_ms/frame avg=%.4f min=%.4f max=%.4f\n",
               tCompTotal / iComposites, tMin, tMax);
        printf("decode_ms/frame    avg=%.4f (n=%d, includes capture hooks)\n",
               dDec / iDecFrames, iDecFrames);
    } else {
        FILE *fOut = fopen(szOut, "wb");
        int iDelay;
        if (!fOut) {
            fprintf(stderr, "cannot write %s\n", szOut);
            return 1;
        }
        iFrame = 0;
        while (1) {
            int rc = GIF_playFrame(&gif, &iDelay, NULL);
            if (rc < 0) {
                fprintf(stderr, "playFrame error %d\n", gif.iError);
                fclose(fOut);
                return 1;
            }
            if (gif.iError == GIF_EMPTY_FRAME)
                break;
            GIF_compositeFrame(&gif);
            fwrite(pCanvas, 1, iCanvasSize, fOut);
            fprintf(stderr,
                    "frame %d: x=%d y=%d w=%d h=%d disp=%d trans=%d\n",
                    iFrame, gif.iX, gif.iY, gif.iWidth, gif.iHeight,
                    gif.ucDisposalMethod, gif.ucGIFBits & 1);
            iFrame++;
            if (rc == 0)
                break; // last frame
        }
        fclose(fOut);
        printf("wrote %d frame(s) x %d bytes to %s (turbo=%d)\n",
               iFrame, iCanvasSize, szOut, iTurbo);
    }

    GIF_endComposite(&gif);
    if (gif.pPixels != NULL) {
        fprintf(stderr, "compositor state not released\n");
        return 1;
    }
    free(pTurbo);
    free(pData);
    return 0;
}
