#include "epub_xhtml_parser.h"
#include "third_party/expat/expat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sys/sys_heap.h"

#define XHTML_PARSER_DEBUG 1
#if XHTML_PARSER_DEBUG
#define XHTML_LOG(fmt, ...) printf("[XHTML] " fmt, ##__VA_ARGS__)
#define XHTML_ERR(fmt, ...) printf("[XHTML ERR] " fmt, ##__VA_ARGS__)
#else
#define XHTML_LOG(fmt, ...)
#define XHTML_ERR(fmt, ...)
#endif

struct XhtmlParser {
    XML_Parser expat_parser;
    xhtml_start_element_cb start_cb;
    xhtml_end_element_cb end_cb;
    xhtml_char_data_cb char_cb;
    void *user_data;
    char error_msg[128];
    bool has_error;
};

static void *expat_psram_malloc(size_t size) {
    return psram_malloc(size);
}

static void *expat_psram_realloc(void *ptr, size_t size) {
    return psram_realloc(ptr, size);
}

static void expat_psram_free(void *ptr) {
    psram_free(ptr);
}

static const XML_Memory_Handling_Suite expat_psram_allocator = {
    expat_psram_malloc,
    expat_psram_realloc,
    expat_psram_free
};

static void XMLCALL expat_start_element(void *userData, const XML_Char *name, const XML_Char **atts) {
    XhtmlParser *parser = (XhtmlParser *)userData;
    if (parser->start_cb)
        parser->start_cb(name, atts, parser->user_data);
}

static void XMLCALL expat_end_element(void *userData, const XML_Char *name) {
    XhtmlParser *parser = (XhtmlParser *)userData;
    if (parser->end_cb)
        parser->end_cb(name, parser->user_data);
}

static void XMLCALL expat_char_data(void *userData, const XML_Char *s, int len) {
    XhtmlParser *parser = (XhtmlParser *)userData;
    if (parser->char_cb)
        parser->char_cb(s, len, parser->user_data);
}

XhtmlParser* xhtml_parser_create(void) {
    XhtmlParser *parser = (XhtmlParser *)psram_malloc(sizeof(XhtmlParser));
    if (!parser) {
        XHTML_ERR("Failed to allocate XhtmlParser\n");
        return NULL;
    }
    memset(parser, 0, sizeof(XhtmlParser));

    XML_Parser expat = XML_ParserCreate_MM(NULL, &expat_psram_allocator, NULL);
    if (!expat) {
        XHTML_ERR("Failed to create XML_Parser\n");
        psram_free(parser);
        return NULL;
    }

    parser->expat_parser = expat;
    XML_SetUserData(expat, parser);
    XML_SetElementHandler(expat, expat_start_element, expat_end_element);
    XML_SetCharacterDataHandler(expat, expat_char_data);

    XHTML_LOG("XhtmlParser created\n");
    return parser;
}

void xhtml_parser_destroy(XhtmlParser *parser) {
    if (!parser) return;
    if (parser->expat_parser)
        XML_ParserFree(parser->expat_parser);
    psram_free(parser);
    XHTML_LOG("XhtmlParser destroyed\n");
}

void xhtml_parser_set_callbacks(
    XhtmlParser *parser,
    xhtml_start_element_cb start_cb,
    xhtml_end_element_cb end_cb,
    xhtml_char_data_cb char_cb,
    void *user_data)
{
    if (!parser) return;
    parser->start_cb = start_cb;
    parser->end_cb = end_cb;
    parser->char_cb = char_cb;
    parser->user_data = user_data;
}

