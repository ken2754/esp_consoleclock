#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Crisp, non-bold 6x8 monospace terminal font (5x7 glyph in 6x8 cell, 1px stroke)
// Fits 52 columns x 30 rows on a 320x240 display.
extern const lv_font_t lv_font_term_6x8;

#ifdef __cplusplus
}
#endif
