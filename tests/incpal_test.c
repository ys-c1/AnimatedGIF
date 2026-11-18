//
// Incremental palette codec acceptance tests.
// Self-contained: synthetic palettes only, no files, no third-party code.
// Build: see run_incpal_test.sh (compiled once as C and once as C++).
//
#include "AnimatedGIF.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define ENTRIES      16                 // small 16-entry palette (48 bytes)
#define FRAMES       4                  // frames per acceptance sequence
#define PAL_BYTES    (ENTRIES * 3)
#define REC_CAP      GIF_INCPAL_MAX_RECORD_BYTES(ENTRIES)

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); g_failures++; } \
} while (0)

// Deterministic synthetic RGB value for (frame, entry, channel).
static uint8_t palValue(int f, int e, int ch)
{
    return (uint8_t)((f * 53 + e * 17 + ch * 131 + 7) & 0xff);
}

static void buildBase(uint8_t *p)
{
    int e, ch;
    for (e = 0; e < ENTRIES; e++)
        for (ch = 0; ch < 3; ch++)
            p[e * 3 + ch] = palValue(0, e, ch);
}

// changedEntries[f] = number of entries that differ from the base in frame f.
static void buildFrame(uint8_t *pFrame, const uint8_t *pBase, int f, int changed)
{
    int e, ch;
    memcpy(pFrame, pBase, PAL_BYTES);
    for (e = 0; e < changed; e++) {
        // touch a spread of indices; force every channel to actually differ
        int idx = (e * 7 + f) % ENTRIES;
        for (ch = 0; ch < 3; ch++)
            pFrame[idx * 3 + ch] = (uint8_t)(pBase[idx * 3 + ch] ^ 0xa5);
    }
}

static const char *modeName(uint8_t mode)
{
    switch (mode) {
        case GIF_INCPAL_UNCHANGED: return "UNCHANGED";
        case GIF_INCPAL_DELTA:     return "DELTA";
        case GIF_INCPAL_FULL:      return "FULL";
        default:                   return "?";
    }
}

typedef struct {
    const char *name;
    int changed[FRAMES];
    uint8_t expectMode[FRAMES];
} seq_t;

//
// One acceptance sequence: encode every frame against the same base,
// verify reconstruction through both decoder entry points and byte counts.
//
static void runSequence(const seq_t *sq, const uint8_t *base,
                        int *incBytes, int *baseBytes)
{
    int f;
    uint8_t frame[PAL_BYTES], rec[REC_CAP];
    uint8_t outDecode[PAL_BYTES], outApply[PAL_BYTES];

    printf("\n[%s]  %d-entry palette, %d frames; local-table cost = %d B/frame\n",
           sq->name, ENTRIES, FRAMES, PAL_BYTES);
    printf("  frame  changed  mode       rec bytes  local bytes  reconstruct\n");
    *incBytes = 0;
    *baseBytes = 0;
    for (f = 0; f < FRAMES; f++) {
        int recLen, rc;
        buildFrame(frame, base, f + 1, sq->changed[f]);
        recLen = GIF_incPalEncode(base, frame, ENTRIES, rec, sizeof(rec));
        CHECK(recLen > 0, "encoder returned an error");
        CHECK(rec[0] == sq->expectMode[f], "unexpected record mode");
        *incBytes += recLen;
        *baseBytes += PAL_BYTES; // classic approach: full local table per frame

        // path 1: decode into a fresh buffer from base + record
        memset(outDecode, 0, sizeof(outDecode));
        rc = GIF_incPalDecode(base, ENTRIES, rec, recLen, outDecode);
        CHECK(rc == GIF_INCPAL_OK, "decode error");
        CHECK(memcmp(outDecode, frame, PAL_BYTES) == 0,
              "decode() reconstruction mismatch");

        // path 2: stateful in-place apply on a buffer holding the base
        memcpy(outApply, base, PAL_BYTES);
        rc = GIF_incPalApply(outApply, ENTRIES, rec, recLen);
        CHECK(rc == GIF_INCPAL_OK, "apply error");
        CHECK(memcmp(outApply, frame, PAL_BYTES) == 0,
              "apply() reconstruction mismatch");

        printf("  %5d  %7d  %-9s  %9d  %11d  %s\n",
               f, sq->changed[f], modeName(rec[0]), recLen, PAL_BYTES,
               memcmp(outDecode, frame, PAL_BYTES) == 0 ? "OK" : "MISMATCH");
    }
    printf("  TOTAL  inc=%d B  local=%d B  saving=%.1f%%\n",
           *incBytes, *baseBytes,
           100.0 * (double)(*baseBytes - *incBytes) / (double)(*baseBytes));
}

