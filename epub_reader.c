#include "epub_reader.h"
#include "epub_xhtml_parser.h"
#include "fs/fatfs/ff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "third_party/miniz/miniz.h"
#include <sys/dma_heap.h>
#include "driver/chip/hal_dcache.h"
#include "driver/chip/psram/psram.h"
#include "compiler.h"

#define EPUB_DEBUG 1
#if EPUB_DEBUG
#define EPUB_LOG(fmt, ...) printf("[EPUB] " fmt, ##__VA_ARGS__)
#define EPUB_ERR(fmt, ...) printf("[EPUB ERR] " fmt, ##__VA_ARGS__)
#else
#define EPUB_LOG(fmt, ...)
#define EPUB_ERR(fmt, ...)
#endif

#define MZ_ZIP_LOCAL_DIR_HEADER_SIZE  30
#define MZ_ZIP_LOCAL_DIR_HEADER_SIG   0x04034b50
#define MZ_ZIP_LDH_FILENAME_LEN_OFS   26
#define MZ_ZIP_LDH_EXTRA_LEN_OFS      28

#define MAX_RAW_ENTRIES 256

typedef struct {
    char name[256];
    mz_uint64 local_header_ofs;
    mz_uint32 uncomp_size;
    mz_uint32 comp_size;
    mz_uint16 comp_method;
} RawZipEntry;

static __psram_bss RawZipEntry s_raw_entries[MAX_RAW_ENTRIES];
static int s_raw_count = 0;
static bool s_raw_built = false;

static void raw_scan_build_index(mz_zip_archive *zip) {
    if (s_raw_built) return;
    memset(s_raw_entries, 0, sizeof(s_raw_entries));
    s_raw_count = 0;

    mz_uint8 header[MZ_ZIP_LOCAL_DIR_HEADER_SIZE];
    mz_uint64 pos = 0;
    mz_uint64 archive_size = zip->m_archive_size;

    while (s_raw_count < MAX_RAW_ENTRIES && pos + 30 <= archive_size) {
        if (zip->m_pRead(zip->m_pIO_opaque, pos, header, MZ_ZIP_LOCAL_DIR_HEADER_SIZE) != MZ_ZIP_LOCAL_DIR_HEADER_SIZE)
            break;

        if (MZ_READ_LE32(header) != MZ_ZIP_LOCAL_DIR_HEADER_SIG) {
            pos++;
            continue;
        }

        mz_uint16 fn_len = MZ_READ_LE16(header + MZ_ZIP_LDH_FILENAME_LEN_OFS);
        mz_uint16 extra_len = MZ_READ_LE16(header + MZ_ZIP_LDH_EXTRA_LEN_OFS);

        RawZipEntry *e = &s_raw_entries[s_raw_count];
        e->local_header_ofs = pos;
        e->comp_method = MZ_READ_LE16(header + 8);
        e->comp_size = MZ_READ_LE32(header + 18);
        e->uncomp_size = MZ_READ_LE32(header + 22);

        if (fn_len > 0 && fn_len < 256) {
            if (zip->m_pRead(zip->m_pIO_opaque, pos + 30, e->name, fn_len) == fn_len) {
                e->name[fn_len] = '\0';
            }
        }
        if (e->name[0] == '\0')
            snprintf(e->name, sizeof(e->name), "__unnamed_%d", s_raw_count);

        s_raw_count++;

        mz_uint32 skip = fn_len + extra_len + e->comp_size;
        pos += 30 + skip;
    }

    s_raw_built = true;
    EPUB_LOG("raw_scan: found %d entries\n", s_raw_count);
}

static int raw_scan_locate_file(mz_zip_archive *zip, const char *target) {
    raw_scan_build_index(zip);
    for (int i = 0; i < s_raw_count; i++) {
        if (strcasecmp(s_raw_entries[i].name, target) == 0) {
            EPUB_LOG("raw_scan: found '%s' at raw index %d (offset 0x%x)\n",
                      target, i, (unsigned)(s_raw_entries[i].local_header_ofs));
            return i;
        }
    }
    return -1;
}

typedef struct {
    uint8_t *dst;       
    size_t remaining;   
} InflateCtx;

static size_t inflate_write_cb(const void *pBuf, size_t len, void *pUser) {
    InflateCtx *ctx = (InflateCtx *)pUser;
    size_t copy = (len < ctx->remaining) ? len : ctx->remaining;
    memcpy(ctx->dst, pBuf, copy);
    ctx->dst += copy;
    ctx->remaining -= copy;
    return copy;
}

