//
// lzw_strategy.cpp — GIF LZW 编码策略优化实验（新增测试，不改动库文件）
//
// 内容：
//  1. 自带 GIF LZW 编码器，支持三种策略：
//       S0 固定策略   : 最小码长恒为 8，字典满(4096)时发清除码重置（基线）
//       S1 自适应码长 : 按数据特征(实际颜色数)选最小码长 2..8，字典满时重置
//       S2 自适应+冻结: 按数据特征选最小码长，字典满时冻结字典（不发清除码）
//     清除码插入时机：码流起始必发一次；之后仅在“字典达到阈值且非冻结”时插入。
//  2. 严格校验解码器：可解任意策略产出的码流；码长越界(最小码长非2..8 / 字典码越界)、
//     清除码缺失、EOI 缺失均报错。
//  3. 用仓库自带的 AnimatedGIF 解码器交叉验证解码一致性（逐像素索引比对）。
//  4. 四类 128x128 测试帧（纯色/渐变/噪点/文字）程序内生成，一次跑完。
//
// 仅依赖仓库自带 AnimatedGIF 库与 C++ 标准库，无网络访问。
//
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <chrono>
#include "../../../src/AnimatedGIF.h"

static const int W = 128, H = 128;          // 测试帧尺寸（小样例）
static const int MAX_CODE = 4096;           // GIF LZW 字典上限 2^12

//============================================================
// 位流写入器（GIF LZW：LSB 在前）
//============================================================
struct BitWriter {
    std::vector<uint8_t> out;
    uint32_t acc = 0; int nbits = 0;
    void write(uint32_t code, int bits) {
        acc |= code << nbits; nbits += bits;
        while (nbits >= 8) { out.push_back(acc & 0xff); acc >>= 8; nbits -= 8; }
    }
    void flush() { if (nbits) { out.push_back(acc & 0xff); acc = 0; nbits = 0; } }
};

//============================================================
// LZW 编码器（策略可配）
//   minCodeSize   : 2..8；0 表示按数据特征自动选择
//   resetThreshold: 字典条目数达到该值时插入清除码并重置（<=4096；4096=字典满才重置）
//   freezeOnFull  : true 时字典满不重置（冻结字典，不再插入清除码）
//============================================================
struct EncOpts {
    int  minCodeSize = 0;        // 0 = auto
    int  resetThreshold = MAX_CODE;
    bool freezeOnFull = false;
};
struct EncResult {
    std::vector<uint8_t> bytes;  // LZW 压缩数据（未分子块）
    int minCodeSize = 0;
    int resets = 0;              // 字典重置次数（不含起始必发的清除码）
    int codes = 0;               // 发出的码总数
    double us = 0.0;             // 编码耗时（微秒）
};

// 按数据特征选择最小码长：能容纳全部颜色索引的最小位数，限制在 2..8
static int autoMinCodeSize(const uint8_t *pix, int n) {
    bool used[256] = {false};
    int maxIdx = 0, colors = 0;
    for (int i = 0; i < n; i++) {
        if (!used[pix[i]]) { used[pix[i]] = true; colors++; }
        if (pix[i] > maxIdx) maxIdx = pix[i];
    }
    int bits = 0; while ((1 << bits) <= maxIdx) bits++;   // ceil(log2(colors))
    if (bits < 2) bits = 2;
    if (bits > 8) bits = 8;
    (void)colors;
    return bits;
}

// 字典：开放寻址哈希，key=(prefix<<8)|suffix，value=code+1（0=空）
struct Dict {
    std::vector<uint32_t> tab;
    Dict() { tab.assign(1 << 15, 0); }
    void clear() { memset(tab.data(), 0, tab.size() * sizeof(uint32_t)); }
    static uint32_t slot(int prefix, int suffix) {
        return ((uint32_t)(prefix * 256 + suffix) * 2654435761u) >> 17;
    }
    int find(int prefix, int suffix) const {
        uint32_t s = slot(prefix, suffix);
        while (tab[s]) {
            if ((int)(tab[s] >> 20) == prefix && (int)((tab[s] >> 12) & 0xff) == suffix)
                return (int)(tab[s] & 0xfff) - 1;
            s = (s + 1) & ((1 << 15) - 1);
        }
        return -1;
    }
    void add(int prefix, int suffix, int code) {
        uint32_t s = slot(prefix, suffix);
        while (tab[s]) s = (s + 1) & ((1 << 15) - 1);
        tab[s] = ((uint32_t)prefix << 20) | ((uint32_t)suffix << 12) | (uint32_t)(code + 1);
    }
};