//
// Threshold boundary on a 256-entry palette: delta iff 4*changed < 3*N,
// so 191/256 changed -> DELTA and 192/256 (exactly 75%) -> FULL fallback.
//
static void testThreshold(void)
{
    enum { N = 256 };
    static uint8_t base[N * 3], frame[N * 3], out[N * 3];
    uint8_t rec[GIF_INCPAL_MAX_RECORD_BYTES(N) + 256]; // room for the
                                                       // pre-fallback delta too
    int e, rc, recLen;
    printf("\n[threshold] 256-entry palette fallback criterion (4*changed >= 3*N)\n");
    for (e = 0; e < N * 3; e++)
        base[e] = (uint8_t)(e * 37 + 11);

    // 191 changed: 4*191=764 < 768 -> delta
    memcpy(frame, base, sizeof(frame));
    for (e = 0; e < 191; e++)
        frame[e * 3] ^= 0x5a;
    recLen = GIF_incPalEncode(base, frame, N, rec, sizeof(rec));
    CHECK(recLen > 0 && rec[0] == GIF_INCPAL_DELTA, "191/256 should be DELTA");
    rc = GIF_incPalDecode(base, N, rec, recLen, out);
    CHECK(rc == GIF_INCPAL_OK && memcmp(out, frame, sizeof(frame)) == 0,
          "191/256 delta reconstruction mismatch");
    printf("  changed=191 (74.6%%): DELTA, %d B (full table would be %d B)\n",
           recLen, 3 * N);

    // 192 changed: 4*192=768 == 768 -> full fallback (equality prefers FULL)
    memcpy(frame, base, sizeof(frame));
    for (e = 0; e < 192; e++)
        frame[e * 3] ^= 0x5a;
    recLen = GIF_incPalEncode(base, frame, N, rec, sizeof(rec));
    CHECK(recLen > 0 && rec[0] == GIF_INCPAL_FULL, "192/256 should be FULL");
    rc = GIF_incPalDecode(NULL, N, rec, recLen, out); // FULL needs no base
    CHECK(rc == GIF_INCPAL_OK && memcmp(out, frame, sizeof(frame)) == 0,
          "192/256 full reconstruction mismatch");
    printf("  changed=192 (75.0%%): FULL fallback, %d B\n", recLen);
}

//
// Error and edge handling: truncation, bad index, undersized output,
// NULL arguments, undersized encoder buffer.
//
static void testErrors(void)
{
    uint8_t base[PAL_BYTES], frame[PAL_BYTES], rec[REC_CAP], out[PAL_BYTES];
    int recLen, rc;
    printf("\n[error paths]\n");
    buildBase(base);
    buildFrame(frame, base, 1, 3);
    recLen = GIF_incPalEncode(base, frame, ENTRIES, rec, sizeof(rec));
    CHECK(recLen > 0, "setup encode failed");

    rc = GIF_incPalDecode(base, ENTRIES, rec, recLen - 1, out); // truncated
    CHECK(rc == GIF_INCPAL_BAD_RECORD, "truncated record rejected");
    rc = GIF_incPalApply(out, ENTRIES, rec, recLen - 1);
    CHECK(rc == GIF_INCPAL_BAD_RECORD, "truncated apply rejected");

    rec[recLen - 4] = (uint8_t)ENTRIES; // corrupt an entry index
    rc = GIF_incPalDecode(base, ENTRIES, rec, recLen, out);
    CHECK(rc == GIF_INCPAL_BAD_RECORD, "out-of-range index rejected");

    CHECK(GIF_incPalEncode(NULL, frame, ENTRIES, rec, sizeof(rec))
          == GIF_INCPAL_BAD_PARAMETER, "NULL base rejected");
    CHECK(GIF_incPalEncode(base, frame, ENTRIES, rec, 1)
          == GIF_INCPAL_BUFFER_TOO_SMALL, "undersized encoder buffer rejected");
    CHECK(GIF_incPalDecode(base, ENTRIES, NULL, 0, out)
          == GIF_INCPAL_BAD_PARAMETER, "NULL record rejected");
    CHECK(GIF_incPalCountChanges(base, frame, 0) == 0, "zero-size guard");
    CHECK(GIF_incPalCountChanges(base, frame, 9999) == 0, "oversize guard");
    printf("  invalid/truncated records and bad arguments all rejected: %s\n",
           g_failures == 0 ? "OK" : "FAILURES PRESENT");
}