static bool extract_raw_deflate_psram(const uint8_t *comp_buf, size_t comp_size,
                                       void *buf, mz_uint buf_size, mz_uint *out_size) {
    uint32_t buf_addr = (uint32_t)(uintptr_t)buf;
    bool is_psram = (buf_addr >= PSRAM_START_ADDR && buf_addr + buf_size <= PSRAM_END_ADDR + 1);

    EPUB_LOG("[INFLATE] comp=%p(%u) out=%p(%u) out_range=0x%x-0x%x is_psram=%d\n",
             comp_buf, comp_size, buf, buf_size,
             buf_addr, buf_addr + buf_size - 1, is_psram);

    /* Use tinfl_decompress_mem_to_callback: allocates internal 32KB SRAM dict buffer
     * for LZ77 sliding window. Callback copies decompressed chunks to final output.
     * This avoids PSRAM write-through cache coherency issue: all dict read/write
     * happens in SRAM, output is write-only from CPU perspective. */
    InflateCtx ctx;
    ctx.dst = (uint8_t *)buf;
    ctx.remaining = buf_size;

    size_t in_consumed = comp_size;
    int ret = tinfl_decompress_mem_to_callback(
        comp_buf, &in_consumed,
        inflate_write_cb, &ctx,
        0);

    if (ret) {
        *out_size = (mz_uint)(buf_size - ctx.remaining);

        if (is_psram) {
            HAL_Dcache_Clean(buf_addr, *out_size);
        }

        {
            mz_uint32 crc = mz_crc32(0, (const mz_uint8 *)buf, *out_size);
            EPUB_LOG("[INFLATE] OK: comp=%u -> uncomp=%u crc32=0x%08x\n",
                     comp_size, *out_size, crc);
            EPUB_LOG("[INFLATE] out[0..47]: ");
            for (int i = 0; i < 48 && i < (int)*out_size; i++)
                printf("%02x ", ((uint8_t *)buf)[i]);
            printf("\n");
        }
        return true;
    }

    EPUB_ERR("[INFLATE] FAILED: ret=%d (comp=%u buf=%u consumed=%u)\n",
             ret, comp_size, buf_size, (unsigned)(comp_size - in_consumed));
    return false;
}

static bool extract_file_at_raw_offset(mz_zip_archive *zip, int raw_idx, void *buf, mz_uint buf_size, mz_uint *out_size) {
    if (raw_idx < 0 || raw_idx >= s_raw_count) return false;
    RawZipEntry *e = &s_raw_entries[raw_idx];

    mz_uint8 header[MZ_ZIP_LOCAL_DIR_HEADER_SIZE];
    if (zip->m_pRead(zip->m_pIO_opaque, e->local_header_ofs, header, MZ_ZIP_LOCAL_DIR_HEADER_SIZE) != MZ_ZIP_LOCAL_DIR_HEADER_SIZE)
        return false;
    if (MZ_READ_LE32(header) != MZ_ZIP_LOCAL_DIR_HEADER_SIG) return false;

    mz_uint16 fn_len = MZ_READ_LE16(header + MZ_ZIP_LDH_FILENAME_LEN_OFS);
    mz_uint16 extra_len = MZ_READ_LE16(header + MZ_ZIP_LDH_EXTRA_LEN_OFS);
    mz_uint16 comp_method = MZ_READ_LE16(header + 8);
    mz_uint32 comp_size = MZ_READ_LE32(header + 18);
    mz_uint32 uncomp_size = MZ_READ_LE32(header + 22);

    mz_uint data_ofs = (mz_uint)(e->local_header_ofs + 30 + fn_len + extra_len);

    if (uncomp_size == 0) uncomp_size = e->uncomp_size;
    if (comp_size == 0) comp_size = e->comp_size;

    EPUB_LOG("extract_at_raw[%d]: ofs=0x%x fn_len=%u extra=%u data_ofs=0x%x comp=%u uncomp=%u\n",
              raw_idx, (unsigned)e->local_header_ofs, (unsigned)fn_len, (unsigned)extra_len,
              (unsigned)data_ofs, (unsigned)comp_size, (unsigned)uncomp_size);

    if (uncomp_size > buf_size) {
        EPUB_ERR("extract_at_raw: buffer too small (%u > %u)\n", (unsigned)uncomp_size, (unsigned)buf_size);
        return false;
    }

    uint8_t *comp_buf = (uint8_t *)_dma_malloc((size_t)comp_size, DMAHEAP_PSRAM);
    if (!comp_buf) return false;

    if (zip->m_pRead(zip->m_pIO_opaque, data_ofs, comp_buf, comp_size) != comp_size) {
        _dma_free(comp_buf, DMAHEAP_PSRAM);
        return false;
    }
    HAL_Dcache_Flush((uint32_t)comp_buf, comp_size);

    /* DEBUG: verify compressed data CRC */
    {
        mz_uint32 crc = mz_crc32(0, comp_buf, (size_t)comp_size);
        EPUB_LOG("comp_buf[%d] crc32=0x%08x\n", raw_idx, (unsigned)crc);
    }

    bool ok = false;
    if (comp_method == 0) {
        memcpy(buf, comp_buf, uncomp_size);
        *out_size = uncomp_size;
        ok = true;
    } else if (comp_method == 8) {
        ok = extract_raw_deflate_psram(comp_buf, (size_t)comp_size, buf, buf_size, out_size);
    } else {
        EPUB_ERR("extract_at_raw: unsupported compression method %u\n", (unsigned)comp_method);
    }

    _dma_free(comp_buf, DMAHEAP_PSRAM);
    return ok;
}

