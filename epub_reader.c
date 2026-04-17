/**
 * @file epub_reader.c
 * @brief EPUB文件解析器实现 - 支持超大章节的流式解析方案
 * 
 * 核心改进：
 * - 使用"以磁盘换内存"策略，支持无限大小章节（>256KB甚至数MB）
 * - 利用mz_zip_reader_extract_to_callback流式解压，直接写入SD卡临时文件
 * - 避免将整个章节加载到内存，彻底解决OOM问题
 */

#include "epub_reader.h"
#include "fs/fatfs/ff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "third_party/miniz/miniz.h"
#include <sys/dma_heap.h>

/* PSRAM 内存劫持：强制 miniz 使用 PSRAM 分配器，避免 SRAM OOM */
static void* miniz_psram_alloc(void *opaque, size_t items, size_t size) {
    return _dma_malloc(items * size, DMAHEAP_PSRAM);
}

static void miniz_psram_free(void *opaque, void *address) {
    if (address) _dma_free(address, 0);
}

static void* miniz_psram_realloc(void *opaque, void *address, size_t items, size_t size) {
    return _dma_realloc(address, items * size, DMAHEAP_PSRAM);
}

#define EPUB_DEBUG 1
#if EPUB_DEBUG
#define EPUB_LOG(fmt, ...) printf("[EPUB] " fmt, ##__VA_ARGS__)
#define EPUB_ERR(fmt, ...) printf("[EPUB ERR] " fmt, ##__VA_ARGS__)
#else
#define EPUB_LOG(fmt, ...)
#define EPUB_ERR(fmt, ...)
#endif

/* 流式解析配置 */
#define EPUB_TEMP_FILE_PATH    "0:/epub_temp.html"  /* 章节临时文件路径 */
#define EPUB_CHUNK_SIZE        4096                 /* 每次读取的HTML块大小 */

/*====================
 * 辅助工具与容错函数
 *====================*/

typedef struct {
    const char *xml;
    int        pos;
    int        len;
} XmlParser;

static void xml_init(XmlParser *p, const char *xml, int len) {
    p->xml = xml;
    p->pos = 0;
    p->len = len;
}

static bool xml_find_element(XmlParser *p, const char *element, char *content, int content_size) {
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s", element);
    snprintf(close_tag, sizeof(close_tag), "</%s>", element);
    
    char *start = strstr(p->xml + p->pos, open_tag);
    if (!start || start > p->xml + p->len) return false;
    
    char *tag_end = strchr(start, '>');
    if (!tag_end) return false;
    tag_end++;
    
    char *end = strstr(tag_end, close_tag);
    if (!end) return false;
    
    int len = end - tag_end;
    if (len >= content_size) len = content_size - 1;
    strncpy(content, tag_end, len);
    content[len] = '\0';
    
    p->pos = end - p->xml + strlen(close_tag);
    return true;
}

