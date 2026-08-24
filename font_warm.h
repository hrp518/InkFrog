#ifndef FONT_WARM_H
#define FONT_WARM_H

#include <stddef.h>
#include <stdbool.h>

/* 开机 UI 显示后延迟调度 L1 预加载（不阻塞首页） */
void font_warm_schedule_boot(void);

/* 开机时在 main() 内同步执行 L1 预加载(在首页构建/刷屏之前, 由开机画面遮罩覆盖)。
 * ttf_path 传 NULL 时按解析规则(font_warm_resolve_reader_path)选择字体。 */
int font_warm_run_boot(const char * ttf_path);

/* 设置里更换字体后重新预热（异步） */
void font_warm_request(const char * ttf_path);

/* 阅读器已自行完成 L1 预热(共享缓存已就绪且被阅读器引用):
 * 记录预热路径并取消后续 boot/延时 warm, 避免二次初始化把字体搞坏/重复加载。 */
void font_warm_mark_loaded(const char * ttf_path);

/* 阅读器正在使用共享 L1 缓存时置 true, 释放字体后置 false。
 * 期间 font_warm 不得释放/重初始化共享缓存(否则阅读器字体被破坏)。 */
void font_warm_reader_active(bool active);

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

/* 在 Font 目录中查找已有 .l1glyf 缓存的 TTF（多个时取最大） */
int font_warm_find_l1glyf_paired_ttf(char * out, size_t out_sz);

#endif
