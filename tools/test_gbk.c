/**
 * @file test_gbk.c
 * @brief gbk.c 主机单测（PC 直跑，不依赖硬件/FatFs/LVGL）
 *
 * 编译运行：
 *   cd tools && gcc -I host_stub -I .. test_gbk.c ../gbk.c -o test_gbk.exe && ./test_gbk.exe
 *
 * 覆盖：
 *   - 编码检测：BOM 三种 / 无 BOM UTF-8（含 4KB 块边界截断复现用例）/ GBK /
 *     纯 ASCII / 长 ASCII 前缀 + GBK / 空文件 / 随机二进制
 *   - 流式嗅探：跨 feed 的字符拼接 / 非法序列 / 超短形式 / 代理区
 *   - 解码：GBK / GB18030 四字节 / UTF-8 / UTF-16LE/BE（含代理对与 0x0A 低位陷阱）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gbk.h"

/* ==================== FatFs 内存桩 ==================== */

static const uint8_t *g_mem;
static int g_mem_len;

FRESULT f_lseek(FIL *fp, int64_t ofs) {
    fp->pos = (int)ofs;
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br) {
    int avail = g_mem_len - fp->pos;
    if (avail < 0) avail = 0;
    UINT n = (btr < (UINT)avail) ? btr : (UINT)avail;
    memcpy(buff, g_mem + fp->pos, n);
    fp->pos += (int)n;
    *br = n;
    return FR_OK;
}

static TxtEncoding detect(const uint8_t *data, int len) {
    g_mem = data;
    g_mem_len = len;
    FIL fp = {0};
    return gbk_detect_encoding(&fp);
}

/* ==================== 断言 ==================== */

static int g_failed = 0, g_total = 0;

#define CHECK(cond, name) do { \
    g_total++; \
    if (cond) { printf("PASS  %s\n", name); } \
    else       { g_failed++; printf("FAIL  %s   (%s:%d)\n", name, __FILE__, __LINE__); } \
} while (0)

/* ==================== 用例 ==================== */

static void test_detection(void) {
    static uint8_t buf[320 * 1024];

    /* 1. UTF-8 BOM + 中文 */
    {
        const uint8_t d[] = {0xEF,0xBB,0xBF, 'a', 0xE4,0xB8,0xAD, 0xE6,0x96,0x87};
        CHECK(detect(d, sizeof(d)) == TXT_ENC_UTF8_BOM, "detect: UTF-8 BOM");
    }
    /* 2. 无 BOM UTF-8，第一块(4096B)末字节恰是多字节 lead —— 旧算法误判 GBK 的复现 */
    {
        memset(buf, 'A', 4096);
        buf[4095] = 0xE6;                       /* 截断在字符中间 */
        buf[4096] = 0xB5; buf[4097] = 0x8B;     /* 属于第二块 */
        CHECK(detect(buf, 4098) == TXT_ENC_UTF8, "detect: 4KB 边界截断判 UTF-8（P0 复现）");
    }
    /* 3. 第二块边界截断同理 */
    {
        memset(buf, 'A', 8192);
        buf[8191] = 0xE4;
        buf[8192] = 0xB8; buf[8193] = 0xAD;
        CHECK(detect(buf, 8194) == TXT_ENC_UTF8, "detect: 8KB 边界截断判 UTF-8");
    }
    /* 4. 无 BOM 正常 UTF-8 中文 */
    {
        const uint8_t d[] = {'h','i','\n', 0xE4,0xB8,0xAD, 0xE6,0x96,0x87, 0xE5,0x86,0x8D, 0xE8,0xA7,0x81};
        CHECK(detect(d, sizeof(d)) == TXT_ENC_UTF8, "detect: 无 BOM UTF-8 中文");
    }
    /* 5. GBK 中文（旧算法也正确，回归） */
    {
        const uint8_t d[] = {'h','i','\n', 0xB2,0xE2, 0xCA,0xD4, 0xD6,0xD0, 0xCE,0xC4};
        CHECK(detect(d, sizeof(d)) == TXT_ENC_GBK, "detect: GBK 中文");
    }
    /* 6. 纯 ASCII 300KB（超过 256KB 采样上限）→ UTF-8（解码等价） */
    {
        memset(buf, 'A', 300 * 1024);
        CHECK(detect(buf, 300 * 1024) == TXT_ENC_UTF8, "detect: 纯 ASCII 300KB");
    }
    /* 7. 100KB ASCII 前缀 + GBK（旧算法只看 4KB 会误判 UTF-8） */
    {
        memset(buf, 'A', 100 * 1024);
        for (int i = 100 * 1024; i < 100 * 1024 + 64; i += 2) {
            buf[i] = 0xB2; buf[i + 1] = 0xE2;   /* GBK「测」 */
        }
        CHECK(detect(buf, 100 * 1024 + 64) == TXT_ENC_GBK, "detect: 100KB ASCII 前缀 + GBK");
    }
    /* 8. UTF-16LE BOM + 中文「中」= 2D 4E */
    {
        const uint8_t d[] = {0xFF,0xFE, 'a',0x00, 0x2D,0x4E, 0x87,0x65};
        CHECK(detect(d, sizeof(d)) == TXT_ENC_UTF16LE, "detect: UTF-16LE BOM");
    }
    /* 9. UTF-16BE BOM */
    {
        const uint8_t d[] = {0xFE,0xFF, 0x00,'a', 0x4E,0x2D};
        CHECK(detect(d, sizeof(d)) == TXT_ENC_UTF16BE, "detect: UTF-16BE BOM");
    }
    /* 10. 空文件不崩溃 */
    CHECK(detect((const uint8_t *)"", 0) == TXT_ENC_UTF8, "detect: 空文件");
    /* 11. 单字节 0xEF（BOM 不足 3 字节，按采样流处理） */
    {
        const uint8_t d[] = {0xEF};
        CHECK(detect(d, 1) == TXT_ENC_UTF8, "detect: 单字节 0xEF");
    }
    /* 12. 随机二进制 → GBK 兜底 */
    {
        srand(42);
        for (int i = 0; i < 8192; i++) buf[i] = (uint8_t)rand();
        CHECK(detect(buf, 8192) == TXT_ENC_GBK, "detect: 随机二进制兜底 GBK");
    }
}