//
// Chained decoding across frames: the decoder keeps the global palette as
// the permanent base plus one active palette buffer. A FULL record replaces
// the active table; later deltas are base-relative, so the active buffer is
// re-seeded from the global base before applying them. Mixes all three modes.
//
static void testChained(void)
{
    uint8_t base[PAL_BYTES], active[PAL_BYTES], frame[PAL_BYTES], rec[REC_CAP];
    int f, changed, recLen, rc;
    static const int changes[6] = {0, 2, 0, 6, ENTRIES, 3}; // frame 4 = FULL
    printf("\n[chained decode] global base + active palette across 6 frames\n");
    buildBase(base);
    memcpy(active, base, PAL_BYTES);
    for (f = 0; f < 6; f++) {
        changed = changes[f];
        buildFrame(frame, base, f + 1, changed);
        recLen = GIF_incPalEncode(base, frame, ENTRIES, rec, sizeof(rec));
        CHECK(recLen > 0, "chained encode failed");
        // FULL is self-contained; UNCHANGED/DELTA are base-relative, so seed
        // the active buffer from the global base before applying them.
        if (rec[0] != GIF_INCPAL_FULL)
            memcpy(active, base, PAL_BYTES);
        rc = GIF_incPalApply(active, ENTRIES, rec, recLen);
        CHECK(rc == GIF_INCPAL_OK, "chained apply failed");
        CHECK(memcmp(active, frame, PAL_BYTES) == 0,
              "chained active palette mismatch");
        // cross-check against the stateless decoder entry point as well
        {
            uint8_t ref[PAL_BYTES];
            rc = GIF_incPalDecode(base, ENTRIES, rec, recLen, ref);
            CHECK(rc == GIF_INCPAL_OK && memcmp(ref, frame, PAL_BYTES) == 0,
                  "chained stateless decode mismatch");
        }
        printf("  frame %d: changed=%2d mode=%-9s active palette %s\n",
               f, changed, modeName(rec[0]),
               memcmp(active, frame, PAL_BYTES) == 0 ? "matches frame" : "WRONG");
    }
}

int main(void)
{
    // Four mandated sequences.
    static const seq_t sequences[] = {
        { "S1 no change",        {0, 0, 0, 0},
          {GIF_INCPAL_UNCHANGED, GIF_INCPAL_UNCHANGED,
           GIF_INCPAL_UNCHANGED, GIF_INCPAL_UNCHANGED} },
        { "S2 single entry",     {1, 1, 1, 1},
          {GIF_INCPAL_DELTA, GIF_INCPAL_DELTA,
           GIF_INCPAL_DELTA, GIF_INCPAL_DELTA} },
        { "S3 multiple entries", {4, 4, 4, 4}, // 25% of 16
          {GIF_INCPAL_DELTA, GIF_INCPAL_DELTA,
           GIF_INCPAL_DELTA, GIF_INCPAL_DELTA} },
        { "S4 all changed",      {ENTRIES, ENTRIES, ENTRIES, ENTRIES},
          {GIF_INCPAL_FULL, GIF_INCPAL_FULL,
           GIF_INCPAL_FULL, GIF_INCPAL_FULL} },
    };
    uint8_t base[PAL_BYTES];
    int i, totalInc = 0, totalLocal = 0, incB, localB;
    double saving;

    buildBase(base);
    printf("== Incremental palette acceptance (palette: %d RGB888 entries, %d bytes) ==",
           ENTRIES, PAL_BYTES);
    for (i = 0; i < 4; i++) {
        runSequence(&sequences[i], base, &incB, &localB);
        totalInc += incB;
        totalLocal += localB;
    }
    saving = 100.0 * (double)(totalLocal - totalInc) / (double)totalLocal;
    printf("\n== Aggregate over all 4 sequences (16 frames) ==\n");
    printf("  incremental: %d B | local tables: %d B | saving: %.1f%% (target >= 40%%)\n",
           totalInc, totalLocal, saving);
    CHECK(saving >= 40.0, "palette size reduction below 40%");

    testChained();
    testThreshold();
    testErrors();

    printf("\n%s\n", g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED");
    return g_failures == 0 ? 0 : 1;
}
