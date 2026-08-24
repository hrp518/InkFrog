INCLUDE rom_symbol.ld
/* Linker script to configure memory regions. */
MEMORY
{
    RAM (rwx) : ORIGIN = 0x00201000, LENGTH = 412K
    FLASH (rx) : ORIGIN = 0x00400000, LENGTH = 16M
    PSRAM (rwx) : ORIGIN = 0x01400000, LENGTH = 4096K
}
__RAM_BASE = ORIGIN(RAM);
__XIP_BASE = ORIGIN(FLASH);
__XIP_LENGTH = LENGTH(FLASH);
__XIP_END = ORIGIN(FLASH) + LENGTH(FLASH) -1;
__PSRAM_BASE = ORIGIN(PSRAM);
__PSRAM_LENGTH = LENGTH(PSRAM);
__PSRAM_END = ORIGIN(PSRAM) + LENGTH(PSRAM) -1;
__DMAHEAP_PSRAM_BASE = __PSRAM_BASE + __PSRAM_LENGTH - 1024*1024;
__DMAHEAP_PSRAM_LENGTH = 1024*1024;
__DMAHEAP_PSRAM_END = __DMAHEAP_PSRAM_BASE + __DMAHEAP_PSRAM_LENGTH -1;
/* Linker script to place sections and symbol values. Should be used together
 * with other linker script that defines memory regions FLASH and RAM.
 * It references following symbols, which must be defined in code:
 *   Reset_Handler : Entry of reset handler
 *
 * It defines following symbols, which code can use without definition:
 *   __exidx_start
 *   __exidx_end
 *   __etext
 *   __data_start__
 *   __preinit_array_start
 *   __preinit_array_end
 *   __init_array_start
 *   __init_array_end
 *   __fini_array_start
 *   __fini_array_end
 *   __data_end__
 *   __bss_start__
 *   __bss_end__
 *   __end__
 *   end
 *   __HeapLimit
 *   __StackLimit
 *   __StackTop
 *   __stack
 *   _estack
 */
