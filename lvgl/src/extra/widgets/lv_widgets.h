/**
 * @file lv_widgets.h
 *
 */

#ifndef LV_WIDGETS_H
#define LV_WIDGETS_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#if LV_USE_ANIMIMG
#include "animimg/lv_animimg.h"
#endif
#if LV_USE_CALENDAR
#include "calendar/lv_calendar.h"
#include "calendar/lv_calendar_header_arrow.h"
#include "calendar/lv_calendar_header_dropdown.h"
#endif
#if LV_USE_CHART
#include "chart/lv_chart.h"
#endif
#if LV_USE_COLORWHEEL
#include "colorwheel/lv_colorwheel.h"
#endif
#if LV_USE_IMGBTN
#include "imgbtn/lv_imgbtn.h"
#endif
#if LV_USE_KEYBOARD
#include "keyboard/lv_keyboard.h"
#endif
#if LV_USE_LED
#include "led/lv_led.h"
#endif
#if LV_USE_LIST
#include "list/lv_list.h"
#endif
#if LV_USE_MENU
#include "menu/lv_menu.h"
#endif
#if LV_USE_METER
#include "meter/lv_meter.h"
#endif
#if LV_USE_MSGBOX
#include "msgbox/lv_msgbox.h"
#endif
#if LV_USE_SPAN
#include "span/lv_span.h"
#endif
#if LV_USE_SPINBOX
#include "spinbox/lv_spinbox.h"
#endif
#if LV_USE_SPINNER
#include "spinner/lv_spinner.h"
#endif
#if LV_USE_TABVIEW
#include "tabview/lv_tabview.h"
#endif
#if LV_USE_TILEVIEW
#include "tileview/lv_tileview.h"
#endif
#if LV_USE_WIN
#include "win/lv_win.h"
#endif

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /*LV_WIDGETS_H*/