static EncResult lzwEncode(const uint8_t *pix, int n, const EncOpts &opt) {
    EncResult r;
    auto t0 = std::chrono::steady_clock::now();

    int mcs = opt.minCodeSize ? opt.minCodeSize : autoMinCodeSize(pix, n);
    r.minCodeSize = mcs;
    const int cc = 1 << mcs, eoi = cc + 1;
    int codesize = mcs + 1;
    int nextcode = cc + 2;
    Dict dict;
    BitWriter bw;

    bw.write(cc, codesize);              // 清除码：码流起始必发
    r.codes++;
    int prefix = pix[0];
    for (int i = 1; i < n; i++) {
        int b = pix[i];
        int found = dict.find(prefix, b);
        if (found >= 0) { prefix = found; continue; }
        // 未命中：输出当前前缀码
        bw.write(prefix, codesize); r.codes++;
        if (nextcode >= opt.resetThreshold) {
            if (opt.freezeOnFull && opt.resetThreshold >= MAX_CODE) {
                // 冻结策略：字典满后不再添加新条目，也不发清除码
            } else {
                // 重置策略：字典满（或达阈值）时插入清除码。
                // 注意：解码端在处理刚输出的码时仍会添加一个条目，
                // 若因此达到 2^codesize 会先升位，故清除码必须按解码端
                // 升位后的码长发出（仅边界情形有影响）。
                if (nextcode == (1 << codesize) && codesize < 12) codesize++;
                bw.write(cc, codesize); r.codes++;
                r.resets++;
                codesize = mcs + 1; nextcode = cc + 2; dict.clear();
            }
        } else {
            dict.add(prefix, b, nextcode); nextcode++;
            // 与解码端同步的码长增长规则（GIF 延迟增长）：
            // 解码端在 nextcode 达到 2^codesize 时升位，编码端字典领先一个条目，
            // 故编码端在 nextcode 超过 2^codesize 时才升位。
            if (nextcode > (1 << codesize) && codesize < 12) codesize++;
        }
        prefix = b;
    }
    bw.write(prefix, codesize); r.codes++;
    bw.write(eoi, codesize); r.codes++;
    bw.flush();
    r.bytes = std::move(bw.out);

    auto t1 = std::chrono::steady_clock::now();
    r.us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    return r;
}

//============================================================
// GIF 容器封装（GIF89a，全局色表，单帧，无交织）
//============================================================
static std::vector<uint8_t> buildGIF(int w, int h,
                                     const uint8_t *palette, int palColors,
                                     const EncResult &enc) {
    std::vector<uint8_t> g;
    auto put  = [&](uint8_t b) { g.push_back(b); };
    auto put16= [&](int v) { g.push_back(v & 0xff); g.push_back((v >> 8) & 0xff); };
    int gctBits = 1; while ((1 << gctBits) < palColors) gctBits++;
    if (gctBits < 1) gctBits = 1;
    const char *hdr = "GIF89a";
    for (int i = 0; i < 6; i++) put(hdr[i]);
    put16(w); put16(h);
    put(0xF0 | (gctBits - 1));          // GCT 标志 + 彩色分辨率 + GCT 大小
    put(0); put(0);                     // 背景色 / 像素纵横比
    for (int i = 0; i < (1 << gctBits); i++) {
        put(palette[3 * (i % palColors) + 0]);
        put(palette[3 * (i % palColors) + 1]);
        put(palette[3 * (i % palColors) + 2]);
    }
    put(0x2C); put16(0); put16(0); put16(w); put16(h); put(0); // 图像描述符
    put((uint8_t)enc.minCodeSize);                              // LZW 最小码长
    // 子块分割（每块 <=255 字节）
    size_t off = 0;
    while (off < enc.bytes.size()) {
        int c = (int)std::min<size_t>(255, enc.bytes.size() - off);
        put((uint8_t)c);
        for (int i = 0; i < c; i++) put(enc.bytes[off + i]);
        off += c;
    }
    put(0);                             // 块终止
    put(0x3B);                          // 文件尾
    return g;
}

