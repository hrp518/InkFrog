/**
 * @file gbk.h
 * @brief GBK/GB2312 编码检测与解码
 *
 * 为 txt_viewer 提供：
 * - 编码自动检测（UTF-8 BOM / UTF-8 合法性 / GBK 兜底）
 * - GBK 双字节 → Unicode 解码
 * - UTF-8 合法性校验
 *
 * 设计要点：
 *   \n (0x0A) 在 ASCII / UTF-8 / GBK 中都是单字节，且绝不会出现在 GBK
 *   lead(0x81-0xFE)/trail(0x40-0x7E ∪ 0x80-0xFE) 字节中，
 *   因此按原始文件字节偏移做换行对齐对所有编码都安全。
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
    TXT_ENC_UTF8,       /* 无 BOM 的 UTF-8（或检测为合法 UTF-8） */
    TXT_ENC_UTF8_BOM,   /* 带 EF BB BF BOM 的 UTF-8 */
    TXT_ENC_GBK,        /* GBK/GB2313/GB18030 双字节 */
} TxtEncoding;

/*====================
 *   编码检测
 *====================*/

/**
 * @brief 检测文件编码
 *
 * 读取文件前 4KB：
 *   - 首 3 字节为 EF BB BF → UTF-8 BOM
 *   - 否则对前 4KB 跑 UTF-8 合法性校验，全程合法 → UTF-8
 *   - 否则 → GBK
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
 * (10xxxxxx) 的搭配、最短编码形式。出现任何非法序列即返回 false。
 *
 * @param data 字节缓冲区
 * @param len  长度
 * @return true 合法 UTF-8，false 含非法序列
 */
bool gbk_is_valid_utf8(const uint8_t *data, int len);

/*====================
 *   字符解码（统一接口，UTF-8/GBK 通用）
 *====================*/

/**
 * @brief 从原始字节流解码下一个字符（UTF-8 或 GBK 通用接口）
 *
 * 返回该字符在原始字节流中占用的字节数，并把 Unicode 码点写入 *uni_out。
 * 非法字节统一兜底为 U+0020（空格），adv=1，绝不崩溃。
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
 * 否则 lead+trail 双字节查表，adv=2；非法兜底空格 adv=1
 */
int gbk_decode_char(const uint8_t *p, const uint8_t *end, uint32_t *uni_out);

#ifdef __cplusplus
}
#endif

#endif /* GBK_H */
