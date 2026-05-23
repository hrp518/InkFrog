/**
 * @file settings_storage.c
 * @brief 基于INI格式的持久化存储实现
 * 
 * INI格式解析/写入，基于FatFs文件系统
 * 写入策略: 读全部→内存修改→写临时文件→rename覆盖，防断电损坏
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "settings_storage.h"
#include "fs/fatfs/ff.h"

#define SETTINGS_FILE       "0:/settings.ini"
#define SETTINGS_TMP_FILE   "0:/settings.tmp"

/* ==================== 内部辅助函数 ==================== */

/* 去除行尾的 \r\n */
static void strip_crlf(char *line)
{
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
}

/* 判断一行是否是section头，如 "[bookmark:/xxx.epub]" */
static bool is_section_header(const char *line, const char *section)
{
    /* 跳过前导空白 */
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '[') return false;
    line++; /* 跳过 '[' */

    /* 构造期望的section头: "[section]" */
    int slen = strlen(section);
    if (strncmp(line, section, slen) != 0) return false;
    line += slen;
    if (*line != ']') return false;
    return true;
}

/* 判断一行是否以 key= 开头 */
static bool is_key_line(const char *line, const char *key)
{
    while (*line == ' ' || *line == '\t') line++;
    int klen = strlen(key);
    if (strncmp(line, key, klen) != 0) return false;
    return (line[klen] == '=');
}

/* 从 key=value 行提取value部分 */
static void extract_value(const char *line, char *value_out, int value_size)
{
    while (*line == ' ' || *line == '\t') line++;
    const char *eq = strchr(line, '=');
    if (!eq) { value_out[0] = '\0'; return; }
    eq++; /* 跳过 '=' */
    while (*eq == ' ') eq++;
    strncpy(value_out, eq, value_size - 1);
    value_out[value_size - 1] = '\0';
    strip_crlf(value_out);
}

/* 读取整个INI文件到内存缓冲区 */
static int read_ini_file(char *buf, int buf_size)
{
    FIL fp;
    FRESULT res;
    UINT br;

    res = f_open(&fp, SETTINGS_FILE, FA_READ);
    if (res != FR_OK) {
        /* 文件不存在是正常情况 */
        buf[0] = '\0';
        return 0;
    }

    res = f_read(&fp, buf, buf_size - 1, &br);
    f_close(&fp);
    if (res != FR_OK) {
        buf[0] = '\0';
        return -1;
    }
    buf[br] = '\0';
    return (int)br;
}

/* 写入整个INI文件（先写临时文件再rename） */
static int write_ini_file(const char *buf, int len)
{
    FIL fp;
    FRESULT res;
    UINT bw;

    /* 1. 写临时文件 */
    res = f_open(&fp, SETTINGS_TMP_FILE, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        printf("[SETTINGS] ERR: create tmp failed, res=%d\n", res);
        return -1;
    }

    res = f_write(&fp, buf, len, &bw);
    f_close(&fp);
    if (res != FR_OK || (int)bw != len) {
        printf("[SETTINGS] ERR: write tmp failed, res=%d bw=%u len=%d\n",
               res, (unsigned)bw, len);
        f_unlink(SETTINGS_TMP_FILE);
        return -1;
    }

    /* 2. 删除旧文件（如果存在） */
    f_unlink(SETTINGS_FILE);

    /* 3. rename临时文件为正式文件 */
    res = f_rename(SETTINGS_TMP_FILE, SETTINGS_FILE + 3); /* 跳过 "0:/" */
    if (res != FR_OK) {
        /* FatFs的f_rename路径不需要逻辑驱动器号前缀 */
        /* 再试一次带前缀的 */
        res = f_rename(SETTINGS_TMP_FILE, SETTINGS_FILE);
    }
    if (res != FR_OK) {
        printf("[SETTINGS] ERR: rename failed, res=%d\n", res);
        return -1;
    }

    return 0;
}

/* ==================== 行迭代器 ==================== */

/* 获取下一行的起始位置和长度 */
static const char *next_line(const char *p, const char *line, int *line_len)
{
    const char *start = p;
    while (*p && *p != '\n') p++;
    *line_len = (int)(p - start);
    if (*p == '\n') p++; /* 跳过 \n */
    /* 复制到临时缓冲区（去除\r\n） */
    if (*line_len > 0) {
        memcpy((char*)line, start, *line_len);
        ((char*)line)[*line_len] = '\0';
    } else {
        ((char*)line)[0] = '\0';
    }
    return p;
}

/* ==================== 公共接口实现 ==================== */

int settings_save_bookmark(const char *filepath, int chapter, int offset)
{
    /* 构造section名: "bookmark:/xxx.epub" */
    char section[SETTINGS_MAX_SECTION];
    snprintf(section, sizeof(section), "bookmark:%s", filepath ? filepath : "");

    char val_buf[32];

    /* 保存chapter */
    snprintf(val_buf, sizeof(val_buf), "%d", chapter);
    if (settings_set_string(section, "chapter", val_buf) != 0) return -1;

    /* 保存offset */
    snprintf(val_buf, sizeof(val_buf), "%d", offset);
    if (settings_set_string(section, "offset", val_buf) != 0) return -1;

    return 0;
}