static size_t miniz_fatfs_read_cb(void *pOpaque, mz_uint64 file_ofs, void *pBuf, size_t n) {
    FIL *fp = (FIL *)pOpaque;
    if (!fp) { EPUB_ERR("fatfs_read: fp=NULL\n"); return 0; }
    FRESULT res = f_lseek(fp, (FSIZE_t)file_ofs);
    if (res != FR_OK) {
        EPUB_ERR("fatfs_read: f_lseek to %u failed (%d)\n", (unsigned)file_ofs, (int)res);
        return 0;
    }
    UINT br = 0;
    res = f_read(fp, pBuf, (UINT)n, &br);
    if (res != FR_OK) {
        EPUB_ERR("fatfs_read: f_read %u at %u failed (%d)\n", (unsigned)n, (unsigned)file_ofs, (int)res);
        return 0;
    }
    if (br != (UINT)n) {
        EPUB_ERR("fatfs_read: partial read at %u: want %u got %u\n",
                 (unsigned)file_ofs, (unsigned)n, (unsigned)br);
    }
    return br;
}

static void *miniz_psram_alloc(void *opaque, size_t items, size_t size) {
    (void)opaque;
    return _dma_malloc(items * size, DMAHEAP_PSRAM);
}

static void miniz_psram_free(void *opaque, void *address) {
    (void)opaque;
    if (address) _dma_free(address, DMAHEAP_PSRAM);
}

static void *miniz_psram_realloc(void *opaque, void *address, size_t items, size_t size) {
    (void)opaque;
    void *newp = _dma_malloc(items * size, DMAHEAP_PSRAM);
    if (newp && address) {
        memcpy(newp, address, items * size);
        _dma_free(address, DMAHEAP_PSRAM);
    }
    return newp;
}

static int fuzzy_locate_file(mz_zip_archive *zip, const char *target, bool verbose) {
    int raw_idx = raw_scan_locate_file(zip, target);
    if (raw_idx >= 0) {
        EPUB_LOG("Located '%s' via raw scan at raw_idx %d\n", target, raw_idx);
        return -2 - raw_idx;
    }

    /* Fallback: try miniz's central directory index (works if mz_zip_reader_init succeeded) */
    if (zip->m_zip_mode == MZ_ZIP_MODE_READING) {
        int mz_idx = mz_zip_reader_locate_file(zip, target, NULL, 0);
        if (mz_idx >= 0) {
            EPUB_LOG("Located '%s' via mz_zip_reader_locate_file at index %d\n", target, mz_idx);
            return mz_idx;
        }
    }

    if (verbose) {
        EPUB_LOG("Failed to locate '%s' in ZIP\n", target);
    }
    return -1;
}