static void preprocess_html_entities(char *buf, int *len) {
    int rp = 0, wp = 0;
    while (rp < *len) {
        if (buf[rp] == '&' && rp + 2 < *len) {
            int semicolon = -1;
            for (int i = rp + 1; i < *len && i - rp < 32; i++) {
                if (buf[i] == ';') { semicolon = i; break; }
            }
            if (semicolon > rp + 1) {
                int name_len = semicolon - rp - 1;
                int replaced = 0;

                if (name_len == 4 && strncmp(buf + rp + 1, "nbsp", 4) == 0) {
                    buf[wp++] = (char)0xC2; buf[wp++] = (char)0xA0;
                    replaced = 1;
                } else if (name_len == 4 && strncmp(buf + rp + 1, "lt", 4) == 0) {
                    buf[wp++] = '<'; replaced = 1;
                } else if (name_len == 3 && strncmp(buf + rp + 1, "gt", 3) == 0) {
                    buf[wp++] = '>'; replaced = 1;
                } else if (name_len == 4 && strncmp(buf + rp + 1, "amp", 4) == 0) {
                    buf[wp++] = '&'; replaced = 1;
                } else if (name_len == 4 && strncmp(buf + rp + 1, "quot", 4) == 0) {
                    buf[wp++] = '"'; replaced = 1;
                } else if (name_len == 4 && strncmp(buf + rp + 1, "apos", 4) == 0) {
                    buf[wp++] = '\''; replaced = 1;

                } else if (name_len == 4 && strncmp(buf + rp + 1, "mdash", 4) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0x94;
                    replaced = 1;
                } else if (name_len == 4 && strncmp(buf + rp + 1, "ndash", 4) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0x93;
                    replaced = 1;
                } else if (name_len == 5 && strncmp(buf + rp + 1, "hellip", 5) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0xA6;
                    replaced = 1;
                } else if (name_len == 5 && strncmp(buf + rp + 1, "ldquo", 5) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0x9C;
                    replaced = 1;
                } else if (name_len == 5 && strncmp(buf + rp + 1, "rdquo", 5) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0x9D;
                    replaced = 1;
                } else if (name_len == 5 && strncmp(buf + rp + 1, "lsquo", 5) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0x98;
                    replaced = 1;
                } else if (name_len == 5 && strncmp(buf + rp + 1, "rsquo", 5) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0x99;
                    replaced = 1;
                } else if (name_len == 4 && strncmp(buf + rp + 1, "bull", 4) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0xA2;
                    replaced = 1;
                } else if (name_len == 2 && strncmp(buf + rp + 1, "em", 2) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0x94;
                    replaced = 1;
                } else if (name_len == 2 && strncmp(buf + rp + 1, "en", 2) == 0) {
                    buf[wp++] = (char)0xE2; buf[wp++] = (char)0x80; buf[wp++] = (char)0x93;
                    replaced = 1;
                } else if (name_len == 2 && strncmp(buf + rp + 1, "lt", 2) == 0) {
                    buf[wp++] = '<'; replaced = 1;
                } else if (name_len == 2 && strncmp(buf + rp + 1, "gt", 2) == 0) {
                    buf[wp++] = '>'; replaced = 1;
                }

                if (replaced) {
                    rp = semicolon + 1;
                    continue;
                }
            }
        }
        buf[wp++] = buf[rp++];
    }
    buf[wp] = '\0';
    *len = wp;
}

/* 把 XML 中非法的裸 '&'（不是 &name; / &#digits; 实体引用）替换为 &amp;。
 * 典型场景: <meta>Before Sunrise & Before Sunset</meta> 里的裸 &, Expat 会报
 * "not well-formed (invalid token)" 导致 OPF/章节解析失败。
 * out==NULL 时只统计所需长度, 不写入; 返回 -1 表示 out 空间不足。 */
static int sanitize_ampersands(const char *in, int in_len, char *out, int out_cap)
{
    int rp = 0, wp = 0;
    while (rp < in_len) {
        if (in[rp] == '&') {
            /* 判断是否为合法实体引用 */
            int k = rp + 1;
            int ok = 0;
            if (k < in_len && in[k] == '#') {
                int d = k + 1;
                if (d < in_len && (in[d] == 'x' || in[d] == 'X')) d++;
                while (d < in_len &&
                       ((in[d] >= '0' && in[d] <= '9') ||
                        (in[d] >= 'a' && in[d] <= 'f') ||
                        (in[d] >= 'A' && in[d] <= 'F'))) d++;
                if (d < in_len && in[d] == ';') ok = 1;
            } else {
                int n0 = k;
                while (k < in_len &&
                       ((in[k] >= 'a' && in[k] <= 'z') ||
                        (in[k] >= 'A' && in[k] <= 'Z') || in[k] == '_')) k++;
                if (k > n0 && k < in_len && in[k] == ';') ok = 1;
            }
            if (ok) {
                if (out) { if (wp + 1 > out_cap) return -1; out[wp] = '&'; }
                wp++; rp++;
                continue;
            }
            /* 裸 '&' -> "&amp;" */
            if (out) {
                if (wp + 5 > out_cap) return -1;
                memcpy(out + wp, "&amp;", 5);
            }
            wp += 5; rp++;
            continue;
        }
        if (out) {
            if (wp + 1 > out_cap) return -1;
            out[wp] = in[rp];
        }
        wp++; rp++;
    }
    if (out) out[wp] = '\0';
    return wp;
}