//============================================================
// 严格校验解码器（解码端：能读任意策略产出的码流，非法码流必报错）
//============================================================
enum DecErr {
    DEC_OK = 0,
    DEC_ERR_CODE_SIZE_RANGE,   // 码长越界：最小码长不在 2..8
    DEC_ERR_MISSING_CLEAR,     // 清除码缺失：首码不是清除码
    DEC_ERR_CODE_OOB,          // 码长越界：字典码超出当前有效范围
    DEC_ERR_NO_EOI,            // 数据耗尽仍无结束码
    DEC_ERR_LENGTH             // 解码像素数与图像尺寸不符
};
static const char *decErrStr(DecErr e) {
    switch (e) {
        case DEC_OK: return "OK";
        case DEC_ERR_CODE_SIZE_RANGE: return "码长越界(最小码长非2..8)";
        case DEC_ERR_MISSING_CLEAR: return "清除码缺失(首码非清除码)";
        case DEC_ERR_CODE_OOB: return "码长越界(字典码超出有效范围)";
        case DEC_ERR_NO_EOI: return "结束码缺失(EOI)";
        case DEC_ERR_LENGTH: return "解码长度不符";
    }
    return "?";
}

struct BitReader {
    const uint8_t *d; size_t n, pos = 0;
    int read(int bits, bool &ok) {
        if (pos + bits > n * 8) { ok = false; return 0; }
        uint32_t v = 0;
        for (int i = 0; i < bits; i++)
            v |= ((d[(pos + i) >> 3] >> ((pos + i) & 7)) & 1) << i;
        pos += bits;
        return (int)v;
    }
};

static DecErr lzwDecodeStrict(const uint8_t *data, size_t len, int minCodeSize,
                              uint8_t *out, int expectedPixels, int *pResets) {
    if (minCodeSize < 2 || minCodeSize > 8) return DEC_ERR_CODE_SIZE_RANGE; // 码长越界
    const int cc = 1 << minCodeSize, eoi = cc + 1;
    uint16_t prefixTab[MAX_CODE];
    uint8_t  suffixTab[MAX_CODE], stack[MAX_CODE + 1];
    int codesize = minCodeSize + 1, nextcode = cc + 2;
    BitReader br{data, len, 0};
    bool ok = true;
    int code = br.read(codesize, ok);
    if (!ok) return DEC_ERR_NO_EOI;
    if (code != cc) return DEC_ERR_MISSING_CLEAR;                           // 清除码缺失
    int prev = -1, produced = 0, resets = 0;
    int firstChar = 0;
    auto emitString = [&](int c) {
        int top = 0;
        while (c >= cc) { stack[top++] = suffixTab[c]; c = prefixTab[c]; }
        firstChar = c;
        stack[top++] = (uint8_t)c;
        while (top > 0 && produced < expectedPixels) out[produced++] = stack[--top];
    };
    while (true) {
        code = br.read(codesize, ok);
        if (!ok) return DEC_ERR_NO_EOI;
        if (code == cc) {                       // 清除码：重置字典
            codesize = minCodeSize + 1; nextcode = cc + 2; prev = -1; resets++;
            continue;
        }
        if (code == eoi) break;
        if (code > nextcode || code >= MAX_CODE) return DEC_ERR_CODE_OOB;   // 码长越界
        if (code == nextcode) {                 // KwKwK 情形
            if (prev < 0) return DEC_ERR_CODE_OOB;
            emitString(prev);
            if (produced < expectedPixels) out[produced++] = (uint8_t)firstChar;
        } else {
            emitString(code);
        }
        if (prev >= 0 && nextcode < MAX_CODE) { // 与编码端同步地增长字典
            prefixTab[nextcode] = (uint16_t)prev;
            suffixTab[nextcode] = (uint8_t)firstChar;
            nextcode++;
            if (nextcode == (1 << codesize) && codesize < 12) codesize++;
        }
        prev = code;
    }
    if (produced != expectedPixels) return DEC_ERR_LENGTH;
    if (pResets) *pResets = resets;
    return DEC_OK;
}

//============================================================
// 用仓库自带 AnimatedGIF 解码器交叉验证
//============================================================
static uint8_t g_canvas[W * H];
static int g_lines;
static void GIFDraw(GIFDRAW *pDraw) {
    memcpy(&g_canvas[(pDraw->iY + pDraw->y) * W + pDraw->iX], pDraw->pPixels, pDraw->iWidth);
    g_lines++;
}
static bool repoDecodeCheck(const std::vector<uint8_t> &gifData,
                            const uint8_t *expected, int npix) {
    AnimatedGIF gif;
    gif.begin(GIF_PALETTE_RGB888);
    if (!gif.open((uint8_t *)gifData.data(), (int)gifData.size(), GIFDraw)) return false;
    int delay = 0;
    g_lines = 0;
    int rc = gif.playFrame(false, &delay);   // 单帧文件解码完会返回0，不能据此判失败
    gif.close();
    if (rc < 0 || g_lines != H) return false;
    return memcmp(g_canvas, expected, npix) == 0;
}

