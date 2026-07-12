#ifndef FONT_WARM_H
#define FONT_WARM_H

#include <stddef.h>

/* 开机 UI 显示后延迟调度 L1 预加载（不阻塞首页） */
void font_warm_schedule_boot(void);

/* 设置里更换字体后重新预热（异步） */
void font_warm_request(const char * ttf_path);

/* 当前共享 L1 是否已就绪 */
int font_warm_is_ready(void);

/* 已预热的 TTF 路径（未预热返回 NULL） */
const char * font_warm_cached_path(void);

/* 路径是否与已预热缓存一致（忽略 0: / 0:/ 差异） */
int font_warm_path_matches(const char * ttf_path);

/* 解析阅读器应使用的 TTF 路径（settings 优先，否则最小 ttf） */
int font_warm_resolve_reader_path(char * out, size_t out_sz);

/* 检查 .l1glyf 是否存在（先 .l1glyf/ 子目录，再同目录 sidecar） */
int font_warm_l1glyf_exists(const char * ttf_path);

/* 由 TTF 路径推导首选 .l1glyf 缓存路径 */
void font_warm_l1glyf_path_for_ttf(const char * ttf_path, char * out, size_t out_sz);

#endif
