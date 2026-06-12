/**
 * @file ui_theme.h
 * @brief Shared colors, fonts, and small utility helpers for all screens.
 */
#pragma once

#include "lvgl.h"

/* Color palette - dark theme, industrial look */
#define UI_COLOR_BG          lv_color_hex(0x1A1A1A)   /* screen background */
#define UI_COLOR_PANEL       lv_color_hex(0x2C2C2C)   /* card / panel */
#define UI_COLOR_PANEL_ALT   lv_color_hex(0x333333)   /* alternating row */
#define UI_COLOR_BORDER      lv_color_hex(0x3A3A3A)

#define UI_COLOR_TEXT        lv_color_hex(0xFFFFFF)
#define UI_COLOR_TEXT_MUTED  lv_color_hex(0x9A9A9A)
#define UI_COLOR_TEXT_DIM    lv_color_hex(0x666666)

/* Status colors */
#define UI_COLOR_PASS        lv_color_hex(0x2A8A4A)   /* green */
#define UI_COLOR_PASS_DARK   lv_color_hex(0x1E6F3E)
#define UI_COLOR_HIGH        lv_color_hex(0xB03030)   /* red */
#define UI_COLOR_HIGH_DARK   lv_color_hex(0x7A2020)
#define UI_COLOR_LOW         lv_color_hex(0xB8860B)   /* amber */
#define UI_COLOR_LOW_DARK    lv_color_hex(0x8A6408)

#define UI_COLOR_ACCENT      lv_color_hex(0x4A90E2)   /* blue accent */

/* Common spacing */
#define UI_PAD               12
#define UI_PAD_S             6
#define UI_RADIUS            8
#define UI_RADIUS_S          4

/* Screen dims for the 10.1" (default rotation: landscape 1280x800) */
#define UI_W                 1280
#define UI_H                 800
