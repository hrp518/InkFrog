# LVGL代码从SRAM迁移到PSRAM计划

## 项目概述

**项目路径**: `C:\XR872\home\Administrator\xradio-skylark-sdk-master\project\demo\FontExp`
**链接脚本**: `gcc\.project.ld`
**编译指南**: `C:\XR872\LVGL编译指南.md`

## 当前状态分析

### 内存布局 (来自.project.ld)
- **RAM (SRAM)**: 0x00201000, 长度412KB - 当前LVGL代码存放位置
- **FLASH (XIP)**: 0x00400000, 长度16MB - XIP执行区域
- **PSRAM**: 0x01400000, 长度4096KB - 目标迁移区域

### 当前LVGL代码在SRAM的情况
查看`.project.ld`的`.text`段(第474-505行)，LVGL代码目前存放在SRAM中，因为：
1. `.text`段放在RAM区域
2. 只有少量LVGL工具函数被放入XIP区域（第222-235行）
3. 大部分LVGL源码编译后的`.text`和`.rodata`默认落入SRAM

### LVGL源码结构分析

LVGL源码位于 `lvgl/src/` 目录，按层次分为：

| 层次 | 目录 | 文件数 | 说明 |
|------|------|--------|------|
| **底层(核心)** | `misc/` | 15个 | 基硎工具：内存、数学、颜色、区域计算等 |
| **底层(HAL)** | `hal/` | 3个 | 硬件抽象层：tick、显示、输入设备 |
| **中层(核心)** | `core/` | 15个 | 对象系统：事件、样式、布局、刷新等 |
| **中层(绘制)** | `draw/` | ~30个 | 绘制引擎：矩形、圆弧、文字、图片等 |
| **高层(控件)** | `widgets/` | 12个 | 基础控件：按钮、标签、滑块等 |
| **高层(扩展)** | `extra/` | ~40个 | 扩展控件、主题、布局、库等 |

---

## 迁移策略：两步走方案

### 迁移原则
1. **先迁移高级代码，后迁移底层代码** - 确保依赖关系正确
2. **通过修改`.project.ld`实现** - 将目标文件放入PSRAM段
3. **分阶段验证** - 每步完成后编译验证

---

## 第一步：迁移高级代码（控件层+扩展层+绘制层）

### 目标
将LVGL的控件、扩展功能、绘制引擎迁移到PSRAM

### 需要修改的文件
**文件**: `gcc\.project.ld`

### 具体修改内容

#### 1.1 在`.psram_text`段添加LVGL高级代码

在`.psram_text`段（第240-258行）的现有内容后添加：