bool xhtml_parser_parse(XhtmlParser *parser, const char *xml_data, int xml_len) {
    if (!parser || !xml_data || xml_len <= 0) {
        XHTML_ERR("Invalid parse parameters\n");
        return false;
    }

    XHTML_LOG("Parsing %d bytes of XHTML...\n", xml_len);

    parser->has_error = false;
    parser->error_msg[0] = '\0';

    XML_ParserReset(parser->expat_parser, NULL);
    XML_SetUserData(parser->expat_parser, parser);
    XML_SetElementHandler(parser->expat_parser, expat_start_element, expat_end_element);
    XML_SetCharacterDataHandler(parser->expat_parser, expat_char_data);

    /* 先扫描裸 '&'; 仅当存在时才拷贝转义副本再解析, 否则直接用原缓冲(零开销) */
    const char *parse_data = xml_data;
    int parse_len = xml_len;
    char *sanitized = NULL;
    int need = sanitize_ampersands(xml_data, xml_len, NULL, 0);
    if (need > xml_len) {
        sanitized = (char *)psram_malloc((size_t)need + 1);
        if (!sanitized) {
            XHTML_ERR("Failed to alloc sanitize buffer (%d bytes)\n", need);
            return false;
        }
        if (sanitize_ampersands(xml_data, xml_len, sanitized, need + 1) < 0) {
            psram_free(sanitized);
            XHTML_ERR("Sanitize buffer overflow\n");
            return false;
        }
        parse_data = sanitized;
        parse_len = need;
        XHTML_LOG("Sanitized bare '&' (%d->%d bytes)\n", xml_len, need);
    }

    enum XML_Status status = XML_Parse(parser->expat_parser, parse_data, parse_len, 1);
    if (sanitized) psram_free(sanitized);

    if (status != XML_STATUS_OK) {
        parser->has_error = true;
        enum XML_Error err_code = XML_GetErrorCode(parser->expat_parser);
        snprintf(parser->error_msg, sizeof(parser->error_msg),
                 "Expat error (line %lu, col %lu): %s",
                 XML_GetCurrentLineNumber(parser->expat_parser),
                 XML_GetCurrentColumnNumber(parser->expat_parser),
                 XML_ErrorString(err_code));
        XHTML_ERR("Parse failed: %s\n", parser->error_msg);
        /* dump context around error */
        {
            long err_line = XML_GetCurrentLineNumber(parser->expat_parser);
            /* find byte offset of line err_line-2 to err_line+2 */
            int line_count = 0;
            const char *p = (const char *)xml_data;
            const char *err_line_start = NULL;
            const char *end = (const char *)xml_data + xml_len;
            while (p < end && line_count <= (int)err_line) {
                if (line_count >= (int)err_line - 3 && err_line_start == NULL)
                    err_line_start = p;
                if (*p == '\n') {
                    line_count++;
                    if (line_count > (int)err_line + 2) break;
                }
                p++;
            }
            if (err_line_start) {
                int ctx_len = (int)(p - err_line_start);
                if (ctx_len > 500) ctx_len = 500;
                XHTML_LOG("--- context around line %ld ---\n", err_line);
                XHTML_LOG("%.*s\n", ctx_len, err_line_start);
                XHTML_LOG("--- end context ---\n");
                /* dump hex at error line col 0-48 */
                {
                    int ln = 1;
                    const char *scan = (const char *)xml_data;
                    const char *target_line = (const char *)xml_data;
                    while (scan < (const char *)xml_data + xml_len && ln < (int)err_line) {
                        if (*scan == '\n') { ln++; if (ln == (int)err_line) target_line = scan + 1; }
                        scan++;
                    }
                    if (ln == (int)err_line && target_line) {
                        int dump_end = 48;
                        if (dump_end > xml_len - (int)(target_line - (const char *)xml_data))
                            dump_end = xml_len - (int)(target_line - (const char *)xml_data);
                        XHTML_LOG("--- hex at line %ld col 0-%d ---\n    ", err_line, dump_end);
                        for (int i = 0; i < dump_end; i++) {
                            printf("%02x ", (unsigned char)target_line[i]);
                            if ((i + 1) % 16 == 0 && i != dump_end - 1) printf("\n    ");
                        }
                        printf("\n--- end hex ---\n");
                    }
                }
            }
        }
        return false;
    }

    XHTML_LOG("Parse successful\n");
    return true;
}

const char* xhtml_parser_get_error(XhtmlParser *parser) {
    if (!parser) return "NULL parser";
    return parser->error_msg;
}
