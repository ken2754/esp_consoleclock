#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Crisp non-bold 5x7 monospace terminal font (4x6 pixel glyph with 1px spacing)
// Fits 63 columns x 34 rows on a 320x240 display!
extern const lv_font_t lv_font_term_5x7;

#ifdef __cplusplus
}
#endif