```ld
    .psram_text :
    {
        . = ALIGN(4);
        __psram_start__ = .;
        __psram_text_start__ = .;
        *project/common/cmd/cmd_psram.o (.text* .rodata*)
        *project/demo/FontExp/http_server.o (.text* .rodata*)
        *project/demo/FontExp/file_manager.o (.text* .rodata*)
        *project/demo/FontExp/epd.o (.text* .rodata*)
        *project/demo/FontExp/epub_reader.o (.text* .rodata*)
        *project/demo/FontExp/epub_viewer.o (.text* .rodata*)
        *project/demo/FontExp/font_priority_loader.o (.text* .rodata*)
        *project/demo/FontExp/level1_data.o (.text* .rodata*)
        *project/demo/FontExp/level2_data.o (.text* .rodata*)
        *project/demo/FontExp/level3_data.o (.text* .rodata*)
        
        /* ===== 第一步：LVGL高级代码迁移到PSRAM ===== */
        
        /* 扩展控件 (extra/widgets/) */
        *lv_win.o (.text* .rodata*)
        *lv_tileview.o (.text* .rodata*)
        *lv_tabview.o (.text* .rodata*)
        *lv_spinner.o (.text* .rodata*)
        *lv_spinbox.o (.text* .rodata*)
        *lv_span.o (.text* .rodata*)
        *lv_msgbox.o (.text* .rodata*)
        *lv_meter.o (.text* .rodata*)
        *lv_menu.o (.text* .rodata*)
        *lv_list.o (.text* .rodata*)
        *lv_led.o (.text* .rodata*)
        *lv_keyboard.o (.text* .rodata*)
        *lv_imgbtn.o (.text* .rodata*)
        *lv_colorwheel.o (.text* .rodata*)
        *lv_chart.o (.text* .rodata*)
        *lv_calendar.o (.text* .rodata*)
        *lv_calendar_header_arrow.o (.text* .rodata*)
        *lv_calendar_header_dropdown.o (.text* .rodata*)
        *lv_animimg.o (.text* .rodata*)
        
        /* 基础控件 (widgets/) */
        *lv_textarea.o (.text* .rodata*)
        *lv_table.o (.text* .rodata*)
        *lv_switch.o (.text* .rodata*)
        *lv_slider.o (.text* .rodata*)
        *lv_roller.o (.text* .rodata*)
        *lv_line.o (.text* .rodata*)
        *lv_label.o (.text* .rodata*)
        *lv_img.o (.text* .rodata*)
        *lv_dropdown.o (.text* .rodata*)
        *lv_checkbox.o (.text* .rodata*)
        *lv_canvas.o (.text* .rodata*)
        *lv_btnmatrix.o (.text* .rodata*)
        *lv_btn.o (.text* .rodata*)
        *lv_bar.o (.text* .rodata*)
        *lv_arc.o (.text* .rodata*)
        
        /* 绘制引擎 (draw/及其子目录) */
        *lv_draw.o (.text* .rodata*)
        *lv_draw_arc.o (.text* .rodata*)
        *lv_draw_img.o (.text* .rodata*)
        *lv_draw_label.o (.text* .rodata*)
        *lv_draw_layer.o (.text* .rodata*)
        *lv_draw_line.o (.text* .rodata*)
        *lv_draw_mask.o (.text* .rodata*)
        *lv_draw_rect.o (.text* .rodata*)
        *lv_draw_transform.o (.text* .rodata*)
        *lv_draw_triangle.o (.text* .rodata*)
        *lv_img_buf.o (.text* .rodata*)
        *lv_img_cache.o (.text* .rodata*)
        *lv_img_decoder.o (.text* .rodata*)
        
        /* 软件绘制 (draw/sw/) */
        *lv_draw_sw.o (.text* .rodata*)
        *lv_draw_sw_arc.o (.text* .rodata*)
        *lv_draw_sw_blend.o (.text* .rodata*)
        *lv_draw_sw_dither.o (.text* .rodata*)
        *lv_draw_sw_gradient.o (.text* .rodata*)
        *lv_draw_sw_img.o (.text* .rodata*)
        *lv_draw_sw_layer.o (.text* .rodata*)
        *lv_draw_sw_letter.o (.text* .rodata*)
        *lv_draw_sw_line.o (.text* .rodata*)
        *lv_draw_sw_polygon.o (.text* .rodata*)
        *lv_draw_sw_rect.o (.text* .rodata*)
        *lv_draw_sw_transform.o (.text* .rodata*)
        
        /* 扩展功能 (extra/) */
        *lv_extra.o (.text* .rodata*)
        *lv_flex.o (.text* .rodata*)
        *lv_grid.o (.text* .rodata*)
        *lv_theme_basic.o (.text* .rodata*)
        *lv_theme_default.o (.text* .rodata*)
        *lv_theme_mono.o (.text* .rodata*)
        *lv_snapshot.o (.text* .rodata*)
        *lv_msg.o (.text* .rodata*)
        *lv_monkey.o (.text* .rodata*)
        *lv_imgfont.o (.text* .rodata*)
        *lv_ime_pinyin.o (.text* .rodata*)
        *lv_gridnav.o (.text* .rodata*)
        *lv_fragment.o (.text* .rodata*)
        *lv_fragment_manager.o (.text* .rodata*)
        
        /* 扩展库 (extra/libs/) */
        *lv_tiny_ttf.o (.text* .rodata*)
        *lv_bmp.o (.text* .rodata*)
        *lv_freetype.o (.text* .rodata*)
        *lv_gif.o (.text* .rodata*)
        *gifdec.o (.text* .rodata*)
        *lv_png.o (.text* .rodata*)
        *lodepng.o (.text* .rodata*)
        *lv_qrcode.o (.text* .rodata*)
        *qrcodegen.o (.text* .rodata*)
        *lv_rlottie.o (.text* .rodata*)
        *lv_sjpg.o (.text* .rodata*)
        *tjpgd.o (.text* .rodata*)
        *lv_fs_fatfs.o (.text* .rodata*)
        *lv_fs_littlefs.o (.text* .rodata*)
        *lv_fs_posix.o (.text* .rodata*)
        *lv_fs_stdio.o (.text* .rodata*)
        *lv_fs_win32.o (.text* .rodata*)
        
        *(.psram_text* .psram_rodata*)
        . = ALIGN(4);
        __psram_text_end__ = .;
    } > PSRAM
```