//============================================================
// 测试帧生成（128x128，调色板索引）
//============================================================
struct Frame {
    const char *name;
    std::vector<uint8_t> pix;      // W*H 索引
    std::vector<uint8_t> palette;  // 3*palColors
    int palColors;
};

// 5x7 点阵字体（列主序，LSB=顶），仅覆盖测试字符串所需字符
static const struct { char ch; uint8_t col[5]; } FONT[] = {
    {'0',{0x3E,0x51,0x49,0x45,0x3E}}, {'1',{0x00,0x42,0x7F,0x40,0x00}},
    {'2',{0x42,0x61,0x51,0x49,0x46}}, {'6',{0x3C,0x4A,0x49,0x49,0x30}},
    {'E',{0x7F,0x49,0x49,0x49,0x41}}, {'F',{0x7F,0x49,0x49,0x49,0x01}},
    {'G',{0x3E,0x41,0x49,0x49,0x7A}}, {'I',{0x00,0x41,0x7F,0x41,0x00}},
    {'L',{0x7F,0x40,0x40,0x40,0x40}}, {'S',{0x46,0x49,0x49,0x49,0x31}},
    {'T',{0x01,0x01,0x7F,0x01,0x01}}, {'W',{0x3F,0x40,0x38,0x40,0x3F}},
    {'Z',{0x61,0x51,0x49,0x45,0x43}}, {' ',{0x00,0x00,0x00,0x00,0x00}},
};
static void drawText(Frame &f, int x0, int y0, const char *s, uint8_t idx) {
    for (int i = 0; s[i]; i++) {
        const uint8_t *col = nullptr;
        for (auto &g : FONT) if (g.ch == s[i]) col = g.col;
        if (!col) continue;
        for (int cx = 0; cx < 5; cx++)
            for (int cy = 0; cy < 7; cy++)
                if (col[cx] & (1 << cy))
                    f.pix[(y0 + cy) * W + x0 + i * 6 + cx] = idx;
    }
}

static Frame makeSolid() {
    Frame f{"纯色", std::vector<uint8_t>(W * H, 1), {255,255,255, 255,128,0}, 2};
    return f;
}
static Frame makeGradient() {
    Frame f{"渐变", std::vector<uint8_t>(W * H), std::vector<uint8_t>(128 * 3), 128};
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) f.pix[y * W + x] = (uint8_t)x;
    for (int i = 0; i < 128; i++) {
        f.palette[3*i+0] = (uint8_t)(i*2); f.palette[3*i+1] = (uint8_t)(255-i*2);
        f.palette[3*i+2] = (uint8_t)(i);
    }
    return f;
}
static Frame makeNoise() {
    Frame f{"噪点", std::vector<uint8_t>(W * H), std::vector<uint8_t>(256 * 3), 256};
    srand(12345);
    for (auto &p : f.pix) p = (uint8_t)(rand() & 0xff);
    for (int i = 0; i < 256; i++) {
        f.palette[3*i+0] = (uint8_t)(i); f.palette[3*i+1] = (uint8_t)(255-i);
        f.palette[3*i+2] = (uint8_t)((i*37) & 0xff);
    }
    return f;
}
static Frame makeText() {
    Frame f{"文字", std::vector<uint8_t>(W * H, 0), {255,255,255, 0,0,0}, 2};
    drawText(f, 13, 50, "GIF LZW TEST", 1);
    drawText(f, 43, 70, "2026", 1);
    return f;
}

//============================================================
// 实验
//============================================================
// S2 全自适应：按数据特征决定“字典满时是否重置 / 清除码插入时机”。
// 对候选策略（满重置 / 满冻结 / 提前重置）分别试编码，取体积最小者。
static EncResult lzwEncodeAdaptive(const uint8_t *pix, int n) {
    const EncOpts cands[3] = {
        {0, MAX_CODE, false},   // 字典满时重置
        {0, MAX_CODE, true},    // 字典满时冻结（不再插入清除码）
        {0, 512, false},        // 提前插入清除码（高熵数据小码长更划算）
    };
    EncResult best; size_t bestSz = SIZE_MAX; double totalUs = 0;
    for (const EncOpts &o : cands) {
        EncResult r = lzwEncode(pix, n, o);
        totalUs += r.us;
        if (r.bytes.size() < bestSz) { bestSz = r.bytes.size(); best = std::move(r); }
    }
    best.us = totalUs;
    return best;
}