static char* read_file_from_zip(EpubReader *reader, const char *filename, size_t *out_size) {
    int file_index = fuzzy_locate_file(&reader->zip_archive, filename, true);
    if (file_index == -1) {
        EPUB_ERR("Failed to locate file in ZIP: %s\n", filename);
        return NULL;
    }

    /* Negative return: raw scan index */
    if (file_index < -1) {
        int raw_idx = -file_index - 2;
        RawZipEntry *e = &s_raw_entries[raw_idx];
        char *buf = (char *)_dma_malloc(e->uncomp_size + 1, DMAHEAP_PSRAM);
        if (!buf) {
            EPUB_ERR("Memory allocation failed for: %s (need %u bytes)\n",
                     filename, (unsigned)(e->uncomp_size + 1));
            return NULL;
        }
        memset(buf, 0, e->uncomp_size + 1);
        mz_uint extracted_size = 0;
        if (extract_file_at_raw_offset(&reader->zip_archive, raw_idx, buf, e->uncomp_size, &extracted_size)) {
            buf[extracted_size] = '\0';
            *out_size = extracted_size;
            EPUB_LOG("Extracted '%s' via raw scan (%u bytes)\n", filename, (unsigned)extracted_size);
            return buf;
        }
        EPUB_ERR("Raw extraction failed for: %s\n", filename);
        _dma_free(buf, DMAHEAP_PSRAM);
        return NULL;
    }

    /* Non-negative return: miniz central directory index */
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&reader->zip_archive, file_index, &stat)) {
        EPUB_ERR("Failed to stat '%s' (mz idx %d)\n", filename, file_index);
        return NULL;
    }
    char *buf = (char *)_dma_malloc((size_t)stat.m_uncomp_size + 1, DMAHEAP_PSRAM);
    if (!buf) {
        EPUB_ERR("Memory allocation failed for: %s (need %u bytes)\n",
                 filename, (unsigned)(stat.m_uncomp_size + 1));
        return NULL;
    }
    if (!mz_zip_reader_extract_to_mem(&reader->zip_archive, file_index, buf, (size_t)stat.m_uncomp_size, 0)) {
        EPUB_ERR("mz extract failed for: %s\n", filename);
        _dma_free(buf, DMAHEAP_PSRAM);
        return NULL;
    }
    buf[stat.m_uncomp_size] = '\0';
    *out_size = (size_t)stat.m_uncomp_size;
    EPUB_LOG("Extracted '%s' via mz API (%u bytes)\n", filename, (unsigned)stat.m_uncomp_size);
    return buf;
}

struct container_parse_ctx {
    char *opf_path;
    int opf_path_size;
    bool found;
};

static void container_start_cb(const char *name, const char **atts, void *user_data) {
    struct container_parse_ctx *ctx = (struct container_parse_ctx *)user_data;
    if (ctx->found) return;
    if (strcmp(name, "rootfile") == 0) {
        for (int i = 0; atts && atts[i]; i += 2) {
            if (strcmp(atts[i], "full-path") == 0) {
                strncpy(ctx->opf_path, atts[i + 1], ctx->opf_path_size - 1);
                ctx->opf_path[ctx->opf_path_size - 1] = '\0';
                ctx->found = true;
                break;
            }
        }
    }
}

static bool parse_container_xml_with_expat(const char *data, int size, char *opf_path, int opf_path_size) {
    XhtmlParser *parser = xhtml_parser_create();
    if (!parser) return false;

    struct container_parse_ctx ctx;
    ctx.opf_path = opf_path;
    ctx.opf_path_size = opf_path_size;
    ctx.found = false;

    xhtml_parser_set_callbacks(parser, container_start_cb, NULL, NULL, &ctx);
    bool ok = xhtml_parser_parse(parser, data, size);
    xhtml_parser_destroy(parser);
    return ok && ctx.found;
}

struct opf_parse_ctx {
    char *title;
    int title_size;
    bool in_title;
    bool title_done;

    char base_path[256];

    EpubSpineItem *spine;
    int *spine_count;
    int max_spine;

    mz_zip_archive *zip;
    bool verbose;
};