#### 1.2 在`.psram_data`段添加LVGL高级代码的数据段

在`.psram_data`段（第259-365行）末尾添加：

```ld
        /* ===== 第一步：LVGL高级代码数据段迁移到PSRAM ===== */
        *lv_win.o ( .data .data.* vtable )
        *lv_tileview.o ( .data .data.* vtable )
        *lv_tabview.o ( .data .data.* vtable )
        *lv_spinner.o ( .data .data.* vtable )
        *lv_spinbox.o ( .data .data.* vtable )
        *lv_span.o ( .data .data.* vtable )
        *lv_msgbox.o ( .data .data.* vtable )
        *lv_meter.o ( .data .data.* vtable )
        *lv_menu.o ( .data .data.* vtable )
        *lv_list.o ( .data .data.* vtable )
        *lv_led.o ( .data .data.* vtable )
        *lv_keyboard.o ( .data .data.* vtable )
        *lv_imgbtn.o ( .data .data.* vtable )
        *lv_colorwheel.o ( .data .data.* vtable )
        *lv_chart.o ( .data .data.* vtable )
        *lv_calendar.o ( .data .data.* vtable )
        *lv_calendar_header_arrow.o ( .data .data.* vtable )
        *lv_calendar_header_dropdown.o ( .data .data.* vtable )
        *lv_animimg.o ( .data .data.* vtable )
        *lv_textarea.o ( .data .data.* vtable )
        *lv_table.o ( .data .data.* vtable )
        *lv_switch.o ( .data .data.* vtable )
        *lv_slider.o ( .data .data.* vtable )
        *lv_roller.o ( .data .data.* vtable )
        *lv_line.o ( .data .data.* vtable )
        *lv_label.o ( .data .data.* vtable )
        *lv_img.o ( .data .data.* vtable )
        *lv_dropdown.o ( .data .data.* vtable )
        *lv_checkbox.o ( .data .data.* vtable )
        *lv_canvas.o ( .data .data.* vtable )
        *lv_btnmatrix.o ( .data .data.* vtable )
        *lv_btn.o ( .data .data.* vtable )
        *lv_bar.o ( .data .data.* vtable )
        *lv_arc.o ( .data .data.* vtable )
        *lv_draw.o ( .data .data.* vtable )
        *lv_draw_arc.o ( .data .data.* vtable )
        *lv_draw_img.o ( .data .data.* vtable )
        *lv_draw_label.o ( .data .data.* vtable )
        *lv_draw_layer.o ( .data .data.* vtable )
        *lv_draw_line.o ( .data .data.* vtable )
        *lv_draw_mask.o ( .data .data.* vtable )
        *lv_draw_rect.o ( .data .data.* vtable )
        *lv_draw_transform.o ( .data .data.* vtable )
        *lv_draw_triangle.o ( .data .data.* vtable )
        *lv_img_buf.o ( .data .data.* vtable )
        *lv_img_cache.o ( .data .data.* vtable )
        *lv_img_decoder.o ( .data .data.* vtable )
        *lv_draw_sw.o ( .data .data.* vtable )
        *lv_draw_sw_arc.o ( .data .data.* vtable )
        *lv_draw_sw_blend.o ( .data .data.* vtable )
        *lv_draw_sw_dither.o ( .data .data.* vtable )
        *lv_draw_sw_gradient.o ( .data .data.* vtable )
        *lv_draw_sw_img.o ( .data .data.* vtable )
        *lv_draw_sw_layer.o ( .data .data.* vtable )
        *lv_draw_sw_letter.o ( .data .data.* vtable )
        *lv_draw_sw_line.o ( .data .data.* vtable )
        *lv_draw_sw_polygon.o ( .data .data.* vtable )
        *lv_draw_sw_rect.o ( .data .data.* vtable )
        *lv_draw_sw_transform.o ( .data .data.* vtable )
        *lv_extra.o ( .data .data.* vtable )
        *lv_flex.o ( .data .data.* vtable )
        *lv_grid.o ( .data .data.* vtable )
        *lv_theme_basic.o ( .data .data.* vtable )
        *lv_theme_default.o ( .data .data.* vtable )
        *lv_theme_mono.o ( .data .data.* vtable )
        *lv_tiny_ttf.o ( .data .data.* vtable )
```

