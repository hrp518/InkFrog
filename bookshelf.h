/**
 * @file bookshelf.h
 * @brief Inkfrog 书架 - 扫描 Inkbook，2×3 平铺，进入阅读
 */

#ifndef BOOKSHELF_H
#define BOOKSHELF_H

#ifdef __cplusplus
extern "C" {
#endif

/** 启动书架界面（调用方需已挂载 SD） */
void bookshelf_init(void);

/** 进入书架：先推"正在解析..."遮罩并刷新到屏，下一拍才真正扫描解析（避免
 *  解析期间屏幕无反馈被当成卡死）。调用方需已挂载 SD。 */
void bookshelf_begin_load(void);

/** 关闭书架并返回主界面 */
void bookshelf_close(void);

/** 从阅读器返回后重建书架 UI */
void bookshelf_show(void);

#ifdef __cplusplus
}
#endif

#endif /* BOOKSHELF_H */