struct Strategy {
    const char *name;
    EncOpts opt;
    bool adaptive;              // true = 使用 lzwEncodeAdaptive
};
static EncResult encodeWith(const uint8_t *pix, int n, const Strategy &s) {
    return s.adaptive ? lzwEncodeAdaptive(pix, n) : lzwEncode(pix, n, s.opt);
}

int main() {
    printf("=== GIF LZW 编码策略优化实验 (帧尺寸 %dx%d, 单帧) ===\n\n", W, H);

    Frame frames[4] = { makeSolid(), makeGradient(), makeNoise(), makeText() };
    Strategy strats[3] = {
        {"S0 固定(码长8+满重置)",   {8, MAX_CODE, false}, false},
        {"S1 自适应码长+满重置",    {0, MAX_CODE, false}, false},
        {"S2 全自适应(码长+清除时机)", {},                true },
    };
    const int NP = W * H;
    bool allOK = true;

    // ---- 实验1：四类帧 x 三种策略：体积 / 编码耗时 / 解码一致性 ----
    printf("--- 实验1: 体积 / 编码耗时 / 解码一致性 ---\n");
    printf("%-6s %-22s %4s %9s %10s %8s %8s %8s\n",
           "帧", "策略", "码长", "GIF字节", "编码耗时us", "重置数", "严格解码", "库解码");
    long totalFixed = 0, totalBest = 0;
    for (auto &f : frames) {
        long fixedSize = -1, bestSize = -1;
        for (auto &s : strats) {
            EncResult enc;
            double best = 1e30;
            for (int rep = 0; rep < 5; rep++) {          // 取5次最稳定耗时
                EncResult t = encodeWith(f.pix.data(), NP, s);
                if (t.us < best) { best = t.us; enc = std::move(t); }
            }
            std::vector<uint8_t> gif = buildGIF(W, H, f.palette.data(), f.palColors, enc);
            // 严格解码器校验
            std::vector<uint8_t> dec(NP);
            DecErr de = lzwDecodeStrict(enc.bytes.data(), enc.bytes.size(),
                                        enc.minCodeSize, dec.data(), NP, nullptr);
            bool strictOK = (de == DEC_OK && memcmp(dec.data(), f.pix.data(), NP) == 0);
            // 仓库解码器交叉校验
            bool repoOK = repoDecodeCheck(gif, f.pix.data(), NP);
            printf("%-6s %-22s %4d %9d %10.1f %8d %8s %8s\n",
                   f.name, s.name, enc.minCodeSize, (int)gif.size(), enc.us,
                   enc.resets, strictOK ? "一致" : decErrStr(de), repoOK ? "一致" : "不一致");
            if (!strictOK || !repoOK) allOK = false;
            if (fixedSize < 0) fixedSize = (long)gif.size();
            if (bestSize < 0 || (long)gif.size() < bestSize) bestSize = (long)gif.size();
        }
        totalFixed += fixedSize; totalBest += bestSize;
        printf("  -> %-4s 最优 vs 固定: %ld -> %ld 字节 (%.1f%%)\n",
               f.name, fixedSize, bestSize, 100.0 * (fixedSize - bestSize) / fixedSize);
    }
    double totalDrop = 100.0 * (totalFixed - totalBest) / totalFixed;
    printf("总计: 固定 %ld 字节, 逐帧最优 %ld 字节, 下降 %.1f%% => 验收(>=10%%): %s\n\n",
           totalFixed, totalBest, totalDrop, totalDrop >= 10.0 ? "通过" : "未通过");
    if (totalDrop < 10.0) allOK = false;

    // ---- 实验2：字典重置次数与体积的关系（阈值扫描，>=3 个数据点）----
    printf("--- 实验2: 字典重置阈值 -> 重置次数 / 体积 ---\n");
    printf("%-6s %8s %8s %10s\n", "帧", "阈值", "重置数", "GIF字节");
    struct { Frame *f; } exp2[2] = { {&frames[2]}, {&frames[1]} }; // 噪点 + 渐变
    int thresholds[3] = {512, 2048, 4096};
    for (auto &e : exp2) {
        for (int th : thresholds) {
            EncOpts o{0, th, false};
            EncResult enc = lzwEncode(e.f->pix.data(), NP, o);
            std::vector<uint8_t> gif = buildGIF(W, H, e.f->palette.data(), e.f->palColors, enc);
            std::vector<uint8_t> dec(NP);
            DecErr de = lzwDecodeStrict(enc.bytes.data(), enc.bytes.size(),
                                        enc.minCodeSize, dec.data(), NP, nullptr);
            bool okc = (de == DEC_OK && memcmp(dec.data(), e.f->pix.data(), NP) == 0);
            printf("%-6s %8d %8d %10d   解码%s\n", e.f->name, th, enc.resets,
                   (int)gif.size(), okc ? "一致" : "不一致");
            if (!okc) allOK = false;
        }
        EncOpts of{0, MAX_CODE, true};   // 冻结 = 0 次重置对照点
        EncResult enc = lzwEncode(e.f->pix.data(), NP, of);
        std::vector<uint8_t> gif = buildGIF(W, H, e.f->palette.data(), e.f->palColors, enc);
        printf("%-6s %8s %8d %10d   (冻结对照)\n", e.f->name, "冻结", enc.resets, (int)gif.size());
    }
    printf("\n");

    // ---- 实验3：非法码流必须报错 ----
    printf("--- 实验3: 码长越界 / 清除码缺失 报错检查 ---\n");
    bool e1 = true, e2 = true, e3 = true, e4 = true;
    {
        // 取一段合法噪点码流，分别声明非法最小码长 1 和 9
        EncResult enc = lzwEncode(frames[2].pix.data(), NP, {8, MAX_CODE, false});
        std::vector<uint8_t> dec(NP);
        e1 = (lzwDecodeStrict(enc.bytes.data(), enc.bytes.size(), 1, dec.data(), NP, nullptr)
              == DEC_ERR_CODE_SIZE_RANGE);
        e2 = (lzwDecodeStrict(enc.bytes.data(), enc.bytes.size(), 9, dec.data(), NP, nullptr)
              == DEC_ERR_CODE_SIZE_RANGE);
        // 清除码缺失：手工构造首码非清除码的码流 (minCode=2, cc=4, eoi=5)
        BitWriter bw; bw.write(0, 3); bw.write(5, 3); bw.flush();
        e3 = (lzwDecodeStrict(bw.out.data(), bw.out.size(), 2, dec.data(), NP, nullptr)
              == DEC_ERR_MISSING_CLEAR);
        // 字典码越界：clear, 0, 7(>nextcode=6), eoi
        BitWriter bw2; bw2.write(4, 3); bw2.write(0, 3); bw2.write(7, 3); bw2.write(5, 3); bw2.flush();
        e4 = (lzwDecodeStrict(bw2.out.data(), bw2.out.size(), 2, dec.data(), NP, nullptr)
              == DEC_ERR_CODE_OOB);
    }
    printf("最小码长=1 报错: %s\n", e1 ? "PASS" : "FAIL");
    printf("最小码长=9 报错: %s\n", e2 ? "PASS" : "FAIL");
    printf("清除码缺失报错: %s\n", e3 ? "PASS" : "FAIL");
    printf("字典码越界报错: %s\n", e4 ? "PASS" : "FAIL");
    if (!(e1 && e2 && e3 && e4)) allOK = false;

    // ---- 导出样例 GIF（每帧取最优策略，便于人工检查）----
    for (int fi = 0; fi < 4; fi++) {
        Frame &f = frames[fi];
        EncResult best; size_t bestSz = SIZE_MAX; const char *fn = nullptr;
        for (auto &s : strats) {
            EncResult enc = encodeWith(f.pix.data(), NP, s);
            std::vector<uint8_t> gif = buildGIF(W, H, f.palette.data(), f.palColors, enc);
            if (gif.size() < bestSz) {
                bestSz = gif.size(); best = std::move(enc);
                static char names[4][64];
                snprintf(names[fi], 64, "sample_%s.gif",
                         fi==0?"solid":fi==1?"gradient":fi==2?"noise":"text");
                fn = names[fi];
                FILE *fp = fopen(fn, "wb");
                if (fp) { fwrite(gif.data(), 1, gif.size(), fp); fclose(fp); }
            }
        }
        if (fn) printf("已导出 %s (%d 字节)\n", fn, (int)bestSz);
    }

    printf("\n=== 总体结论: %s ===\n", allOK ? "全部通过" : "存在失败项");
    return allOK ? 0 : 1;
}