#### 1.3 在`.psram_bss`段添加LVGL高级代码的BSS段

在`.psram_bss`段（第366-473行）末尾添加：

```ld
        /* ===== 第一步：LVGL高级代码BSS段迁移到PSRAM ===== */
        *lv_win.o ( .bss .bss.* COMMON )
        *lv_tileview.o ( .bss .bss.* COMMON )
        *lv_tabview.o ( .bss .bss.* COMMON )
        *lv_spinner.o ( .bss .bss.* COMMON )
        *lv_spinbox.o ( .bss .bss.* COMMON )
        *lv_span.o ( .bss .bss.* COMMON )
        *lv_msgbox.o ( .bss .bss.* COMMON )
        *lv_meter.o ( .bss .bss.* COMMON )
        *lv_menu.o ( .bss .bss.* COMMON )
        *lv_list.o ( .bss .bss.* COMMON )
        *lv_led.o ( .bss .bss.* COMMON )
        *lv_keyboard.o ( .bss .bss.* COMMON )
        *lv_imgbtn.o ( .bss .bss.* COMMON )
        *lv_colorwheel.o ( .bss .bss.* COMMON )
        *lv_chart.o ( .bss .bss.* COMMON )
        *lv_calendar.o ( .bss .bss.* COMMON )
        *lv_calendar_header_arrow.o ( .bss .bss.* COMMON )
        *lv_calendar_header_dropdown.o ( .bss .bss.* COMMON )
        *lv_animimg.o ( .bss .bss.* COMMON )
        *lv_textarea.o ( .bss .bss.* COMMON )
        *lv_table.o ( .bss .bss.* COMMON )
        *lv_switch.o ( .bss .bss.* COMMON )
        *lv_slider.o ( .bss .bss.* COMMON )
        *lv_roller.o ( .bss .bss.* COMMON )
        *lv_line.o ( .bss .bss.* COMMON )
        *lv_label.o ( .bss .bss.* COMMON )
        *lv_img.o ( .bss .bss.* COMMON )
        *lv_dropdown.o ( .bss .bss.* COMMON )
        *lv_checkbox.o ( .bss .bss.* COMMON )
        *lv_canvas.o ( .bss .bss.* COMMON )
        *lv_btnmatrix.o ( .bss .bss.* COMMON )
        *lv_btn.o ( .bss .bss.* COMMON )
        *lv_bar.o ( .bss .bss.* COMMON )
        *lv_arc.o ( .bss .bss.* COMMON )
        *lv_draw.o ( .bss .bss.* COMMON )
        *lv_draw_arc.o ( .bss .bss.* COMMON )
        *lv_draw_img.o ( .bss .bss.* COMMON )
        *lv_draw_label.o ( .bss .bss.* COMMON )
        *lv_draw_layer.o ( .bss .bss.* COMMON )
        *lv_draw_line.o ( .bss .bss.* COMMON )
        *lv_draw_mask.o ( .bss .bss.* COMMON )
        *lv_draw_rect.o ( .bss .bss.* COMMON )
        *lv_draw_transform.o ( .bss .bss.* COMMON )
        *lv_draw_triangle.o ( .bss .bss.* COMMON )
        *lv_img_buf.o ( .bss .bss.* COMMON )
        *lv_img_cache.o ( .bss .bss.* COMMON )
        *lv_img_decoder.o ( .bss .bss.* COMMON )
        *lv_draw_sw.o ( .bss .bss.* COMMON )
        *lv_draw_sw_arc.o ( .bss .bss.* COMMON )
        *lv_draw_sw_blend.o ( .bss .bss.* COMMON )
        *lv_draw_sw_dither.o ( .bss .bss.* COMMON )
        *lv_draw_sw_gradient.o ( .bss .bss.* COMMON )
        *lv_draw_sw_img.o ( .bss .bss.* COMMON )
        *lv_draw_sw_layer.o ( .bss .bss.* COMMON )
        *lv_draw_sw_letter.o ( .bss .bss.* COMMON )
        *lv_draw_sw_line.o ( .bss .bss.* COMMON )
        *lv_draw_sw_polygon.o ( .bss .bss.* COMMON )
        *lv_draw_sw_rect.o ( .bss .bss.* COMMON )
        *lv_draw_sw_transform.o ( .bss .bss.* COMMON )
        *lv_extra.o ( .bss .bss.* COMMON )
        *lv_flex.o ( .bss .bss.* COMMON )
        *lv_grid.o ( .bss .bss.* COMMON )
        *lv_theme_basic.o ( .bss .bss.* COMMON )
        *lv_theme_default.o ( .bss .bss.* COMMON )
        *lv_theme_mono.o ( .bss .bss.* COMMON )
        *lv_tiny_ttf.o ( .bss .bss.* COMMON )
```