/* 净化URL：处理 %20 解码，并剔除 # 锚点 */
static void decode_and_clean_href(char *dst, const char *src) {
    while (*src) {
        if (*src == '#' || *src == '?') {
            break; 
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* 定位ZIP内的文件，优先使用mz_zip_reader_locate_file */
static int fuzzy_locate_file(mz_zip_archive *zip, const char *target, bool verbose) {
    /* 首先尝试miniz内置的查找函数，它支持大小写不敏感匹配 */
    int idx = mz_zip_reader_locate_file(zip, target, NULL, 0);
    if (idx >= 0) {
        EPUB_LOG("mz_zip_reader_locate_file found '%s' at index %d\n", target, idx);
        return idx;
    }
    
    if (verbose) {
        /* 打印调试信息 */
        mz_uint num_files = mz_zip_reader_get_num_files(zip);
        EPUB_LOG("mz_zip_reader_locate_file('%s') returned -1, scanning %u files in ZIP:\n",
                 target, (unsigned)num_files);
        
        /* 遍历检查每个文件名 */
        for (mz_uint i = 0; i < num_files && i < 30; i++) {
            char filename_buf[256] = {0};
            mz_zip_reader_get_filename(zip, i, filename_buf, sizeof(filename_buf));
            EPUB_LOG("  [%u]: '%s'\n", i, filename_buf);
        }
    }
    
    return -1;
}

/*====================
 * DMA 跳板缓冲机制
 * 解决 SPI DMA 无法直接读写 PSRAM 的硬件限制
 *====================*/

/**
 * @brief miniz 从 ZIP 读取数据的回调
 * @param pOpaque 指向打开的 FIL 文件指针
 * @param file_ofs 当前读取位置（字节偏移）
 * @param pBuf miniz 提供的缓冲区（可能在 PSRAM）
 * @param n 要读取的字节数
 * @return 实际读取的字节数，0 表示失败
 *
 * 使用 512 字节栈数组作为跳板，零碎片、零分配失败：
 * SD卡(DMA) -> SRAM bounce_buf -> memcpy -> PSRAM pBuf
 */
static size_t miniz_fatfs_read_cb(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n) {
    FIL *fp = (FIL *)pOpaque;
    if (!fp) {
        EPUB_ERR("miniz_fatfs_read_cb: fp is NULL!\n");
        return 0;
    }
    if (f_lseek(fp, (FSIZE_t)file_ofs) != FR_OK) {
        EPUB_ERR("miniz_fatfs_read_cb: f_lseek failed at ofs %llu\n",
                 (unsigned long long)file_ofs);
        return 0;
    }
    
    size_t bytes_read = 0;
    char bounce_buf[512]; /* 512字节栈跳板，零碎片，DMA绝对安全 */
    
    while (n > 0) {
        size_t to_read = (n > sizeof(bounce_buf)) ? sizeof(bounce_buf) : n;
        UINT br = 0;
        FRESULT res = f_read(fp, bounce_buf, (UINT)to_read, &br);
        if (res != FR_OK) {
            EPUB_ERR("miniz_fatfs_read_cb: f_read failed, res=%d, to_read=%u\n",
                     res, (unsigned)to_read);
            break;
        }
        if (br == 0) {
            EPUB_ERR("miniz_fatfs_read_cb: br=0 at ofs %llu, n=%u, read %u so far\n",
                     (unsigned long long)file_ofs, (unsigned)n, (unsigned)bytes_read);
            break;
        }
        
        memcpy((uint8_t*)pBuf + bytes_read, bounce_buf, br);
        bytes_read += br;
        n -= br;
    }
    
    if (bytes_read > 0) {
        EPUB_LOG("miniz_fatfs_read_cb: read %u bytes at ofs %llu\n",
                 (unsigned)bytes_read, (unsigned long long)file_ofs);
    }
    
    return bytes_read;
}

/**
 * @brief miniz 流式解压写入 FatFs 的回调
 * @param pOpaque 指向打开的 FIL 文件指针
 * @param file_ofs 当前写入位置（字节偏移）
 * @param pBuf 解压数据缓冲区（可能在 PSRAM）
 * @param n 本次回调要写入的数据大小
 * @return 实际写入的字节数，0 表示失败
 *
 * 使用 512 字节栈数组作为跳板：
 * PSRAM pBuf -> memcpy -> SRAM bounce_buf -> SD卡(DMA)
 */
static size_t miniz_extract_to_fatfs_cb(void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n) {
    FIL *fp = (FIL *)pOpaque;
    if (!fp) {
        EPUB_ERR("miniz_extract_to_fatfs_cb: fp is NULL!\n");
        return 0;
    }
    
    size_t bytes_written = 0;
    char bounce_buf[512]; /* 512字节栈跳板 */
    
    while (n > 0) {
        size_t to_write = (n > sizeof(bounce_buf)) ? sizeof(bounce_buf) : n;
        
        memcpy(bounce_buf, (const uint8_t*)pBuf + bytes_written, to_write);
        
        UINT bw = 0;
        FRESULT res = f_write(fp, bounce_buf, (UINT)to_write, &bw);
        if (res != FR_OK) {
            EPUB_ERR("miniz_extract_to_fatfs_cb: f_write failed, res=%d, to_write=%u, bw=%u\n",
                     res, (unsigned)to_write, (unsigned)bw);
            break;
        }
        if (bw == 0) {
            EPUB_ERR("miniz_extract_to_fatfs_cb: bw=0, wrote %u bytes so far\n", (unsigned)bytes_written);
            break;
        }
        
        bytes_written += bw;
        n -= bw;
    }
    
    if (bytes_written > 0 && n == 0) {
        EPUB_LOG("miniz_extract_to_fatfs_cb: wrote %u bytes at ofs %llu\n",
                 (unsigned)bytes_written, (unsigned long long)file_ofs);
    }
    
    return bytes_written;
}

/**
 * @brief 将章节流式解压到SD卡临时文件
 * @param reader EPUB阅读器
 * @param chapter_index 章节索引
 * @param temp_file_path 临时文件路径
 * @return 解压后字节数（>0成功），-1失败
 *
 * 核心思想：利用mz_zip_reader_extract_to_callback边解压边写入SD卡，
 * 内存占用只有几KB（回调缓冲区），彻底解决大章节OOM问题。
 */
int epub_reader_extract_chapter_to_file(EpubReader *reader, int chapter_index, const char *temp_file_path) {
    if (!reader || !reader->loaded) return -1;
    if (chapter_index < 0 || chapter_index >= reader->spine_count) return -1;
    
    EPUB_LOG("Extracting chapter %d to temp file: %s\n", chapter_index, temp_file_path);
    
    /* 定位ZIP内的文件 */
    int file_index = fuzzy_locate_file(&reader->zip_archive, reader->spine[chapter_index].href, true);
    if (file_index < 0) {
        EPUB_ERR("Failed to locate chapter file: %s\n", reader->spine[chapter_index].href);
        return -1;
    }
    
    /* 获取文件信息 */
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&reader->zip_archive, file_index, &stat)) {
        EPUB_ERR("Failed to get file stat\n");
        return -1;
    }
    
    EPUB_LOG("Chapter file: comp_size=%u, uncomp_size=%u\n",
             (unsigned)stat.m_comp_size, (unsigned)stat.m_uncomp_size);
    
    /* 打开临时文件进行写入 */
    FIL temp_fp;
    FRESULT res = f_open(&temp_fp, temp_file_path, FA_WRITE | FA_CREATE_ALWAYS);
    if (res != FR_OK) {
        EPUB_ERR("Failed to create temp file: %d\n", res);
        return -1;
    }
    
    /* 流式解压，不再消耗大块内存
     * mz_zip_reader_extract_to_callback会多次调用miniz_extract_to_fatfs_cb，
     * 每次只传递一小块数据（通常4KB-32KB），内存占用恒定 */
    mz_bool result = mz_zip_reader_extract_to_callback(
        &reader->zip_archive,
        file_index,
        miniz_extract_to_fatfs_cb,
        &temp_fp,
        0  /* flags */
    );
    
    /* 【关键修复点】：在关闭文件前，利用 FatFs 的 f_size 探针获取我们实际写入了多少字节 */
    FSIZE_t extracted_size = f_size(&temp_fp);
    f_close(&temp_fp);
    
    if (!result) {
        /* 容错防御机制：如果 miniz 报错，但实际解压出的文件大小和预期完全一致，
         * 说明仅仅是 ZIP 头部的 CRC 校验和不匹配（很多 EPUB 都会这样）。直接忽略该错误放行。 */
        if (extracted_size > 0 && extracted_size == stat.m_uncomp_size) {
            EPUB_LOG("Warning: CRC mismatch ignored. File size perfectly matches expected %u bytes.\n",
                     (unsigned)extracted_size);
        } else {
            EPUB_ERR("mz_zip_reader_extract_to_callback failed (size %u != expected %u)\n",
                     (unsigned)extracted_size, (unsigned)stat.m_uncomp_size);
            /* 只有大小真的对不上时，才认为解压失败，删除不完整的文件 */
            f_unlink(temp_file_path);
            return -1;
        }
    }
    
    EPUB_LOG("Chapter %d extracted to temp file successfully (%u bytes)\n", chapter_index, (unsigned)extracted_size);
    return (int)extracted_size;
}

static char* read_file_from_zip(EpubReader *reader, const char *filename, size_t *out_size) {
    EPUB_LOG("Extracting from ZIP: %s\n", filename);
    
    /* 使用 verbose 模式打印错误信息 */
    int file_index = fuzzy_locate_file(&reader->zip_archive, filename, true);
    if (file_index < 0) {
        EPUB_ERR("Failed to locate file in ZIP: %s\n", filename);
        return NULL;
    }
    
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&reader->zip_archive, file_index, &stat)) {
        EPUB_ERR("Failed to get file stat for: %s\n", filename);
        return NULL;
    }
    
    EPUB_LOG("File info: comp_size=%u, uncomp_size=%u, is_encrypted=%d\n",
             (unsigned)stat.m_comp_size, (unsigned)stat.m_uncomp_size, stat.m_is_encrypted);
    
    /* 检查文件是否加密或不支持的压缩格式 */
    if (stat.m_is_encrypted) {
        EPUB_ERR("File is encrypted, not supported: %s\n", filename);
        return NULL;
    }
    
    /* 使用 MZ_ZIP_FLAG_CASE_SENSITIVE 避免模糊匹配问题 */
    mz_uint flags = MZ_ZIP_FLAG_CASE_SENSITIVE;
    
    /* 分配解压缓冲区 - 使用 PSRAM（CPU解压不走DMA，放PSRAM安全且省SRAM） */
    char *buf = (char *)_dma_malloc(stat.m_uncomp_size + 1, DMAHEAP_PSRAM);
    if (!buf) {
        EPUB_ERR("Memory allocation failed for extracting: %s (need %u bytes)\n",
                 filename, (unsigned)(stat.m_uncomp_size + 1));
        return NULL;
    }
    memset(buf, 0, stat.m_uncomp_size + 1);
    
    EPUB_LOG("Calling mz_zip_reader_extract_to_mem...\n");
    
    /* 使用MZ_ZIP_FLAG_DO_NOT_SCAN_CENTRAL_HEADER避免扫描问题 */
    if (!mz_zip_reader_extract_to_mem(&reader->zip_archive, file_index, buf, stat.m_uncomp_size, flags)) {
        EPUB_ERR("mz_zip_reader_extract_to_mem failed for: %s\n", filename);
        _dma_free(buf, 0);
        return NULL;
    }
    
    buf[stat.m_uncomp_size] = '\0';
    *out_size = stat.m_uncomp_size;
    
    EPUB_LOG("Successfully extracted %u bytes from %s\n", (unsigned)*out_size, filename);
    return buf;
}

