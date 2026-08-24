/**
 * @file gbk.c
 * @brief GBK 编码检测与解码实现
 *
 * gbk_to_uni 映射表由 tools/gen_gbk_table.py 自动生成到 gbk_table.c。
 *
 * GBK 双字节索引规则（与生成器一致）：
 *   lead byte  : 0x81 .. 0xFE
 *   trail byte : 0x40 .. 0x7E ∪ 0x80 .. 0xFE  (跳过 0x7F)
 *   trail_idx  : 0x40..0x7E -> trail - 0x40          (0..62)
 *                0x80..0xFE -> trail - 0x80 + 63     (63..189)
 *   flat idx   : (lead - 0x81) * 190 + trail_idx
 */

#include "gbk.h"
#include <stdio.h>

/* 自动生成的 GBK -> Unicode 表（const，落 flash .rodata，约 47KB）。
 * 用 .inc 后缀避免被 Makefile 的 *.c glob 当成独立翻译单元编译（否则
 * 与本文件 include 的副本造成 g_gbk_to_uni 重复定义）。 */
#include "gbk_table.inc"

/* ==================== GBK 索引计算 ==================== */

/* trail byte -> 0..189，返回 -1 表示非法 trail（0x7F / <0x40 / >0xFE） */
static int gbk_trail_index(uint8_t trail) {
    if (trail >= 0x40 && trail <= 0x7E) return trail - 0x40;          /* 0..62 */
    if (trail >= 0x80 && trail <= 0xFE) return trail - 0x80 + 63;     /* 63..189 */
    return -1;
}

/* ==================== GBK 单字符解码 ==================== */

int gbk_decode_char(const uint8_t *p, const uint8_t *end, uint32_t *uni_out) {
    if (!p || p >= end) {
        if (uni_out) *uni_out = 0x20;
        return 0;
    }

    uint8_t lead = p[0];

    /* ASCII 区：单字节 */
    if (lead < 0x80) {
        if (uni_out) *uni_out = lead;
        return 1;
    }

    /* 双字节 GBK：需要 lead + trail */
    if (lead < 0x81 || lead > 0xFE) {
        /* 非法 lead（0x80），兜底空格前进 1 */
        if (uni_out) *uni_out = 0x20;
        return 1;
    }
    if (p + 1 >= end) {
        /* 末尾孤立 lead，兜底空格前进 1 */
        if (uni_out) *uni_out = 0x20;
        return 1;
    }

    uint8_t trail = p[1];

    /* GB18030 四字节序列（lead + 0x30-0x39 开头）：无映射表，整组按
     * 4 字节消费兜底空格，防止把后续字节当新字符导致整段错位 */
    if (trail >= 0x30 && trail <= 0x39) {
        if (uni_out) *uni_out = 0x20;
        return (end - p >= 4) ? 4 : (int)(end - p);
    }

    int ti = gbk_trail_index(trail);
    if (ti < 0) {
        /* trail 非法（含 0x7F），仅前进 1（lead 单独非法） */
        if (uni_out) *uni_out = 0x20;
        return 1;
    }

    int idx = (lead - 0x81) * GBK_TRAIL_COUNT + ti;
    if (idx < 0 || idx >= GBK_TABLE_TOTAL) {
        if (uni_out) *uni_out = 0x20;
        return 1;
    }

    uint16_t cp = g_gbk_to_uni[idx];
    if (cp == 0) {
        /* 表中未定义，兜底空格 */
        if (uni_out) *uni_out = 0x20;
        return 2;  /* 仍是合法双字节结构，按 2 前进避免错位 */
    }
    if (uni_out) *uni_out = cp;
    return 2;
}

/* ==================== 流式 UTF-8 嗅探状态机 ==================== */

/* 关键语义：块尾不完整的多字节序列保持 pending，不判非法——采样窗口
 * 截断在字符中间是常态（概率约 2/3，纯中文 UTF-8），旧实现把它当
 * 非法序列导致 UTF-8 文件被误判 GBK。残缺尾巴留给下一块或最终忽略。 */

void txt_sniff_reset(TxtSniffState *st) {
    if (!st) return;
    st->need = 0;
    st->saw_multibyte = 0;
    st->utf8_broken = 0;
    st->cp = 0;
    st->min_cp = 0;
}

void txt_sniff_feed(TxtSniffState *st, const uint8_t *data, int len) {
    if (!st || !data || len <= 0 || st->utf8_broken) return;

    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (st->need == 0) {
            if (b <= 0x7F) continue;
            st->saw_multibyte = 1;
            if ((b & 0xE0) == 0xC0) {
                st->need = 1; st->min_cp = 0x80;    st->cp = b & 0x1F;
            } else if ((b & 0xF0) == 0xE0) {
                st->need = 2; st->min_cp = 0x800;   st->cp = b & 0x0F;
            } else if ((b & 0xF8) == 0xF0) {
                st->need = 3; st->min_cp = 0x10000; st->cp = b & 0x07;
            } else {
                st->utf8_broken = 1;   /* 孤立 continuation / 非法前导 */
                return;
            }
        } else {
            if ((b & 0xC0) != 0x80) {  /* continuation 必须 10xxxxxx */
                st->utf8_broken = 1;
                return;
            }
            st->cp = (st->cp << 6) | (b & 0x3F);
            if (--st->need == 0) {
                if (st->cp < st->min_cp) { st->utf8_broken = 1; return; }         /* 非最短形式 */
                if (st->cp >= 0xD800 && st->cp <= 0xDFFF) { st->utf8_broken = 1; return; } /* 代理区 */
                if (st->cp > 0x10FFFF) { st->utf8_broken = 1; return; }           /* 超上限 */
            }
        }
    }
}

