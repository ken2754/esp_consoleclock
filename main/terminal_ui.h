#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TERM_STYLE_NORMAL = 0,      // Standard terminal output (#d4d4d8, recolor OFF)
    TERM_STYLE_PROMPT,          // Prompt + command line (recolor ON for green/blue prompt)
    TERM_STYLE_DATE_HIGHLIGHT,  // Highlighted `date` output (bright background bar, dark text)
    TERM_STYLE_QUOTA,           // Antigravity quota text output (#67e8f9, recolor OFF)
    TERM_STYLE_CODE,            // Source code / grep / sed output (#a1a1aa, recolor OFF)
    TERM_STYLE_CHAT,            // Chat / write / wall messages (#fde047, recolor OFF)
    TERM_STYLE_GAME,            // ASCII game graphics (#4ade80, recolor OFF)
    TERM_STYLE_STATUS_BAR,      // Inverted status bar for vim / games / tmux (#e4e4e7 on #27272a)
    TERM_STYLE_ALERT            // Panic / error / boss key alert (#f87171, recolor OFF)
} term_line_style_t;

esp_err_t terminal_ui_init(void);
void terminal_ui_timer_handler(void);

// Line & Buffer manipulation (Thread-safe)
void terminal_ui_clear(void);
void terminal_ui_push_line(const char *text, term_line_style_t style);
void terminal_ui_set_line(int row, const char *text, term_line_style_t style);
int  terminal_ui_get_active_row(void);

// Active typing line helpers (prompt + typed command + blinking cursor)
void terminal_ui_start_prompt(const char *cwd_short);
void terminal_ui_update_command(const char *cmd_text, bool show_cursor);
void terminal_ui_finish_command(const char *final_cmd_text);

// Alternate screen buffer support (like xterm smcup/rmcup for vim & full-screen games)
void terminal_ui_enter_alt_screen(void);
void terminal_ui_exit_alt_screen(void);
bool terminal_ui_is_alt_screen(void);

// Count how many scrolls have occurred since last reset (used to protect :00 time/quota)
int  terminal_ui_get_scroll_count(void);
void terminal_ui_reset_scroll_count(void);

#ifdef __cplusplus
}
#endif