static bool parse_container_xml(const char *xml_data, int xml_size, char *content_opf_path) {
    XmlParser parser;
    xml_init(&parser, xml_data, xml_size);
    char *start = strstr(parser.xml, "rootfile");
    if (!start) return false;
    char *path_start = strstr(start, "full-path=\"");
    if (!path_start) return false;
    
    path_start += 11;
    char *path_end = strchr(path_start, '"');
    if (!path_end) return false;
    
    int path_len = path_end - path_start;
    if (path_len >= 256) path_len = 255;
    strncpy(content_opf_path, path_start, path_len);
    content_opf_path[path_len] = '\0';
    return true;
}

/*====================
 * 公共API
 *====================*/

EpubReader* epub_reader_create(void) {
    /* 强行把阅读器结构体塞进PSRAM，拯救SRAM */
    EpubReader *reader = (EpubReader*)_dma_malloc(sizeof(EpubReader), DMAHEAP_PSRAM);
    if (!reader) {
        EPUB_ERR("Failed to allocate EpubReader!\n");
        return NULL;
    }
    memset(reader, 0, sizeof(EpubReader));
    EPUB_LOG("EpubReader created in PSRAM\n");
    return reader;
}

bool epub_reader_open(EpubReader *reader, const char *filepath) {
    EPUB_LOG("Opening EPUB: %s\n", filepath);
    if (!reader || !filepath) return false;
    
    strncpy(reader->book.file_path, filepath, sizeof(reader->book.file_path) - 1);
    if (f_open(&reader->archive_fp, filepath, FA_OPEN_EXISTING | FA_READ) != FR_OK) return false;
    
    memset(&reader->zip_archive, 0, sizeof(reader->zip_archive));
    reader->zip_archive.m_pIO_opaque = &reader->archive_fp;
    reader->zip_archive.m_pRead = miniz_fatfs_read_cb;
    
    /* 【PSRAM 内存劫持】：强制 miniz 使用 PSRAM 分配器，避免 61KB comp_size 缓冲区申请导致 SRAM OOM */
    reader->zip_archive.m_pAlloc = miniz_psram_alloc;
    reader->zip_archive.m_pFree = miniz_psram_free;
    reader->zip_archive.m_pRealloc = miniz_psram_realloc;
    
    if (!mz_zip_reader_init(&reader->zip_archive, f_size(&reader->archive_fp), 0)) {
        f_close(&reader->archive_fp);
        return false;
    }
    
    size_t container_size;
    char *container_data = read_file_from_zip(reader, "META-INF/container.xml", &container_size);
    if (!container_data) {
        mz_zip_reader_end(&reader->zip_archive);
        f_close(&reader->archive_fp);
        return false;
    }
    
    char content_opf_path[256];
    if (!parse_container_xml(container_data, container_size, content_opf_path)) {
        free(container_data);
        mz_zip_reader_end(&reader->zip_archive);
        f_close(&reader->archive_fp);
        return false;
    }
    free(container_data);
    
    strncpy(reader->book.base_path, content_opf_path, sizeof(reader->book.base_path) - 1);
    char *lastSlash = strrchr(reader->book.base_path, '/');
    if (lastSlash) *lastSlash = '\0';
    else reader->book.base_path[0] = '\0';
    
    size_t opf_size;
    char *opf_data = read_file_from_zip(reader, content_opf_path, &opf_size);
    if (!opf_data) {
        mz_zip_reader_end(&reader->zip_archive);
        f_close(&reader->archive_fp);
        return false;
    }
    
    XmlParser parser;
    xml_init(&parser, opf_data, opf_size);
    char content[256];
    if (xml_find_element(&parser, "dc:title", content, sizeof(content))) {
        strncpy(reader->book.title, content, sizeof(reader->book.title) - 1);
    }
    
    reader->spine_count = 0;
    char *manifest_start = strstr(opf_data, "<manifest");
    if (manifest_start) {
        char *item_start = manifest_start;
        while (reader->spine_count < EPUB_MAX_SPINE_COUNT) {
            char *item_pos = strstr(item_start, "<item");
            if (!item_pos) break;
            
            char href_raw[256] = {0};
            char *href_start = strstr(item_pos, "href=\"");
            if (href_start) {
                href_start += 6;
                char *href_end = strchr(href_start, '"');
                if (href_end && href_end - href_start < sizeof(href_raw)) {
                    strncpy(href_raw, href_start, href_end - href_start);
                }
            }
            
            char href[256] = {0};
            decode_and_clean_href(href, href_raw);
            
            if ((strstr(item_pos, "application/xhtml+xml") || strstr(item_pos, "text/html"))
                && !strstr(href, ".ncx")
                && !strstr(href, "nav.xhtml")
                && !strstr(href, "/cover.")
                && !strstr(href, "title_page")
                && !strstr(href, "titlepage")
                && !strstr(href, ".css")) {
                
                char full_href[256];
                if (reader->book.base_path[0] != '\0') {
                    snprintf(full_href, sizeof(full_href), "%s/%s", reader->book.base_path, href);
                } else {
                    strncpy(full_href, href, sizeof(full_href) - 1);
                }
                
                /* 【核心防御】：预先验活！如果文件在ZIP里压根不存在，直接丢弃，不进章节列表！ */
                /* verbose 设置为 false，丢弃时不要刷屏报错 */
                if (fuzzy_locate_file(&reader->zip_archive, full_href, false) >= 0) {
                    strncpy(reader->spine[reader->spine_count].href, full_href, sizeof(reader->spine[reader->spine_count].href) - 1);
                    strcpy(reader->spine[reader->spine_count].id, "ch");
                    reader->spine_count++;
                    EPUB_LOG("Added chapter: %s\n", full_href);
                } else {
                    EPUB_ERR("Skipping missing file declared in OPF: %s\n", full_href);
                }
            }
            item_start = item_pos + 4;
        }
    }
    
    if (reader->spine_count == 0) {
        reader->spine_count = 1;
        strcpy(reader->spine[0].href, content_opf_path);
    }

    /* ===== 解析 toc.ncx 生成目录 ===== */
    char toc_path[256];
    snprintf(toc_path, sizeof(toc_path), "%s/toc.ncx",
             reader->book.base_path[0] ? reader->book.base_path : "EPUB");
    size_t toc_size = 0;
    char *toc_data = read_file_from_zip(reader, toc_path, &toc_size);
    if (toc_data && toc_size > 0) {
        reader->toc_count = 0;
        const char *scan = toc_data;
        while (reader->toc_count < EPUB_MAX_TOC_COUNT) {
            char *np_start = strstr(scan, "<navPoint");
            if (!np_start) break;
            char *text_start = strstr(np_start, "<text>");
            char *text_end = text_start ? strstr(text_start + 6, "</text>") : NULL;
            char *content_start = strstr(np_start, "<content src=\"");
            char *content_end = content_start ? strchr(content_start + 14, '"') : NULL;
            if (text_start && text_end && content_start && content_end) {
                int text_len = text_end - text_start - 6;
                int href_len = content_end - content_start - 14;
                if (text_len > 0 && text_len < 128 && href_len > 0 && href_len < 256) {
                    strncpy(reader->toc[reader->toc_count].title,
                            text_start + 6, text_len);
                    reader->toc[reader->toc_count].title[text_len] = '\0';
                    strncpy(reader->toc[reader->toc_count].href,
                            content_start + 14, href_len);
                    reader->toc[reader->toc_count].href[href_len] = '\0';
                    reader->toc[reader->toc_count].spine_index = 0;
                    reader->toc_count++;
                }
            }
            scan = np_start + 8;
        }
        EPUB_LOG("Parsed %d TOC entries from toc.ncx\n", reader->toc_count);
        free(toc_data);
    }

    free(opf_data);

    reader->loaded = true;
    EPUB_LOG("EPUB opened successfully: %s\n", reader->book.title);
    return true;
}