static void test_sniff(void) {
    TxtSniffState st;

    /* 跨 feed 的字符拼接：E6 B5 | 8B */
    txt_sniff_reset(&st);
    const uint8_t p1[] = {0xE6, 0xB5};
    const uint8_t p2[] = {0x8B};
    txt_sniff_feed(&st, p1, 2);
    txt_sniff_feed(&st, p2, 1);
    CHECK(!st.utf8_broken && st.need == 0, "sniff: 跨块字符拼接");

    /* 尾部截断不算非法（E4 B8 后还差 1 个 continuation） */
    txt_sniff_reset(&st);
    const uint8_t p3[] = {'A', 0xE4, 0xB8};
    txt_sniff_feed(&st, p3, 3);
    CHECK(!st.utf8_broken && st.need == 1, "sniff: 块尾截断 pending");

    /* continuation 后跟 ASCII lead → 非法 */
    txt_sniff_reset(&st);
    const uint8_t p4[] = {0xC2, 'A'};
    txt_sniff_feed(&st, p4, 2);
    CHECK(st.utf8_broken, "sniff: C2 41 非法");

    /* 超短形式 C0 80 → 非法 */
    txt_sniff_reset(&st);
    const uint8_t p5[] = {0xC0, 0x80};
    txt_sniff_feed(&st, p5, 2);
    CHECK(st.utf8_broken, "sniff: 超短形式 C0 80");

    /* UTF-8 编码的代理区 ED A0 80 → 非法 */
    txt_sniff_reset(&st);
    const uint8_t p6[] = {0xED, 0xA0, 0x80};
    txt_sniff_feed(&st, p6, 3);
    CHECK(st.utf8_broken, "sniff: 代理区 ED A0 80");

    /* gbk_is_valid_utf8 尾部截断 → true（旧实现返回 false） */
    {
        const uint8_t d[] = {'A', 'B', 0xE6, 0xB5};
        CHECK(gbk_is_valid_utf8(d, 4), "is_valid_utf8: 尾部截断容忍");
    }
}