int settings_load_bookmark(const char *filepath, int *chapter_out, int *offset_out)
{
    char section[SETTINGS_MAX_SECTION];
    snprintf(section, sizeof(section), "bookmark:%s", filepath ? filepath : "");

    char val[32];

    if (settings_get_string(section, "chapter", val, sizeof(val)) != 0) return -1;
    *chapter_out = atoi(val);

    if (settings_get_string(section, "offset", val, sizeof(val)) != 0) return -1;
    *offset_out = atoi(val);

    printf("[SETTINGS] Loaded bookmark: %s ch=%d off=%d\n", filepath, *chapter_out, *offset_out);
    return 0;
}

int settings_set_string(const char *section, const char *key, const char *value)
{
    static char ini_buf[SETTINGS_MAX_FILE_SIZE] __attribute__((section(".psram_bss")));
    static char out_buf[SETTINGS_MAX_FILE_SIZE] __attribute__((section(".psram_bss")));
    char line_buf[SETTINGS_MAX_PATH + 64];

    /* 1. 读取现有INI文件 */
    int ini_len = read_ini_file(ini_buf, sizeof(ini_buf));

    /* 2. 解析并重建 */
    const char *p = ini_buf;
    int out_pos = 0;
    bool section_found = false;
    bool key_written = false;
    bool in_target_section = false;

    #define APPEND_OUT(fmt, ...) do { \
        out_pos += snprintf(out_buf + out_pos, sizeof(out_buf) - out_pos, fmt, ##__VA_ARGS__); \
        if (out_pos >= (int)sizeof(out_buf) - 2) { printf("[SETTINGS] ERR: out_buf overflow\n"); return -1; } \
    } while(0)

    while (*p) {
        int llen;
        p = next_line(p, line_buf, &llen);
        strip_crlf(line_buf);

        /* 空行 */
        if (line_buf[0] == '\0') {
            APPEND_OUT("\n");
            continue;
        }

        /* 注释行（以;或#开头） */
        if (line_buf[0] == ';' || line_buf[0] == '#') {
            APPEND_OUT("%s\n", line_buf);
            continue;
        }

        /* section头 */
        if (line_buf[0] == '[') {
            /* 离开当前section前，如果target section已找到但key没写，现在写 */
            if (in_target_section && !key_written) {
                APPEND_OUT("%s=%s\n", key, value);
                key_written = true;
            }
            in_target_section = is_section_header(line_buf, section);
            if (in_target_section) section_found = true;
            APPEND_OUT("%s\n", line_buf);
            continue;
        }

        /* key=value行 */
        if (in_target_section && is_key_line(line_buf, key)) {
            /* 替换为目标值 */
            APPEND_OUT("%s=%s\n", key, value);
            key_written = true;
            continue;
        }

        /* 其他行直接保留 */
        APPEND_OUT("%s\n", line_buf);
    }

    /* 如果target section存在但key没写（section为空或没有这个key） */
    if (section_found && !key_written) {
        APPEND_OUT("%s=%s\n", key, value);
        key_written = true;
    }

    /* 如果target section不存在，追加新section */
    if (!section_found) {
        APPEND_OUT("[%s]\n", section);
        APPEND_OUT("%s=%s\n", key, value);
    }

    #undef APPEND_OUT

    /* 3. 写回文件 */
    if (write_ini_file(out_buf, out_pos) != 0) {
        printf("[SETTINGS] ERR: write_ini_file failed\n");
        return -1;
    }

    return 0;
}

int settings_get_string(const char *section, const char *key, char *value_out, int value_size)
{
    static char ini_buf[SETTINGS_MAX_FILE_SIZE] __attribute__((section(".psram_bss")));
    char line_buf[SETTINGS_MAX_PATH + 64];

    /* 1. 读取INI文件 */
    int ini_len = read_ini_file(ini_buf, sizeof(ini_buf));
    if (ini_len <= 0) return -1;

    /* 2. 逐行扫描 */
    const char *p = ini_buf;
    bool in_target_section = false;

    while (*p) {
        int llen;
        p = next_line(p, line_buf, &llen);
        strip_crlf(line_buf);

        /* 空行或注释 */
        if (line_buf[0] == '\0' || line_buf[0] == ';' || line_buf[0] == '#') continue;

        /* section头 */
        if (line_buf[0] == '[') {
            in_target_section = is_section_header(line_buf, section);
            continue;
        }

        /* key=value行 */
        if (in_target_section && is_key_line(line_buf, key)) {
            extract_value(line_buf, value_out, value_size);
            return 0;
        }
    }

    /* 未找到 */
    return -1;
}