### 第一步验证
编译并检查map文件，确认上述目标文件已放入PSRAM区域

---

## 第二步：迁移底层代码（核心层+HAL层+基础工具层）

### 目标
将LVGL的核心对象系统、HAL层、基础工具迁移到PSRAM

### 需要修改的文件
**文件**: `gcc\.project.ld`

### 具体修改内容

#### 2.1 在`.psram_text`段末尾（第一步添加的内容之后）添加：

```ld
        /* ===== 第二步：LVGL底层代码迁移到PSRAM ===== */
        
        /* 核心对象系统 (core/) */
        *lv_obj.o (.text* .rodata*)
        *lv_obj_class.o (.text* .rodata*)
        *lv_obj_draw.o (.text* .rodata*)
        *lv_obj_pos.o (.text* .rodata*)
        *lv_obj_scroll.o (.text* .rodata*)
        *lv_obj_style.o (.text* .rodata*)
        *lv_obj_style_gen.o (.text* .rodata*)
        *lv_obj_tree.o (.text* .rodata*)
        *lv_disp.o (.text* .rodata*)
        *lv_event.o (.text* .rodata*)
        *lv_group.o (.text* .rodata*)
        *lv_indev.o (.text* .rodata*)
        *lv_indev_scroll.o (.text* .rodata*)
        *lv_refr.o (.text* .rodata*)
        *lv_theme.o (.text* .rodata*)
        
        /* HAL层 (hal/) */
        *lv_hal_disp.o (.text* .rodata*)
        *lv_hal_indev.o (.text* .rodata*)
        *lv_hal_tick.o (.text* .rodata*)
        
        /* 基础工具层 (misc/) - 注意：部分已在XIP中 */
        *lv_anim.o (.text* .rodata*)
        *lv_anim_timeline.o (.text* .rodata*)
        *lv_async.o (.text* .rodata*)
        *lv_fs.o (.text* .rodata*)
        *lv_gc.o (.text* .rodata*)
        *lv_log.o (.text* .rodata*)
        *lv_mem.o (.text* .rodata*)
        *lv_timer.o (.text* .rodata*)
        *lv_tlsf.o (.text* .rodata*)
        
        /* 字体系统 (font/) */
        *lv_font.o (.text* .rodata*)
        *lv_font_dejavu_16_persian_hebrew.o (.text* .rodata*)
        *lv_font_fmt_txt.o (.text* .rodata*)
        *lv_font_loader.o (.text* .rodata*)
        *lv_font_montserrat_*.o (.text* .rodata*)
        *lv_font_simsun_16_cjk.o (.text* .rodata*)
        *lv_font_unscii_*.o (.text* .rodata*)
```

