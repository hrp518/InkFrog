#ifndef EPUB_XHTML_PARSER_H
#define EPUB_XHTML_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XhtmlParser XhtmlParser;

typedef void (*xhtml_start_element_cb)(const char *name, const char **atts, void *user_data);
typedef void (*xhtml_end_element_cb)(const char *name, void *user_data);
typedef void (*xhtml_char_data_cb)(const char *data, int len, void *user_data);

XhtmlParser* xhtml_parser_create(void);

void xhtml_parser_destroy(XhtmlParser *parser);

void xhtml_parser_set_callbacks(
    XhtmlParser *parser,
    xhtml_start_element_cb start_cb,
    xhtml_end_element_cb end_cb,
    xhtml_char_data_cb char_cb,
    void *user_data
);

bool xhtml_parser_parse(XhtmlParser *parser, const char *xml_data, int xml_len);

const char* xhtml_parser_get_error(XhtmlParser *parser);

#ifdef __cplusplus
}
#endif

#endif /* EPUB_XHTML_PARSER_H */