int txt_sniff_decide(const TxtSniffState *st) {
    if (!st) return TXT_SNIFF_GBK;
    return st->utf8_broken ? TXT_SNIFF_GBK : TXT_SNIFF_UTF8;
}

/* ==================== UTF-8 校验 ==================== */

bool gbk_is_valid_utf8(const uint8_t *data, int len) {
    if (!data || len <= 0) return true;
    TxtSniffState st;
    txt_sniff_reset(&st);
    txt_sniff_feed(&st, data, len);
    return !st.utf8_broken;
}

/* ==================== 统一解码接口 ==================== */

int gbk_decode_next(TxtEncoding enc, const uint8_t *p, const uint8_t *end,
                    uint32_t *uni_out) {
    if (!p || p >= end) {
        if (uni_out) *uni_out = 0x20;
        return 0;
    }

    if (enc == TXT_ENC_GBK) {
        return gbk_decode_char(p, end, uni_out);
    }

    /* UTF-16 路径：BMP 2 字节直取，代理对组合 4 字节，孤立代理兜底空格 */
    if (enc == TXT_ENC_UTF16LE || enc == TXT_ENC_UTF16BE) {
        if (end - p < 2) {
            if (uni_out) *uni_out = 0x20;
            return 1;
        }
        uint32_t cp = (enc == TXT_ENC_UTF16LE)
                      ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8))
                      : (((uint32_t)p[0] << 8) | (uint32_t)p[1]);
        if (cp >= 0xD800 && cp <= 0xDBFF && (end - p) >= 4) {
            uint32_t lo = (enc == TXT_ENC_UTF16LE)
                          ? ((uint32_t)p[2] | ((uint32_t)p[3] << 8))
                          : (((uint32_t)p[2] << 8) | (uint32_t)p[3]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                if (uni_out) *uni_out = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                return 4;
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            /* 孤立代理（或窗口尾截断的代理对前半）：兜底空格按 2 前进 */
            if (uni_out) *uni_out = 0x20;
            return 2;
        }
        if (uni_out) *uni_out = cp;
        return 2;
    }

    /* UTF-8 / UTF-8 BOM 路径：复用合法 UTF-8 解码，非法兜底空格 */
    uint8_t b = p[0];
    if (b < 0x80) {
        if (uni_out) *uni_out = b;
        return 1;
    }
    int need = 0;
    if ((b & 0xE0) == 0xC0) need = 1;
    else if ((b & 0xF0) == 0xE0) need = 2;
    else if ((b & 0xF8) == 0xF0) need = 3;

    if (need == 0 || p + 1 + need > end) {
        /* 非法前导或截断：兜底空格，前进 1 */
        if (uni_out) *uni_out = 0x20;
        return 1;
    }

    /* 校验 continuation 字节 */
    for (int k = 1; k <= need; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            if (uni_out) *uni_out = 0x20;
            return 1;
        }
    }

    uint32_t cp;
    if (need == 1) cp = ((uint32_t)(b & 0x1F) << 6) | (p[1] & 0x3F);
    else if (need == 2) cp = ((uint32_t)(b & 0x0F) << 12) |
                            ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else cp = ((uint32_t)(b & 0x07) << 18) |
              ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);

    if (uni_out) *uni_out = cp;
    return 1 + need;
}

/* ==================== 编码检测 ==================== */

int gbk_bom_size(TxtEncoding enc) {
    if (enc == TXT_ENC_UTF8_BOM) return 3;
    if (enc == TXT_ENC_UTF16LE || enc == TXT_ENC_UTF16BE) return 2;
    return 0;
}

#define SNIFF_CHUNK      4096
#define SNIFF_MAX_BYTES  (256 * 1024)   /* 采样上限：再长仍纯 ASCII 则按 UTF-8（解码等价） */

TxtEncoding gbk_detect_encoding(FIL *fp) {
    if (!fp) return TXT_ENC_GBK;

    /* 分块采样缓冲（.psram_bss，与 txt_viewer 的 32KB 窗口不同时使用） */
    static uint8_t sniff[SNIFF_CHUNK] __attribute__((section(".psram_bss")));
    TxtSniffState st;
    txt_sniff_reset(&st);

    if (f_lseek(fp, 0) != FR_OK) return TXT_ENC_GBK;

    int total = 0;
    int first_chunk = 1;
    for (;;) {
        UINT br = 0;
        if (f_read(fp, sniff, SNIFF_CHUNK, &br) != FR_OK || br == 0) break;

        if (first_chunk) {
            first_chunk = 0;
            if (br >= 3 && sniff[0] == 0xEF && sniff[1] == 0xBB && sniff[2] == 0xBF) {
                f_lseek(fp, 0);
                return TXT_ENC_UTF8_BOM;
            }
            if (br >= 2 && sniff[0] == 0xFF && sniff[1] == 0xFE) {
                f_lseek(fp, 0);
                return TXT_ENC_UTF16LE;
            }
            if (br >= 2 && sniff[0] == 0xFE && sniff[1] == 0xFF) {
                f_lseek(fp, 0);
                return TXT_ENC_UTF16BE;
            }
        }

        txt_sniff_feed(&st, sniff, (int)br);
        total += (int)br;

        if (st.utf8_broken) break;              /* 已定案 GBK，终止采样省 IO */
        if (br < SNIFF_CHUNK) break;            /* EOF */
        if (total >= SNIFF_MAX_BYTES) break;    /* 采样上限 */
    }

    f_lseek(fp, 0);
    return st.utf8_broken ? TXT_ENC_GBK : TXT_ENC_UTF8;
}