#### 2.2 在`.psram_data`段末尾添加：

```ld
        /* ===== 第二步：LVGL底层代码数据段迁移到PSRAM ===== */
        *lv_obj.o ( .data .data.* vtable )
        *lv_obj_class.o ( .data .data.* vtable )
        *lv_obj_draw.o ( .data .data.* vtable )
        *lv_obj_pos.o ( .data .data.* vtable )
        *lv_obj_scroll.o ( .data .data.* vtable )
        *lv_obj_style.o ( .data .data.* vtable )
        *lv_obj_style_gen.o ( .data .data.* vtable )
        *lv_obj_tree.o ( .data .data.* vtable )
        *lv_disp.o ( .data .data.* vtable )
        *lv_event.o ( .data .data.* vtable )
        *lv_group.o ( .data .data.* vtable )
        *lv_indev.o ( .data .data.* vtable )
        *lv_indev_scroll.o ( .data .data.* vtable )
        *lv_refr.o ( .data .data.* vtable )
        *lv_theme.o ( .data .data.* vtable )
        *lv_hal_disp.o ( .data .data.* vtable )
        *lv_hal_indev.o ( .data .data.* vtable )
        *lv_hal_tick.o ( .data .data.* vtable )
        *lv_anim.o ( .data .data.* vtable )
        *lv_anim_timeline.o ( .data .data.* vtable )
        *lv_async.o ( .data .data.* vtable )
        *lv_fs.o ( .data .data.* vtable )
        *lv_gc.o ( .data .data.* vtable )
        *lv_log.o ( .data .data.* vtable )
        *lv_mem.o ( .data .data.* vtable )
        *lv_timer.o ( .data .data.* vtable )
        *lv_tlsf.o ( .data .data.* vtable )
        *lv_font.o ( .data .data.* vtable )
```

#### 2.3 在`.psram_bss`段末尾添加：

