/**
 * @file gbk.h
 * @brief TXT 编码检测与解码（UTF-8 / GBK / UTF-16）
 *
 * 为 txt_viewer 提供：
 * - 编码自动检测（BOM 嗅探 + 分块流式 UTF-8 校验 + GBK 兜底）
 * - GBK 双字节 → Unicode 解码
 * - UTF-16LE/BE → Unicode 解码（含代理对）
 * - UTF-8 合法性校验（流式状态机，可在主机单测）
 *
 * 设计要点：
 *   \n (0x0A) 在 ASCII / UTF-8 / GBK 中都是单字节，且绝不会出现在 GBK
 *   lead(0x81-0xFE)/trail(0x40-0x7E ∪ 0x80-0xFE) 字节中，
 *   因此对这三种编码按原始文件字节偏移做换行对齐安全。
 *   UTF-16 的换行是 0A 00 / 00 0A 双字节，且某汉字（如「上」U+4E0A）
 *   的低位字节恰为 0x0A —— 换行判断必须走解码值，偏移需按 2 字节码元对齐。
 */

#ifndef GBK_H
#define GBK_H

#include <stdint.h>
#include <stdbool.h>
#include "fs/fatfs/ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/*====================
 *   编码类型
 *====================*/

typedef enum {
    TXT_ENC_UTF8,       /* 无 BOM 的 UTF-8（或检测为合法 UTF-8；含纯 ASCII） */
    TXT_ENC_UTF8_BOM,   /* 带 EF BB BF BOM 的 UTF-8 */
    TXT_ENC_GBK,        /* GBK/GB2312/GB18030 双字节 */
    TXT_ENC_UTF16LE,    /* 带 FF FE BOM 的 UTF-16 小端 */
    TXT_ENC_UTF16BE,    /* 带 FE FF BOM 的 UTF-16 大端 */
} TxtEncoding;

/*====================
 *   流式 UTF-8 嗅探（纯函数，无 IO，可主机单测）
 *====================*/

typedef struct {
    int      need;          /* 当前多字节序列还差的 continuation 字节数（0~3） */
    int      saw_multibyte; /* 采样中出现过非 ASCII 多字节序列 */
    int      utf8_broken;   /* 出现非法 UTF-8 序列（判 GBK 的唯一依据） */
    uint32_t cp;            /* 正在拼装的码点 */
    uint32_t min_cp;        /* 当前序列长度的最小合法码点（拒绝超短形式） */
} TxtSniffState;

void txt_sniff_reset(TxtSniffState *st);
void txt_sniff_feed(TxtSniffState *st, const uint8_t *data, int len);

#define TXT_SNIFF_UTF8 1   /* 全程合法 UTF-8（纯 ASCII 也归此类，解码等价） */
#define TXT_SNIFF_GBK  0   /* 出现非法序列，判 GBK */

int txt_sniff_decide(const TxtSniffState *st);

/*====================
 *   编码检测
 *====================*/

/**
 * @brief 检测文件编码（分块流式采样）
 *
 * 首块先做 BOM 嗅探：EF BB BF → UTF-8 BOM；FF FE → UTF-16LE；FE FF → UTF-16BE。
 * 无 BOM 则分块（4KB）流式喂给 UTF-8 嗅探状态机：
 *   - 出现任何非法 UTF-8 序列 → 立即判 GBK（终止采样省 IO）
 *   - 读到 EOF 或累计 256KB 仍全程合法 → UTF-8
 * 采样块尾不完整的多字节尾巴不参与判定（避免窗口截断误判），
 * 跨块序列由状态机自然衔接（等价 3 字节 carry）。
 *
 * @param fp 已打开的 FIL（函数内会 seek 回 0）
 * @return 检测到的编码
 */
TxtEncoding gbk_detect_encoding(FIL *fp);

/**
 * @brief 获取编码对应的 BOM 字节数（用于跳过 BOM 计算有效偏移）
 */
int gbk_bom_size(TxtEncoding enc);

/*====================
 *   UTF-8 校验
 *====================*/

/**
 * @brief 校验一段字节是否是合法的 UTF-8 文本
 *
 * 严格按 UTF-8 多字节规则检查：前导字节与 continuation 字节
 * (10xxxxxx) 的搭配、最短编码形式、代理区、码点上限。
 * 缓冲区尾部不完整的序列（可能被截断）不视为非法。
 *
 * @param data 字节缓冲区
 * @param len  长度
 * @return true 合法 UTF-8，false 含非法序列
 */
bool gbk_is_valid_utf8(const uint8_t *data, int len);

/*====================
 *   字符解码（统一接口，各编码通用）
 *====================*/

/**
 * @brief 从原始字节流解码下一个字符（UTF-8 / GBK / UTF-16 通用接口）
 *
 * 返回该字符在原始字节流中占用的字节数，并把 Unicode 码点写入 *uni_out。
 * UTF-16 支持 BMP 直取与代理对组合（4 字节），孤立代理兜底空格。
 * 非法字节统一兜底为 U+0020（空格），adv≥1，绝不崩溃、绝不返回 0（p<end 时）。
 *
 * @param enc     编码类型
 * @param p       当前指针
 * @param end     缓冲区结束指针（不含）
 * @param uni_out 输出 Unicode 码点
 * @return int    本字符占用的原始字节数（1~4）
 */
int gbk_decode_next(TxtEncoding enc, const uint8_t *p, const uint8_t *end,
                    uint32_t *uni_out);

/**
 * @brief GBK 单字符解码（暴露给需要按编码分支调用的场合）
 *
 * lead < 0x80 → ASCII，adv=1
 * lead+trail 双字节查表，adv=2；非法兜底空格 adv=1
 * GB18030 四字节序列（trail 0x30-0x39 开头）整组按 4 字节消费兜底空格，防错位
 */
int gbk_decode_char(const uint8_t *p, const uint8_t *end, uint32_t *uni_out);

#ifdef __cplusplus
}
#endif

#endif /* GBK_H */
