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

/* ==================== UTF-8 校验 ==================== */

bool gbk_is_valid_utf8(const uint8_t *data, int len) {
    if (!data || len <= 0) return true;
    int i = 0;
    while (i < len) {
        uint8_t b = data[i];
        int need;       /* 后续 continuation 字节数 */
        uint32_t min_cp;/* 该长度下最小合法码点（用于拒绝非最短形式） */

        if (b <= 0x7F) {
            i++;
            continue;
        } else if ((b & 0xE0) == 0xC0) {
            need = 1; min_cp = 0x80;
        } else if ((b & 0xF0) == 0xE0) {
            need = 2; min_cp = 0x800;
        } else if ((b & 0xF8) == 0xF0) {
            need = 3; min_cp = 0x10000;
        } else {
            /* 孤立的 continuation 字节或非法前导 */
            return false;
        }
        if (i + need >= len) return false;  /* 截断 */

        uint32_t cp;
        if (need == 1) {
            cp = (uint32_t)(b & 0x1F);
        } else if (need == 2) {
            cp = (uint32_t)(b & 0x0F);
        } else {
            cp = (uint32_t)(b & 0x07);
        }

        for (int k = 1; k <= need; k++) {
            uint8_t cb = data[i + k];
            if ((cb & 0xC0) != 0x80) return false;  /* continuation 必须 10xxxxxx */
            cp = (cp << 6) | (cb & 0x3F);
        }
        if (cp < min_cp) return false;          /* 非最短形式 */
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;  /* UTF-16 代理区 */
        if (cp > 0x10FFFF) return false;        /* 超出 Unicode 上限 */

        i += 1 + need;
    }
    return true;
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
    return (enc == TXT_ENC_UTF8_BOM) ? 3 : 0;
}

TxtEncoding gbk_detect_encoding(FIL *fp) {
    if (!fp) return TXT_ENC_GBK;

    /* 读前 4KB 用于 BOM 嗅探 + UTF-8 合法性校验 */
    static uint8_t sniff[4096] __attribute__((section(".psram_bss")));
    UINT br = 0;
    FRESULT res;

    f_lseek(fp, 0);
    res = f_read(fp, sniff, sizeof(sniff), &br);
    f_lseek(fp, 0);

    if (res != FR_OK || br == 0) {
        return TXT_ENC_GBK;
    }

    /* UTF-8 BOM */
    if (br >= 3 && sniff[0] == 0xEF && sniff[1] == 0xBB && sniff[2] == 0xBF) {
        return TXT_ENC_UTF8_BOM;
    }

    /* UTF-16 BOM：不在支持范围，按 GBK 兜底（中文场景 GBK 更可能） */
    if (br >= 2 && sniff[0] == 0xFF && sniff[1] == 0xFE) {
        return TXT_ENC_GBK;
    }
    if (br >= 2 && sniff[0] == 0xFE && sniff[1] == 0xFF) {
        return TXT_ENC_GBK;
    }

    /* UTF-8 合法性校验：全程合法则判为 UTF-8，否则 GBK */
    if (gbk_is_valid_utf8(sniff, (int)br)) {
        return TXT_ENC_UTF8;
    }
    return TXT_ENC_GBK;
}
