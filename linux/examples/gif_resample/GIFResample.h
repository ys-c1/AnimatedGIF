// GIFResample.h - resample a decoded (cooked) GIF canvas to a target size
//
// Built on top of the AnimatedGIF decoder: decode each frame as usual
// (GIF_DRAW_COOKED + framebuffer, GIF_PALETTE_RGB565_LE), then call
// GIFResample565() on the cooked canvas to obtain a frame of the exact
// target dimensions.
//
// Resampling rules:
//   - shrinking an axis  -> area-average (box filter, exact fractional
//                           weights in 16.16 fixed point)
//   - enlarging an axis  -> nearest neighbor
//   - GIF_RESAMPLE_NEAREST forces nearest on both axes. This mode never
//     creates colors that were not present in the source frame, so the
//     palette/color set cannot be polluted by resampling.
//
// Frame-to-frame consistency: the resampler keeps no state between calls;
// the output for a frame is a pure function of that frame's pixels, the
// source/target sizes and the mode. Reusing one scratch buffer across a
// whole sequence therefore gives pixel-identical results to resampling
// every frame with a fresh, independent call.
//
// The implementation is pure C (no external dependencies) and does no
// dynamic allocation: the caller supplies the scratch buffer.
//
#ifndef __GIF_RESAMPLE__
#define __GIF_RESAMPLE__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    GIF_RESAMPLE_AUTO = 0,   // area-average when shrinking, nearest when enlarging
    GIF_RESAMPLE_NEAREST = 1 // nearest everywhere (never adds new colors)
};

// Number of scratch bytes GIFResample565() needs for a given conversion.
// Returns 0 when no scratch is needed (pure nearest path).
static int GIFResampleScratchSize(int iSrcW, int iSrcH, int iDstW, int iDstH, int iMode)
{
    if (iMode == GIF_RESAMPLE_NEAREST)
        return 0;
    if (iDstW >= iSrcW && iDstH >= iSrcH)
        return 0; // pure enlargement -> single-pass nearest
    // two-pass path: intermediate is iDstW x iSrcH pixels, 3 x uint16 per pixel
    return iDstW * iSrcH * 3 * (int)sizeof(uint16_t);
}

// nearest source index for destination index i (center aligned)
static int GIFResampleNearest(int i, int iSrcLen, int iDstLen)
{
    int j = (int)(((int64_t)(2 * i + 1) * iSrcLen) / (2 * (int64_t)iDstLen));
    if (j >= iSrcLen) j = iSrcLen - 1;
    return j;
}

// unpack a RGB565 pixel into its 3 channels at q8.8 fixed point scale
static void GIFResampleUnpack(uint16_t u16, uint16_t *pQ)
{
    pQ[0] = (uint16_t)(((u16 >> 11) & 0x1f) << 8); // red   (5 bits)
    pQ[1] = (uint16_t)(((u16 >> 5) & 0x3f) << 8);  // green (6 bits)
    pQ[2] = (uint16_t)((u16 & 0x1f) << 8);         // blue  (5 bits)
}

// Horizontal pass: iSrcW RGB565 pixels -> iDstW pixels of 3 x q8.8 channels.
// Area-average when shrinking (AUTO mode), nearest otherwise.
static void GIFResamplePassH(const uint16_t *pSrc, int iSrcW, uint16_t *pDst, int iDstW, int iMode)
{
    if (iMode == GIF_RESAMPLE_NEAREST || iDstW >= iSrcW) {
        for (int i = 0; i < iDstW; i++) {
            int x = GIFResampleNearest(i, iSrcW, iDstW);
            GIFResampleUnpack(pSrc[x], &pDst[i * 3]);
        }
        return;
    }
    // area average, 16.16 fixed point source rectangle per dest pixel
    for (int i = 0; i < iDstW; i++) {
        uint32_t fx0 = (uint32_t)(((int64_t)i * iSrcW << 16) / iDstW);
        uint32_t fx1 = (uint32_t)(((int64_t)(i + 1) * iSrcW << 16) / iDstW);
        int j0 = (int)(fx0 >> 16);
        int j1 = (int)((fx1 - 1) >> 16); // last source pixel touched
        uint32_t r = 0, g = 0, b = 0;
        for (int j = j0; j <= j1; j++) {
            uint32_t lo = (uint32_t)j << 16, hi = (uint32_t)(j + 1) << 16;
            uint32_t w = ((fx1 < hi ? fx1 : hi) - (fx0 > lo ? fx0 : lo)); // overlap weight
            uint16_t q[3];
            GIFResampleUnpack(pSrc[j], q);
            r += q[0] * w; g += q[1] * w; b += q[2] * w;
        }
        // q = v<<8 and sum of weights == 65536, so acc = avg << 24;
        // store the intermediate as avg << 8 (q8.8, rounded)
        pDst[i * 3 + 0] = (uint16_t)((r + 32768) >> 16);
        pDst[i * 3 + 1] = (uint16_t)((g + 32768) >> 16);
        pDst[i * 3 + 2] = (uint16_t)((b + 32768) >> 16);
    }
}