static void opf_start_cb(const char *name, const char **atts, void *user_data) {
    struct opf_parse_ctx *ctx = (struct opf_parse_ctx *)user_data;

    if (strcmp(name, "dc:title") == 0 || strcmp(name, "title") == 0) {
        if (!ctx->title_done) {
            ctx->in_title = true;
        }
        return;
    }

    if (strcmp(name, "item") == 0) {
        char href[256] = {0};
        char media_type[64] = {0};
        for (int i = 0; atts && atts[i]; i += 2) {
            if (strcmp(atts[i], "href") == 0) {
                strncpy(href, atts[i + 1], sizeof(href) - 1);
            } else if (strcmp(atts[i], "media-type") == 0) {
                strncpy(media_type, atts[i + 1], sizeof(media_type) - 1);
            }
        }
        if (href[0] && (strstr(media_type, "xhtml+xml") || strstr(media_type, "text/html"))) {
            if (strstr(href, ".ncx") || strstr(href, "nav.xhtml") ||
                strstr(href, "/cover.") || strstr(href, "title_page") ||
                strstr(href, "titlepage") || strstr(href, ".css")) {
                return;
            }
            if (*ctx->spine_count >= ctx->max_spine) return;
            char full_href[256];
            if (ctx->base_path[0]) {
                snprintf(full_href, sizeof(full_href), "%s/%s", ctx->base_path, href);
            } else {
                strncpy(full_href, href, sizeof(full_href) - 1);
            }
            int idx = fuzzy_locate_file(ctx->zip, full_href, false);
            if (idx != -1) {
                strncpy(ctx->spine[*ctx->spine_count].href, full_href,
                        sizeof(ctx->spine[0].href) - 1);
                strcpy(ctx->spine[*ctx->spine_count].id, "ch");
                (*ctx->spine_count)++;
                EPUB_LOG("Added chapter: %s\n", full_href);
            } else {
                if (ctx->verbose)
                    EPUB_ERR("Skipping missing file: %s\n", full_href);
            }
        }
        return;
    }
}

static void opf_end_cb(const char *name, void *user_data) {
    struct opf_parse_ctx *ctx = (struct opf_parse_ctx *)user_data;
    if (strcmp(name, "dc:title") == 0 || strcmp(name, "title") == 0) {
        ctx->in_title = false;
        ctx->title_done = true;
    }
}

static void opf_char_cb(const char *data, int len, void *user_data) {
    struct opf_parse_ctx *ctx = (struct opf_parse_ctx *)user_data;
    if (ctx->in_title && !ctx->title_done) {
        int copy_len = len;
        if (copy_len > ctx->title_size - 1) copy_len = ctx->title_size - 1;
        strncpy(ctx->title, data, copy_len);
        ctx->title[copy_len] = '\0';
    }
}

static bool parse_opf_with_expat(const char *data, int size, EpubReader *reader) {
    XhtmlParser *parser = xhtml_parser_create();
    if (!parser) return false;

    struct opf_parse_ctx ctx;
    ctx.title = reader->book.title;
    ctx.title_size = sizeof(reader->book.title);
    ctx.in_title = false;
    ctx.title_done = false;
    strncpy(ctx.base_path, reader->book.base_path, sizeof(ctx.base_path) - 1);
    ctx.spine = reader->spine;
    ctx.spine_count = &reader->spine_count;
    ctx.max_spine = EPUB_MAX_SPINE_COUNT;
    ctx.zip = &reader->zip_archive;
    ctx.verbose = true;

    reader->spine_count = 0;

    xhtml_parser_set_callbacks(parser, opf_start_cb, opf_end_cb, opf_char_cb, &ctx);
    bool ok = xhtml_parser_parse(parser, data, size);
    xhtml_parser_destroy(parser);
    return ok;
}