ENTRY(Reset_Handler)
SECTIONS
{
    .xip :
    {
        . = ALIGN(4);
        __xip_start__ = .;
        /* MUST not put IRQ handler/callback in .xip section */
        *libmp3.a: (.text .text.* .rodata .rodata.*)
        *libaac.a: (.text .text.* .rodata .rodata.*)
        *libwav.a: (.text .text.* .rodata .rodata.*)
        *libamr.a: (.text .text.* .rodata .rodata.*)
        *libamren.a: (.text .text.* .rodata .rodata.*)
        *libreverb.a: (.text .text.* .rodata .rodata.*)
        *libopus.a: (.text .text.* .rodata .rodata.*)
        *libspeex.a: (.text .text.* .rodata .rodata.*)
        *libcedarx.a: (.text .text.* .rodata .rodata.*)
        *libpcm.a: (.text .text.* .rodata .rodata.*)
        *libaudmgr.a: (.text .text.* .rodata .rodata.*)
        *liblwip.a: (.text .text.* .rodata .rodata.*)
        *libmbedtls.a: (.text .text.* .rodata .rodata.*)
        *libhttpcli.a: (.text .text.* .rodata .rodata.*)
        *libhttpd.a: (.text .text.* .rodata .rodata.*)
        *libmqtt.a: (.text .text.* .rodata .rodata.*)
        *libnopoll.a: (.text .text.* .rodata .rodata.*)
        *liblibwebsockets.a: (.text .text.* .rodata .rodata.*)
        *libping.a: (.text .text.* .rodata .rodata.*)
        *libsntp.a: (.text .text.* .rodata .rodata.*)
        *libudhcpd.a: (.text .text.* .rodata .rodata.*)
        *libaliyun.a: (.text .text.* .rodata .rodata.*)
        *libwlan.a: (.text .text.* .rodata .rodata.*)
        *libwpa.a: (.text .text.* .rodata .rodata.*)
        *libwpas.a: (.text .text.* .rodata .rodata.*)
        *libwpas_wps.a: (.text .text.* .rodata .rodata.*)
        *libhostapd.a: (.text .text.* .rodata .rodata.*)
        *libnet80211.a: (.text .text.* .rodata .rodata.*)
        *libxrwireless.a: (.text .text.* .rodata .rodata.*)
        *libethernetif.a: (.text .text.* .rodata .rodata.*)
        *libxretf.a: (.text .text.* .rodata .rodata.*)
        *libxretf_gui.a: (.text .text.* .rodata .rodata.*)
        *libxrsys.a: (.text .text.* .rodata .rodata.*)
        *libsc_assistant.a: (.text .text.* .rodata .rodata.*)
        *libsmartlink.a: (.text .text.* .rodata .rodata.*)
        *libairkiss_aes.a: (.text .text.* .rodata .rodata.*)
        *libadt.a: (.text .text.* .rodata .rodata.*)
        *libcjson.a: (.text .text.* .rodata .rodata.*)
        *libfs.a: (.text .text.* .rodata .rodata.*)
        *libatcmd.a: (.text .text.* .rodata .rodata.*)
        *libefpg.a: (.text .text.* .rodata .rodata.*)
        *libota.a: (.text .text.* .rodata .rodata.*)
        *libxz.a: (.text .text.* .rodata .rodata.*)
        *libconsole.a: (.text .text.* .rodata .rodata.*)
        *libutil.a: (.text .text.* .rodata .rodata.*)
        *libzbar.a: (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_wdg.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_timer.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_rtc.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_crypto.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_sdhost.o (.text .text.* .rodata .rodata.*)
        *libchip.a:sd.o (.text .text.* .rodata .rodata.*)
        *libchip.a:core.o (.text .text.* .rodata .rodata.*)
        *libchip.a:test.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_i2s.o (.text .text.* .rodata .rodata.*)
        *libchip.a:ac107.o (.text .text.* .rodata .rodata.*)
        *libchip.a:xradio_internal_codec.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_snd_card.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_i2c.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_dmic.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_pwm.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_spi.o (.text .text.* .rodata .rodata.*)
        *libchip.a:hal_efuse.o (.text .text.* .rodata .rodata.*)
        *libm.a: (EXCLUDE_FILE (lib_a-s_fpclassify.o lib_a-sf_fpclassify.o) .text
                  EXCLUDE_FILE (lib_a-s_fpclassify.o lib_a-sf_fpclassify.o) .text.*
                  EXCLUDE_FILE (lib_a-s_fpclassify.o lib_a-sf_fpclassify.o) .rodata
                  EXCLUDE_FILE (lib_a-s_fpclassify.o lib_a-sf_fpclassify.o) .rodata.*)
        *libc_nano.a:lib_a-asctime.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-asctime_r.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-ctime.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-gmtime.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-gmtime_r.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-lcltime.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-lcltime_r.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-mktime.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strftime.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-gettzinfo.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-month_lengths.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-timelocal.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-tzcalc_limits.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-tzlock.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-tzset.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-tzset_r.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-tzvars.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-envlock.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-getenv.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-getenv_r.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-rand.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-getopt.o (.text .text.* .rodata .rodata.*)
        /*
         * Before putting the following objs of libc_nano.a to xip,
         * make sure they are not called in IRQ handler/callback.
         */
/*
        *libc_nano.a:lib_a-atoi.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-atol.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-atoll.o (.text .text.* .rodata .rodata.*)

        *libc_nano.a:lib_a-strcasecmp.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strcmp.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strncasecmp.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strncmp.o (.text .text.* .rodata .rodata.*)

        *libc_nano.a:lib_a-strcpy.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strlcpy.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strncpy.o (.text .text.* .rodata .rodata.*)

        *libc_nano.a:lib_a-strcat.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strchr.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strcspn.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strdup.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strdup_r.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strerror.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-u_strerr.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strrchr.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strspn.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strstr.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strtod.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-gdtoa-gethex.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-gdtoa-hexnan.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strtok.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strtok_r.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strtol.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strtoll.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strtoll_r.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-strtoul.o (.text .text.* .rodata .rodata.*)

        *libc_nano.a:lib_a-sscanf.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-stdio.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-nano-svfscanf.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-nano-vfscanf_i.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-nano-vfscanf_float.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-sf_nan.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-sccl.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-ungetc.o (.text .text.* .rodata .rodata.*)
*/
        /*
         * Before putting the following objs of libc_nano.a to xip,
         * make sure float printf is not used in IRQ handler/callback.
         */
/*
        *libc_nano.a:lib_a-nano-vfprintf_float.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-dtoa.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-locale.o (.text .text.* .rodata .rodata.*)
        *libc_nano.a:lib_a-mprec.o (.text .text.* .rodata .rodata.*)
        *libm.a:lib_a-s_fpclassify.o (.text .text.* .rodata .rodata.*)
        *libm.a:lib_a-sf_fpclassify.o (.text .text.* .rodata .rodata.*)
*/
        *project/common/board/board.o (.text .text.* .rodata .rodata.*)
        *project/common/cmd*.o (.text .text.* .rodata .rodata.*)
        *project/common/iperf*.o (.text .text.* .rodata .rodata.*)
        *project/common/framework*.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/player_app.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/recorder_app.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/os_glue/cdx_memory.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/os_glue/atomic.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/os_glue/file.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/os_glue/pthread.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/os_glue/sleep.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/capture_ctrl/captureControl_rtos.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/sound_ctrl/card_pcm.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/sound_ctrl/reverb_buffer.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/sound_ctrl/reverb_pcm.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/sound_ctrl/soundControl_rtos.o (.text .text.* .rodata .rodata.*)
        *project/common/apps/cedarx/sound_ctrl/soundStreamControl.o (.text .text.* .rodata .rodata.*)
        *command.o (.text .text.* .rodata .rodata.*)
        /* CoreMark benchmark */
        *coremark*.o (.text .text.* .rodata .rodata.*)
        /* Stage-1 XIP whitelist: LVGL misc pure utility/math helpers only */
        *lv_area.o (.text .text.* .rodata .rodata.*)
        *lv_color.o (.text .text.* .rodata .rodata.*)
        *lv_math.o (.text .text.* .rodata .rodata.*)
        *lv_printf.o (.text .text.* .rodata .rodata.*)
        *lv_utils.o (.text .text.* .rodata .rodata.*)
        /* Stage-2 XIP whitelist: LVGL mid-layer text/style/container helpers */
        *lv_ll.o (.text .text.* .rodata .rodata.*)
        *lv_lru.o (.text .text.* .rodata .rodata.*)
        *lv_bidi.o (.text .text.* .rodata .rodata.*)
        *lv_txt.o (.text .text.* .rodata .rodata.*)
        *lv_txt_ap.o (.text .text.* .rodata .rodata.*)
        *lv_style.o (.text .text.* .rodata .rodata.*)
        *lv_style_gen.o (.text .text.* .rodata .rodata.*)
        /* Stage-3 XIP whitelist: 默认字体/内嵌JS/XML解析/压缩库
         * (2026-08-12 曾为腾 SRAM 移入 PSRAM, 但 PSRAM 取指/取数慢,
         *  且 misans 是 LV_FONT_DEFAULT, 阅读器每条文本路径都会回退查它;
         *  改放 XIP: 不占 SRAM, 不走 PSRAM 慢总线, 休眠安全——挂起链
         *  在 flashc_suspend 之后才掉电, SDK 代码本就运行在 .xip。
         *  任务栈仍全部留在 SRAM (08-12 修复的前提不变)。 */
        *project/demo/FontExp/lv_font_misans_16.o (.text .text.* .rodata .rodata.*)
        *project/demo/FontExp/http_l1glyf_js.o (.text .text.* .rodata .rodata.*)
        *project/demo/FontExp/third_party/expat/xmltok.o (.text .text.* .rodata .rodata.*)
        *project/demo/FontExp/third_party/expat/xmlparse.o (.text .text.* .rodata .rodata.*)
        *project/demo/FontExp/third_party/expat/xmlrole.o (.text .text.* .rodata .rodata.*)
        *project/demo/FontExp/third_party/miniz/miniz.o (.text .text.* .rodata .rodata.*)
        *(.xip_text* .xip_rodata*)
        . = ALIGN(4);
        __xip_end__ = .;
    } > FLASH
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
        /* ===== 休眠崩溃修复 (2026-08-12): 任务栈必须留在 SRAM =====
         * 原因: SRAM .text 过大把堆挤到只剩 ~940B, disp_task/lvgl_task
         * (最后创建) 的 40KB 栈落入 PSRAM; 休眠挂起链跑在 PSRAM 栈上,
         * 链内 psram_suspend/psramc_suspend 重配 PSRAM 时栈被破坏 →
         * 返回地址变垃圾 → hard fault (PC=PSRAM 栈地址, 多份崩溃日志)。
         * 修复思路保留: 大数据/代码不能占 SRAM, 栈必须留在 SRAM。
         * 原移入 PSRAM 的 6 个对象 (lv_font_misans_16/http_l1glyf_js/
         * expat×3/miniz) 已改放 .xip 段 (flash XIP, 见 .xip 的
         * Stage-3 whitelist): 同样不占 SRAM (栈空间不受影响),
         * 且不走 PSRAM 慢总线, 休眠同样安全。 */
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
        *(.psram_text* .psram_rodata*)
        . = ALIGN(4);
        __psram_text_end__ = .;
    } > PSRAM
    .psram_data :
    {
        . = ALIGN(4);
        __psram_data_start__ = .;
        *libmp3.a: ( .data .data.* vtable )
        *libaac.a: ( .data .data.* vtable )
        *libwav.a: ( .data .data.* vtable )
        *libamr.a: ( .data .data.* vtable )
        *libamren.a: ( .data .data.* vtable )
        *libreverb.a: ( .data .data.* vtable )
        *libopus.a: ( .data .data.* vtable )
        *libcedarx.a: ( .data .data.* vtable )
        *libpcm.a: ( .data .data.* vtable )
        *libaudmgr.a: ( .data .data.* vtable )
        *liblwip.a: ( .data .data.* vtable )
        *libmbedtls.a: ( .data .data.* vtable )
        *libhttpcli.a: ( .data .data.* vtable )
        *libhttpd.a: ( .data .data.* vtable )
        *libmqtt.a: ( .data .data.* vtable )
        *libnopoll.a: ( .data .data.* vtable )
        *liblibwebsockets.a: ( .data .data.* vtable )
        *libping.a: ( .data .data.* vtable )
        *libsntp.a: ( .data .data.* vtable )
        *libudhcpd.a: ( .data .data.* vtable )
        *libaliyun.a: ( .data .data.* vtable )
        *libwlan.a: ( .data .data.* vtable )
        *libwpa.a: ( .data .data.* vtable )
        *libwpas.a: ( .data .data.* vtable )
        *libwpas_wps.a: ( .data .data.* vtable )
        *libhostapd.a: ( .data .data.* vtable )
        *libnet80211.a: ( .data .data.* vtable )
        *libxrwireless.a: ( .data .data.* vtable )
        *libethernetif.a: ( .data .data.* vtable )
        *libxretf.a: ( .data .data.* vtable )
        *libxretf_gui.a: ( .data .data.* vtable )
        *libxrsys.a: ( .data .data.* vtable )
        *libsc_assistant.a: ( .data .data.* vtable )
        *libsmartlink.a: ( .data .data.* vtable )
        *libairkiss_aes.a: ( .data .data.* vtable )
        *libadt.a: ( .data .data.* vtable )
        *libcjson.a: ( .data .data.* vtable )
        *libfs.a: ( .data .data.* vtable )
        *libatcmd.a: ( .data .data.* vtable )
        *libefpg.a: ( .data .data.* vtable )
        *libota.a: ( .data .data.* vtable )
        *libxz.a: ( .data .data.* vtable )
        *libutil.a: ( .data .data.* vtable )
        *libchip.a:hal_wdg.o ( .data .data.* vtable )
        *libchip.a:hal_timer.o ( .data .data.* vtable )
        *libchip.a:hal_rtc.o ( .data .data.* vtable )
        *libchip.a:hal_crypto.o ( .data .data.* vtable )
        *libchip.a:hal_sdhost.o ( .data .data.* vtable )
        *libchip.a:sd.o ( .data .data.* vtable )
        *libchip.a:core.o ( .data .data.* vtable )
        *libchip.a:test.o ( .data .data.* vtable )
        *libchip.a:hal_i2s.o ( .data .data.* vtable )
        *libchip.a:ac107.o ( .data .data.* vtable )
        *libchip.a:xradio_internal_codec.o ( .data .data.* vtable )
        *libchip.a:hal_snd_card.o ( .data .data.* vtable )
        *libchip.a:hal_i2c.o ( .data .data.* vtable )
        *libchip.a:hal_dmic.o ( .data .data.* vtable )
        *libchip.a:hal_pwm.o ( .data .data.* vtable )
        *libchip.a:hal_spi.o ( .data .data.* vtable )
        *libchip.a:hal_efuse.o ( .data .data.* vtable )
        *libm.a: ( .data .data.* vtable )
        *libc_nano.a:lib_a-asctime.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-asctime_r.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-ctime.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-gmtime.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-gmtime_r.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-lcltime.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-lcltime_r.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-mktime.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-strftime.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-gettzinfo.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-month_lengths.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-timelocal.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-tzcalc_limits.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-tzlock.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-tzset.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-tzset_r.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-tzvars.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-envlock.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-getenv.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-getenv_r.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-rand.o ( .data .data.* vtable )
        *libc_nano.a:lib_a-getopt.o ( .data .data.* vtable )
        *project/common/cmd*.o ( .data .data.* vtable )
        *project/common/iperf*.o ( .data .data.* vtable )
        *project/common/apps/player_app.o ( .data .data.* vtable )
        *project/common/apps/recorder_app.o ( .data .data.* vtable )
        *project/common/apps/cedarx/os_glue/cdx_memory.o ( .data .data.* vtable )
        *project/common/apps/cedarx/os_glue/atomic.o ( .data .data.* vtable )
        *project/common/apps/cedarx/os_glue/file.o ( .data .data.* vtable )
        *project/common/apps/cedarx/os_glue/pthread.o ( .data .data.* vtable )
        *project/common/apps/cedarx/os_glue/sleep.o ( .data .data.* vtable )
        *project/common/apps/cedarx/capture_ctrl/captureControl_rtos.o ( .data .data.* vtable )
        *project/common/apps/cedarx/sound_ctrl/card_pcm.o ( .data .data.* vtable )
        *project/common/apps/cedarx/sound_ctrl/reverb_buffer.o ( .data .data.* vtable )
        *project/common/apps/cedarx/sound_ctrl/reverb_pcm.o ( .data .data.* vtable )
        *project/common/apps/cedarx/sound_ctrl/soundControl_rtos.o ( .data .data.* vtable )
        *project/common/apps/cedarx/sound_ctrl/soundStreamControl.o ( .data .data.* vtable )
        *command.o ( .data .data.* vtable )
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
        *(.psram_data*)
        . = ALIGN(4);
        __psram_data_end__ = .;
    } > PSRAM
    .psram_bss :
    {
        . = ALIGN(4);
        __psram_bss_start__ = .;
        *libmp3.a: ( .bss .bss.* COMMON )
        *libaac.a: ( .bss .bss.* COMMON )
        *libwav.a: ( .bss .bss.* COMMON )
        *libamr.a: ( .bss .bss.* COMMON )
        *libamren.a: ( .bss .bss.* COMMON )
        *libreverb.a: ( .bss .bss.* COMMON )
        *libopus.a: ( .bss .bss.* COMMON )
        *libcedarx.a: ( .bss .bss.* COMMON )
        *libpcm.a: ( .bss .bss.* COMMON )
        *libaudmgr.a: ( .bss .bss.* COMMON )
        *liblwip.a: ( .bss .bss.* COMMON )
        *libmbedtls.a: ( .bss .bss.* COMMON )
        *libhttpcli.a: ( .bss .bss.* COMMON )
        *libhttpd.a: ( .bss .bss.* COMMON )
        *libmqtt.a: ( .bss .bss.* COMMON )
        *libnopoll.a: ( .bss .bss.* COMMON )
        *liblibwebsockets.a: ( .bss .bss.* COMMON )
        *libping.a: ( .bss .bss.* COMMON )
        *libsntp.a: ( .bss .bss.* COMMON )
        *libudhcpd.a: ( .bss .bss.* COMMON )
        *libaliyun.a: ( .bss .bss.* COMMON )
        *libwlan.a: ( .bss .bss.* COMMON )
        *libwpa.a: ( .bss .bss.* COMMON )
        *libwpas.a: ( .bss .bss.* COMMON )
        *libwpas_wps.a: ( .bss .bss.* COMMON )
        *libhostapd.a: ( .bss .bss.* COMMON )
        *libnet80211.a: ( .bss .bss.* COMMON )
        *libxrwireless.a: ( .bss .bss.* COMMON )
        *libethernetif.a: ( .bss .bss.* COMMON )
        *libxretf.a: ( .bss .bss.* COMMON )
        *libxretf_gui.a: ( .bss .bss.* COMMON )
        *libxrsys.a: ( .bss .bss.* COMMON )
        *libsc_assistant.a: ( .bss .bss.* COMMON )
        *libsmartlink.a: ( .bss .bss.* COMMON )
        *libairkiss_aes.a: ( .bss .bss.* COMMON )
        *libadt.a: ( .bss .bss.* COMMON )
        *libcjson.a: ( .bss .bss.* COMMON )
        *libfs.a: ( .bss .bss.* COMMON )
        *libatcmd.a: ( .bss .bss.* COMMON )
        *libefpg.a: ( .bss .bss.* COMMON )
        *libota.a: ( .bss .bss.* COMMON )
        *libxz.a: ( .bss .bss.* COMMON )
        *libutil.a: ( .bss .bss.* COMMON )
        *libchip.a:hal_wdg.o ( .bss .bss.* COMMON )
        *libchip.a:hal_timer.o ( .bss .bss.* COMMON )
        *libchip.a:hal_rtc.o ( .bss .bss.* COMMON )
        *libchip.a:hal_crypto.o ( .bss .bss.* COMMON )
        *libchip.a:hal_sdhost.o ( .bss .bss.* COMMON )
        *libchip.a:sd.o ( .bss .bss.* COMMON )
        *libchip.a:core.o ( .bss .bss.* COMMON )
        *libchip.a:test.o ( .bss .bss.* COMMON )
        *libchip.a:hal_i2s.o ( .bss .bss.* COMMON )
        *libchip.a:ac107.o ( .bss .bss.* COMMON )
        *libchip.a:xradio_internal_codec.o ( .bss .bss.* COMMON )
        *libchip.a:hal_snd_card.o ( .bss .bss.* COMMON )
        *libchip.a:hal_i2c.o ( .bss .bss.* COMMON )
        *libchip.a:hal_dmic.o ( .bss .bss.* COMMON )
        *libchip.a:hal_pwm.o ( .bss .bss.* COMMON )
        *libchip.a:hal_spi.o ( .bss .bss.* COMMON )
        *libchip.a:hal_efuse.o ( .bss .bss.* COMMON )
        *libm.a: ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-asctime.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-asctime_r.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-ctime.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-gmtime.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-gmtime_r.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-lcltime.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-lcltime_r.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-mktime.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-strftime.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-gettzinfo.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-month_lengths.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-timelocal.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-tzcalc_limits.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-tzlock.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-tzset.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-tzset_r.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-tzvars.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-envlock.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-getenv.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-getenv_r.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-rand.o ( .bss .bss.* COMMON )
        *libc_nano.a:lib_a-getopt.o ( .bss .bss.* COMMON )
        *project/common/cmd*.o ( .bss .bss.* COMMON )
        *project/common/iperf*.o ( .bss .bss.* COMMON )
        *project/common/apps/player_app.o ( .bss .bss.* COMMON )
        *project/common/apps/recorder_app.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/os_glue/cdx_memory.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/os_glue/atomic.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/os_glue/file.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/os_glue/pthread.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/os_glue/sleep.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/capture_ctrl/captureControl_rtos.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/sound_ctrl/card_pcm.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/sound_ctrl/reverb_buffer.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/sound_ctrl/reverb_pcm.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/sound_ctrl/soundControl_rtos.o ( .bss .bss.* COMMON )
        *project/common/apps/cedarx/sound_ctrl/soundStreamControl.o ( .bss .bss.* COMMON )
        *command.o ( .bss .bss.* COMMON )
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
        *(.psram_bss*)
        . = ALIGN(16);
        __psram_bss_end__ = .;
        __psram_end__ = .;
    } > PSRAM
    .text :
    {
        . = ALIGN(4);
        __text_start__ = .;
        __VECTOR_BASE = .;
        KEEP(*(.isr_vector))
        KEEP(*(.text.Reset_Handler))
        KEEP(*(.cpu_text))
        *(.text*)
        *(.nonxip_text*)
        *(.sram_text*)
        KEEP(*(.init))
        KEEP(*(.fini))
        /* .ctors */
        *crtbegin.o(.ctors)
        *crtbegin?.o(.ctors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .ctors)
        *(SORT(.ctors.*))
        *(.ctors)
        /* .dtors */
        *crtbegin.o(.dtors)
        *crtbegin?.o(.dtors)
        *(EXCLUDE_FILE(*crtend?.o *crtend.o) .dtors)
        *(SORT(.dtors.*))
        *(.dtors)
        *(.rodata*)
        *(.nonxip_rodata*)
        *(.sram_rodata*)
        KEEP(*(.eh_frame*))
        . = ALIGN(4);
        __text_end__ = .;
    } > RAM
    .ARM.extab :
    {
        *(.ARM.extab* .gnu.linkonce.armextab.*)
    } > RAM
    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    } > RAM
    __exidx_end = .;
    . = ALIGN(4);
    __etext = .;
    _sidata = .;
    .data :
    {
        . = ALIGN(4);
        __data_start__ = .;
        _sdata = .;
        *(vtable)
        *(.data*)
        *(.nonxip_data*)
        *(.sram_data*)
        . = ALIGN(4);
        /* preinit data */
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
        . = ALIGN(4);
        /* init data */
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE_HIDDEN (__init_array_end = .);
        . = ALIGN(4);
        /* finit data */
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array))
        PROVIDE_HIDDEN (__fini_array_end = .);
        KEEP(*(.jcr*))
        . = ALIGN(4);
        *(*.got*)
        __data_end__ = .;
        _edata = .;
    } > RAM
    .bss :
    {
        . = ALIGN(4);
        __bss_start__ = .;
        _sbss = .;
        *(.bss*)
        *(COMMON)
        *(.nonxip_bss*)
        *(.sram_bss*)
        . = ALIGN(4);
        __bss_end__ = .;
        _ebss = .;
    } > RAM
    .heap (COPY):
    {
        __end__ = .;
        end = __end__;
        *(.heap*)
        __HeapLimit = .;
    } > RAM
    /* .stack_dummy section doesn't contains any symbols. It is only
     * used for linker to calculate size of stack sections, and assign
     * values to stack symbols later */
    .stack_dummy (COPY):
    {
        *(.stack*)
    } > RAM
    .ram_table : AT(__bss_start__)
    {
        . = ALIGN(4);
        KEEP(*(.ram_table))
        . = ALIGN(4);
    } > RAM
    __ram_table_lma_start__ = __bss_start__;
    __ram_table_lma_end__ = __ram_table_lma_start__ + SIZEOF(.ram_table);
    REMAIN_CACHE_SIZE = 0;
    /* Set stack top to end of RAM, and stack limit move down by
     * size of stack_dummy section */
 __StackTop = ORIGIN(RAM) + LENGTH(RAM);
    __StackTop += REMAIN_CACHE_SIZE;
    _estack = __StackTop;
    __StackLimit = __StackTop - SIZEOF(.stack_dummy);
    PROVIDE(__stack = __StackTop);
    /* Check if data + heap + stack exceeds RAM limit */
    ASSERT(__StackLimit >= __HeapLimit, "ld: region RAM overflowed with stack")
 /* Check if psramdata exceeds PSRAM limit */
    ASSERT(__DMAHEAP_PSRAM_BASE > __psram_end__, "ld: region dmapsramheap overflowed with psram heap")
}