// Vertical pass: iSrcH rows of q8.8 pixels -> iDstH rows of RGB565.
static void GIFResamplePassV(const uint16_t *pSrc, int iWidth, int iSrcH, uint16_t *pDst, int iDstH, int iMode)
{
    if (iMode == GIF_RESAMPLE_NEAREST || iDstH >= iSrcH) {
        for (int i = 0; i < iDstH; i++) {
            int y = GIFResampleNearest(i, iSrcH, iDstH);
            const uint16_t *s = &pSrc[y * iWidth * 3];
            uint16_t *d = &pDst[i * iWidth];
            for (int x = 0; x < iWidth; x++) {
                uint16_t r = (uint16_t)((s[x * 3 + 0] + 128) >> 8);
                uint16_t g = (uint16_t)((s[x * 3 + 1] + 128) >> 8);
                uint16_t b = (uint16_t)((s[x * 3 + 2] + 128) >> 8);
                d[x] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
        return;
    }
    // area average vertically
    for (int i = 0; i < iDstH; i++) {
        uint32_t fx0 = (uint32_t)(((int64_t)i * iSrcH << 16) / iDstH);
        uint32_t fx1 = (uint32_t)(((int64_t)(i + 1) * iSrcH << 16) / iDstH);
        int j0 = (int)(fx0 >> 16);
        int j1 = (int)((fx1 - 1) >> 16);
        uint16_t *d = &pDst[i * iWidth];
        for (int x = 0; x < iWidth; x++) {
            uint64_t r = 0, g = 0, b = 0;
            for (int j = j0; j <= j1; j++) {
                uint32_t lo = (uint32_t)j << 16, hi = (uint32_t)(j + 1) << 16;
                uint32_t w = ((fx1 < hi ? fx1 : hi) - (fx0 > lo ? fx0 : lo));
                const uint16_t *q = &pSrc[(j * iWidth + x) * 3];
                r += (uint64_t)q[0] * w; g += (uint64_t)q[1] * w; b += (uint64_t)q[2] * w;
            }
            // acc = avg_q << 16 where avg_q = avg << 8; recover channel depth
            uint16_t R = (uint16_t)((r + (1 << 23)) >> 24);
            uint16_t G = (uint16_t)((g + (1 << 23)) >> 24);
            uint16_t B = (uint16_t)((b + (1 << 23)) >> 24);
            d[x] = (uint16_t)((R << 11) | (G << 5) | B);
        }
    }
}

// Resample one cooked RGB565 frame from (iSrcW x iSrcH) to (iDstW x iDstH).
// pScratch must hold at least GIFResampleScratchSize(...) bytes (may be NULL
// when that returns 0). pSrc and pDst must not overlap.
static void GIFResample565(const uint16_t *pSrc, int iSrcW, int iSrcH,
                           uint16_t *pDst, int iDstW, int iDstH,
                           int iMode, void *pScratch)
{
    if (iMode == GIF_RESAMPLE_NEAREST || (iDstW >= iSrcW && iDstH >= iSrcH)) {
        // single-pass nearest (also covers forced-nearest downscaling)
        for (int y = 0; y < iDstH; y++) {
            int sy = GIFResampleNearest(y, iSrcH, iDstH);
            const uint16_t *s = &pSrc[sy * iSrcW];
            uint16_t *d = &pDst[y * iDstW];
            if (iDstW == iSrcW) {
                for (int x = 0; x < iDstW; x++) d[x] = s[x];
            } else {
                for (int x = 0; x < iDstW; x++)
                    d[x] = s[GIFResampleNearest(x, iSrcW, iDstW)];
            }
        }
        return;
    }
    // two-pass: horizontal into q8.8 intermediate, then vertical
    uint16_t *pTmp = (uint16_t *)pScratch; // iDstW x iSrcH x 3
    for (int y = 0; y < iSrcH; y++)
        GIFResamplePassH(&pSrc[y * iSrcW], iSrcW, &pTmp[y * iDstW * 3], iDstW, iMode);
    GIFResamplePassV(pTmp, iDstW, iSrcH, pDst, iDstH, iMode);
}

#ifdef __cplusplus
}
#endif

#endif // __GIF_RESAMPLE__