EpubReader* epub_reader_create(void) {
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
    s_raw_built = false;
    EPUB_LOG("Opening EPUB: %s\n", filepath);
    if (!reader || !filepath) return false;

    strncpy(reader->book.file_path, filepath, sizeof(reader->book.file_path) - 1);
    if (f_open(&reader->archive_fp, filepath, FA_OPEN_EXISTING | FA_READ) != FR_OK) {
        EPUB_ERR("Failed to open file\n");
        return false;
    }

    memset(&reader->zip_archive, 0, sizeof(reader->zip_archive));
    reader->zip_archive.m_pIO_opaque = &reader->archive_fp;
    reader->zip_archive.m_pRead = miniz_fatfs_read_cb;
    reader->zip_archive.m_pAlloc = miniz_psram_alloc;
    reader->zip_archive.m_pFree = miniz_psram_free;
    reader->zip_archive.m_pRealloc = miniz_psram_realloc;

    {
        FSIZE_t archive_size = f_size(&reader->archive_fp);
        EPUB_LOG("mz_zip_reader_init: archive_size=%u bytes, pRead=%p, pIO=%p\n",
                 (unsigned)archive_size, reader->zip_archive.m_pRead, reader->zip_archive.m_pIO_opaque);

        /* Debug: read and print last 64 bytes of file to verify EOCD signature */
        {
            #define TAIL_BUF_SIZE 64
            mz_uint8 tail_buf[TAIL_BUF_SIZE];
            FSIZE_t tail_ofs = (archive_size > TAIL_BUF_SIZE) ? (archive_size - TAIL_BUF_SIZE) : 0;
            UINT tail_br = 0;
            if (f_lseek(&reader->archive_fp, tail_ofs) == FR_OK &&
                f_read(&reader->archive_fp, tail_buf, TAIL_BUF_SIZE, &tail_br) == FR_OK) {
                EPUB_LOG("File tail at ofs %u (%u bytes read):\n", (unsigned)tail_ofs, (unsigned)tail_br);
                for (int ti = 0; ti < (int)tail_br; ti++) {
                    printf("%02x ", tail_buf[ti]);
                    if ((ti + 1) % 16 == 0) printf("\n");
                }
                printf("\n");
                /* Check for EOCD signature 0x06054b50 (PK\x05\x06, little-endian) */
                for (int ti = 0; ti <= (int)tail_br - 4; ti++) {
                    if (tail_buf[ti] == 0x50 && tail_buf[ti+1] == 0x4B &&
                        tail_buf[ti+2] == 0x05 && tail_buf[ti+3] == 0x06) {
                        EPUB_LOG("EOCD signature found at tail+%d (file ofs %u)\n",
                                 ti, (unsigned)(tail_ofs + ti));
                        break;
                    }
                }
            } else {
                EPUB_ERR("Failed to read file tail for verification\n");
            }
            /* Seek back to beginning */
            f_lseek(&reader->archive_fp, 0);
            #undef TAIL_BUF_SIZE
        }

        if (!mz_zip_reader_init(&reader->zip_archive, archive_size, 0)) {
            EPUB_ERR("mz_zip_reader_init failed: size=%u, last_error=%d\n",
                     (unsigned)archive_size, (int)reader->zip_archive.m_last_error);
            f_close(&reader->archive_fp);
            return false;
        }
    }

    size_t container_size;
    char *container_data = read_file_from_zip(reader, "META-INF/container.xml", &container_size);
    if (!container_data) {
        EPUB_ERR("Failed to read container.xml\n");
        mz_zip_reader_end(&reader->zip_archive);
        f_close(&reader->archive_fp);
        return false;
    }

    char content_opf_path[256] = {0};
    if (!parse_container_xml_with_expat(container_data, container_size,
                                         content_opf_path, sizeof(content_opf_path))) {
        EPUB_ERR("Failed to parse container.xml\n");
        _dma_free(container_data, DMAHEAP_PSRAM);
        mz_zip_reader_end(&reader->zip_archive);
        f_close(&reader->archive_fp);
        return false;
    }
    _dma_free(container_data, DMAHEAP_PSRAM);
    EPUB_LOG("container.xml parsed, OPF path: %s\n", content_opf_path);

    strncpy(reader->book.base_path, content_opf_path, sizeof(reader->book.base_path) - 1);
    char *lastSlash = strrchr(reader->book.base_path, '/');
    if (lastSlash) *lastSlash = '\0';
    else reader->book.base_path[0] = '\0';

    size_t opf_size;
    char *opf_data = read_file_from_zip(reader, content_opf_path, &opf_size);
    if (!opf_data) {
        EPUB_ERR("Failed to read OPF: %s\n", content_opf_path);
        mz_zip_reader_end(&reader->zip_archive);
        f_close(&reader->archive_fp);
        return false;
    }

    if (!parse_opf_with_expat(opf_data, opf_size, reader)) {
        EPUB_ERR("Failed to parse OPF\n");
        _dma_free(opf_data, DMAHEAP_PSRAM);
        mz_zip_reader_end(&reader->zip_archive);
        f_close(&reader->archive_fp);
        return false;
    }
    _dma_free(opf_data, DMAHEAP_PSRAM);

    if (reader->spine_count == 0) {
        reader->spine_count = 1;
        strcpy(reader->spine[0].href, content_opf_path);
    }

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
        _dma_free(toc_data, DMAHEAP_PSRAM);
    }

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
    s_raw_built = false;
    s_raw_count = 0;
}