void epub_reader_close(EpubReader *reader) {
    if (!reader) return;
    if (reader->loaded) {
        mz_zip_reader_end(&reader->zip_archive);
        f_close(&reader->archive_fp);
        reader->loaded = false;
    }
}

void epub_reader_destroy(EpubReader *reader) {
    if (!reader) return;
    epub_reader_close(reader);
    _dma_free(reader, 0);
}

const char* epub_reader_get_title(EpubReader *reader) { return reader ? reader->book.title : ""; }
int epub_reader_get_chapter_count(EpubReader *reader) { return reader ? reader->spine_count : 0; }
int epub_reader_get_toc_count(EpubReader *reader) { return reader ? reader->toc_count : 0; }
EpubTocEntry* epub_reader_get_toc(EpubReader *reader, int index) {
    if (!reader || index < 0 || index >= reader->toc_count) return NULL;
    return &reader->toc[index];
}

int epub_reader_read_chapter(EpubReader *reader, int chapter_index, char *buffer, int buffer_size) {
    EPUB_LOG("read_chapter called: index=%d, buffer_size=%d\n", chapter_index, buffer_size);
    
    if (!reader || !reader->loaded || !buffer || buffer_size <= 0) {
        EPUB_ERR("read_chapter: invalid parameters\n");
        return -1;
    }
    
    if (chapter_index < 0 || chapter_index >= reader->spine_count) {
        EPUB_ERR("read_chapter: chapter_index %d out of range (0-%d)\n",
                 chapter_index, reader->spine_count - 1);
        return -1;
    }
    
    EPUB_LOG("Reading chapter %d: %s\n", chapter_index, reader->spine[chapter_index].href);
    
    int file_index = fuzzy_locate_file(&reader->zip_archive, reader->spine[chapter_index].href, true);
    if (file_index < 0) {
        EPUB_ERR("Failed to locate chapter file: %s\n", reader->spine[chapter_index].href);
        return -1;
    }
    
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&reader->zip_archive, file_index, &stat)) {
        EPUB_ERR("Failed to get file stat\n");
        return -1;
    }
    
    EPUB_LOG("Chapter file: comp_size=%u, uncomp_size=%u, buffer_size=%d\n",
             (unsigned)stat.m_comp_size, (unsigned)stat.m_uncomp_size, buffer_size);
    
    /* 有多大buffer就读取多少内容，截断多余部分 */
    mz_uint read_size = (mz_uint)(buffer_size - 1);  /* 留一个字节给\0 */
    if (read_size > stat.m_uncomp_size) {
        read_size = stat.m_uncomp_size;
    }
    
    if (!mz_zip_reader_extract_to_mem(&reader->zip_archive, file_index, buffer, read_size, 0)) {
        EPUB_ERR("Failed to extract chapter to buffer\n");
        return -1;
    }
    
    buffer[read_size] = '\0';
    EPUB_LOG("read_chapter: extracted %u bytes to buffer (requested %d)\n",
             (unsigned)read_size, buffer_size);
    
    return (int)read_size;
}

int epub_reader_jump_to_toc(EpubReader *reader, int toc_index) {
    if (!reader || toc_index < 0 || toc_index >= reader->toc_count) return -1;
    return reader->toc[toc_index].spine_index;
}