static void test_decode(void) {
    uint32_t cp;
    int adv;

    /* GBK「测」= B2 E2 → U+6D4B */
    {
        const uint8_t d[] = {0xB2, 0xE2, 0x00};
        adv = gbk_decode_next(TXT_ENC_GBK, d, d + 2, &cp);
        CHECK(adv == 2 && cp == 0x6D4B, "decode: GBK 测→U+6D4B");
    }
    /* GBK「试」= CA D4 → U+8BD5 */
    {
        const uint8_t d[] = {0xCA, 0xD4};
        adv = gbk_decode_next(TXT_ENC_GBK, d, d + 2, &cp);
        CHECK(adv == 2 && cp == 0x8BD5, "decode: GBK 试→U+8BD5");
    }
    /* GBK 非法 trail(0x7F) → 空格 adv 1 */
    {
        const uint8_t d[] = {0xB2, 0x7F};
        adv = gbk_decode_next(TXT_ENC_GBK, d, d + 2, &cp);
        CHECK(adv == 1 && cp == 0x20, "decode: GBK 非法 trail 兜底");
    }
    /* GBK 末尾孤立 lead → 空格 adv 1 */
    {
        const uint8_t d[] = {0xB2};
        adv = gbk_decode_next(TXT_ENC_GBK, d, d + 1, &cp);
        CHECK(adv == 1 && cp == 0x20, "decode: GBK 孤立 lead 兜底");
    }
    /* GB18030 四字节（81 30 81 30）：整组消费不错位，后续「中」正常解出 */
    {
        const uint8_t d[] = {0x81, 0x30, 0x81, 0x30, 0xD6, 0xD0, 0x00};
        adv = gbk_decode_next(TXT_ENC_GBK, d, d + 6, &cp);
        int ok4 = (adv == 4 && cp == 0x20);
        adv = gbk_decode_next(TXT_ENC_GBK, d + 4, d + 6, &cp);
        CHECK(ok4 && adv == 2 && cp == 0x4E2D, "decode: GB18030 四字节消费不错位");
    }
    /* UTF-8「中」E4 B8 AD → U+4E2D adv 3 */
    {
        const uint8_t d[] = {0xE4, 0xB8, 0xAD};
        adv = gbk_decode_next(TXT_ENC_UTF8, d, d + 3, &cp);
        CHECK(adv == 3 && cp == 0x4E2D, "decode: UTF-8 中→U+4E2D");
    }
    /* UTF-8 窗口尾截断 → 空格 adv 1 */
    {
        const uint8_t d[] = {0xE4, 0xB8};
        adv = gbk_decode_next(TXT_ENC_UTF8, d, d + 2, &cp);
        CHECK(adv == 1 && cp == 0x20, "decode: UTF-8 截断兜底");
    }
    /* UTF-16LE 换行 0A 00 → U+000A adv 2 */
    {
        const uint8_t d[] = {0x0A, 0x00, 'x'};
        adv = gbk_decode_next(TXT_ENC_UTF16LE, d, d + 3, &cp);
        CHECK(adv == 2 && cp == 0x0A, "decode: UTF16LE 换行");
    }
    /* UTF-16LE「上」= 0A 4E → U+4E0A（低位 0x0A 不是换行——关键陷阱） */
    {
        const uint8_t d[] = {0x0A, 0x4E, 'x'};
        adv = gbk_decode_next(TXT_ENC_UTF16LE, d, d + 3, &cp);
        CHECK(adv == 2 && cp == 0x4E0A, "decode: UTF16LE 上(U+4E0A) 低位 0A 不误判");
    }
    /* UTF-16BE「上」= 4E 0A → U+4E0A */
    {
        const uint8_t d[] = {0x4E, 0x0A, 'x'};
        adv = gbk_decode_next(TXT_ENC_UTF16BE, d, d + 3, &cp);
        CHECK(adv == 2 && cp == 0x4E0A, "decode: UTF16BE 上(U+4E0A)");
    }
    /* UTF-16LE 代理对 U+1F600 = D83D DE00 → 字节 3D D8 00 DE，adv 4 */
    {
        const uint8_t d[] = {0x3D, 0xD8, 0x00, 0xDE};
        adv = gbk_decode_next(TXT_ENC_UTF16LE, d, d + 4, &cp);
        CHECK(adv == 4 && cp == 0x1F600, "decode: UTF16LE 代理对 U+1F600");
    }
    /* UTF-16LE 孤立高代理（窗口尾）→ 空格 adv 2 */
    {
        const uint8_t d[] = {0x3D, 0xD8};
        adv = gbk_decode_next(TXT_ENC_UTF16LE, d, d + 2, &cp);
        CHECK(adv == 2 && cp == 0x20, "decode: UTF16LE 孤立代理兜底");
    }
    /* UTF-16 窗口尾只剩 1 字节 → 不崩溃 */
    {
        const uint8_t d[] = {0x2D};
        adv = gbk_decode_next(TXT_ENC_UTF16LE, d, d + 1, &cp);
        CHECK(adv == 1 && cp == 0x20, "decode: UTF16 单字节尾兜底");
    }
    /* bom_size */
    CHECK(gbk_bom_size(TXT_ENC_UTF8) == 0 &&
          gbk_bom_size(TXT_ENC_UTF8_BOM) == 3 &&
          gbk_bom_size(TXT_ENC_GBK) == 0 &&
          gbk_bom_size(TXT_ENC_UTF16LE) == 2 &&
          gbk_bom_size(TXT_ENC_UTF16BE) == 2, "bom_size 各编码");
}

/* 用真实文件跑检测（可选参数：test_gbk.exe <file...>） */
static void test_real_files(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("SKIP  无法打开 %s\n", argv[i]); continue; }
        static uint8_t big[4 * 1024 * 1024];
        size_t n = fread(big, 1, sizeof(big), f);
        fclose(f);
        TxtEncoding e = detect(big, (int)n);
        const char *name = e == TXT_ENC_UTF8 ? "UTF-8" :
                           e == TXT_ENC_UTF8_BOM ? "UTF-8(BOM)" :
                           e == TXT_ENC_GBK ? "GBK" :
                           e == TXT_ENC_UTF16LE ? "UTF-16LE" : "UTF-16BE";
        printf("FILE  %-50s -> %s\n", argv[i], name);
    }
}

int main(int argc, char **argv) {
    test_detection();
    test_sniff();
    test_decode();
    test_real_files(argc, argv);
    printf("\n%d/%d passed\n", g_total - g_failed, g_total);
    return g_failed ? 1 : 0;
}