void epub_reader_destroy(EpubReader *reader) {
    if (!reader) return;
    epub_reader_close(reader);
    _dma_free(reader, DMAHEAP_PSRAM);
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
    if (!reader || !reader->loaded || !buffer || buffer_size <= 0) return -1;
    if (chapter_index < 0 || chapter_index >= reader->spine_count) return -1;

    int file_index = fuzzy_locate_file(&reader->zip_archive, reader->spine[chapter_index].href, true);
    if (file_index == -1) {
        EPUB_ERR("Failed to locate chapter file: %s\n", reader->spine[chapter_index].href);
        return -1;
    }

    if (file_index < -1) {
        int raw_idx = -file_index - 2;
        mz_uint read_size = s_raw_entries[raw_idx].uncomp_size;
        if (read_size >= (mz_uint)buffer_size) read_size = (mz_uint)(buffer_size - 1);
        mz_uint extracted = 0;
        if (!extract_file_at_raw_offset(&reader->zip_archive, raw_idx, buffer, read_size, &extracted))
            return -1;
        buffer[extracted] = '\0';
        EPUB_LOG("read_chapter: extracted %u bytes (raw)\n", (unsigned)extracted);
        return (int)extracted;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&reader->zip_archive, file_index, &stat)) {
        EPUB_ERR("Failed to get file stat\n");
        return -1;
    }

    mz_uint read_size = (mz_uint)(buffer_size - 1);
    if (read_size > stat.m_uncomp_size) read_size = stat.m_uncomp_size;

    if (!mz_zip_reader_extract_to_mem(&reader->zip_archive, file_index, buffer, read_size, 0)) {
        EPUB_ERR("Failed to extract chapter to buffer\n");
        return -1;
    }

    buffer[read_size] = '\0';
    EPUB_LOG("read_chapter: extracted %u bytes\n", (unsigned)read_size);
    return (int)read_size;
}

int epub_reader_read_chapter_full(EpubReader *reader, int chapter_index, char *out_buf, int buf_size) {
    EPUB_LOG("read_chapter_full: index=%d, buf_size=%d\n", chapter_index, buf_size);
    if (!reader || !reader->loaded || !out_buf || buf_size <= 0) return -1;
    if (chapter_index < 0 || chapter_index >= reader->spine_count) return -1;

    int file_index = fuzzy_locate_file(&reader->zip_archive, reader->spine[chapter_index].href, false);
    if (file_index == -1) {
        EPUB_ERR("Failed to locate: %s\n", reader->spine[chapter_index].href);
        return -1;
    }

    if (file_index < -1) {
        int raw_idx = -file_index - 2;
        mz_uint uncomp_size = s_raw_entries[raw_idx].uncomp_size;
        if ((int)uncomp_size > buf_size) {
            EPUB_ERR("Chapter too large: %u > %d bytes (raw)\n", (unsigned)uncomp_size, buf_size);
            return -2;
        }
        mz_uint extracted = 0;
        if (!extract_file_at_raw_offset(&reader->zip_archive, raw_idx, out_buf, uncomp_size, &extracted))
            return -1;
        out_buf[extracted] = '\0';
        EPUB_LOG("Full chapter extracted: %u bytes (raw)\n", (unsigned)extracted);
        return (int)extracted;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&reader->zip_archive, file_index, &stat)) {
        EPUB_ERR("Failed to get file stat\n");
        return -1;
    }

    if ((int)stat.m_uncomp_size > buf_size) {
        EPUB_ERR("Chapter too large: %u > %d bytes\n", (unsigned)stat.m_uncomp_size, buf_size);
        return -2;
    }

    if (!mz_zip_reader_extract_to_mem(&reader->zip_archive, file_index, out_buf, stat.m_uncomp_size, 0)) {
        EPUB_ERR("Failed to extract chapter\n");
        return -1;
    }

    out_buf[stat.m_uncomp_size] = '\0';
    EPUB_LOG("Full chapter extracted: %u bytes\n", (unsigned)stat.m_uncomp_size);
    return (int)stat.m_uncomp_size;
}

int epub_reader_jump_to_toc(EpubReader *reader, int toc_index) {
    if (!reader || toc_index < 0 || toc_index >= reader->toc_count) return -1;
    return reader->toc[toc_index].spine_index;
}