```ld
        /* ===== 第二步：LVGL底层代码BSS段迁移到PSRAM ===== */
        *lv_obj.o ( .bss .bss.* COMMON )
        *lv_obj_class.o ( .bss .bss.* COMMON )
        *lv_obj_draw.o ( .bss .bss.* COMMON )
        *lv_obj_pos.o ( .bss .bss.* COMMON )
        *lv_obj_scroll.o ( .bss .bss.* COMMON )
        *lv_obj_style.o ( .bss .bss.* COMMON )
        *lv_obj_style_gen.o ( .bss .bss.* COMMON )
        *lv_obj_tree.o ( .bss .bss.* COMMON )
        *lv_disp.o ( .bss .bss.* COMMON )
        *lv_event.o ( .bss .bss.* COMMON )
        *lv_group.o ( .bss .bss.* COMMON )
        *lv_indev.o ( .bss .bss.* COMMON )
        *lv_indev_scroll.o ( .bss .bss.* COMMON )
        *lv_refr.o ( .bss .bss.* COMMON )
        *lv_theme.o ( .bss .bss.* COMMON )
        *lv_hal_disp.o ( .bss .bss.* COMMON )
        *lv_hal_indev.o ( .bss .bss.* COMMON )
        *lv_hal_tick.o ( .bss .bss.* COMMON )
        *lv_anim.o ( .bss .bss.* COMMON )
        *lv_anim_timeline.o ( .bss .bss.* COMMON )
        *lv_async.o ( .bss .bss.* COMMON )
        *lv_fs.o ( .bss .bss.* COMMON )
        *lv_gc.o ( .bss .bss.* COMMON )
        *lv_log.o ( .bss .bss.* COMMON )
        *lv_mem.o ( .bss .bss.* COMMON )
        *lv_timer.o ( .bss .bss.* COMMON )
        *lv_tlsf.o ( .bss .bss.* COMMON )
        *lv_font.o ( .bss .bss.* COMMON )
```

### 第二步验证
编译并检查map文件，确认所有LVGL代码已放入PSRAM区域

---

## 注意事项

### 保留在XIP区域的代码
以下基础工具函数已在XIP区域（第222-235行），**不建议迁移**：
- `lv_area.o` - 区域计算
- `lv_color.o` - 颜色处理
- `lv_math.o` - 数学运算
- `lv_printf.o` - 打印函数
- `lv_utils.o` - 工具函数
- `lv_ll.o` - 链表
- `lv_lru.o` - LRU缓存
- `lv_bidi.o` - 双向文本
- `lv_txt.o` - 文本处理
- `lv_txt_ap.o` - 文字对齐
- `lv_style.o` - 样式
- `lv_style_gen.o` - 样式生成

这些是高频调用的纯计算函数，放在XIP区域执行效率更高。

### 编译验证命令
```bash
# 清理并编译
C:/XR872/bin/bash.exe -lc "cd /home/Administrator/xradio-skylark-sdk-master/project/demo/FontExp/gcc && rm -f objects_response.txt && make build"

# 生成IMG镜像
C:/XR872/bin/bash.exe -lc "cd /home/Administrator/xradio-skylark-sdk-master/project/demo/FontExp/image/xr872 && /cygdrive/c/XR872/home/Administrator/xradio-skylark-sdk-master/tools/mkimage.exe -O -c image_auto_cal.cfg -o xr_system.img"
```

### 验证方法
检查 `gcc/FontExp.map` 文件，确认：
1. LVGL目标文件地址落在PSRAM区域 (0x01400000 - 0x01800000)
2. SRAM区域使用量显著减少
3. 无链接错误

---

## 预期效果

| 指标 | 迁移前 | 第一步后 | 第二步后 |
|------|--------|----------|----------|
| SRAM代码段 | ~200KB+ | ~100KB | ~20KB |
| PSRAM代码段 | ~50KB | ~150KB | ~230KB |
| SRAM可用堆 | ~95KB | ~195KB | ~275KB |

---

## 文件修改清单

| 步骤 | 文件 | 修改位置 | 修改类型 |
|------|------|----------|----------|
| 第一步 | `gcc\.project.ld` | `.psram_text`段 | 添加高级代码 |
| 第一步 | `gcc\.project.ld` | `.psram_data`段 | 添加高级代码数据 |
| 第一步 | `gcc\.project.ld` | `.psram_bss`段 | 添加高级代码BSS |
| 第二步 | `gcc\.project.ld` | `.psram_text`段 | 添加底层代码 |
| 第二步 | `gcc\.project.ld` | `.psram_data`段 | 添加底层代码数据 |
| 第二步 | `gcc\.project.ld` | `.psram_bss`段 | 添加底层代码BSS |
