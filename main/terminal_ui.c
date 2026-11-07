#include "terminal_ui.h"
#include "term_font_6x8.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "terminal_ui";

#define MAX_CMD_CHARS   140
#define MAX_PROMPT_RAW  116

typedef struct {
    char text[MAX_PROMPT_RAW];
    term_line_style_t style;
} term_row_data_t;

static SemaphoreHandle_t s_lvgl_mux = NULL;
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_row_labels[TERM_ROWS];
static lv_obj_t *s_cursor_lbl = NULL;
static lv_timer_t *s_cursor_timer = NULL;

// Main screen buffer
static term_row_data_t s_rows[TERM_ROWS];
static int s_active_row = 0;
static int s_scroll_count = 0;

// Dirty-check cache to avoid redundant LVGL invalidations and eliminate flicker
static char s_rendered_text[TERM_ROWS][MAX_PROMPT_RAW];
static term_line_style_t s_rendered_style[TERM_ROWS];
static bool s_style_initialized[TERM_ROWS];

// Saved screen buffer for alternate screen mode (vim, nethack, etc.)
static term_row_data_t s_saved_rows[TERM_ROWS];
static int s_saved_active_row = 0;
static bool s_in_alt_screen = false;

// Active multi-line prompt & cursor state
static char s_current_cwd[16] = "~/agy";
static char s_current_cmd[MAX_CMD_CHARS] = "";
static bool s_prompt_active = false;
static int  s_prompt_start_row = 0;
static int  s_prompt_num_rows = 1;
static bool s_cursor_visible = true;
static int  s_cursor_row = 0;
static int  s_cursor_col = 0;

static void apply_row_visual(int r)
{
    if (r < 0 || r >= TERM_ROWS || !s_row_labels[r]) return;
    lv_obj_t *lbl = s_row_labels[r];
    term_line_style_t st = s_rows[r].style;

    // Only update LVGL style properties when the row's style actually changes
    if (!s_style_initialized[r] || s_rendered_style[r] != st) {
        s_style_initialized[r] = true;
        s_rendered_style[r] = st;

        lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_label_set_recolor(lbl, false);

        switch (st) {
        case TERM_STYLE_PROMPT:
            lv_label_set_recolor(lbl, true);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xf4f4f5), LV_PART_MAIN);
            break;

        case TERM_STYLE_DATE_HIGHLIGHT:
            lv_obj_set_style_bg_color(lbl, lv_color_hex(0x22c55e), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x052e16), LV_PART_MAIN);
            break;

        case TERM_STYLE_QUOTA:
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x67e8f9), LV_PART_MAIN);
            break;

        case TERM_STYLE_CODE:
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xa1a1aa), LV_PART_MAIN);
            break;

        case TERM_STYLE_CHAT:
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xfde047), LV_PART_MAIN);
            break;

        case TERM_STYLE_GAME:
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x4ade80), LV_PART_MAIN);
            break;

        case TERM_STYLE_STATUS_BAR:
            lv_obj_set_style_bg_color(lbl, lv_color_hex(0x27272a), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x38bdf8), LV_PART_MAIN);
            break;

        case TERM_STYLE_ALERT:
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xf87171), LV_PART_MAIN);
            break;

        case TERM_STYLE_NORMAL:
        default:
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xd4d4d8), LV_PART_MAIN);
            break;
        }
    }

    // Only call lv_label_set_text when the text string has actually changed
    if (strcmp(s_rendered_text[r], s_rows[r].text) != 0) {
        strncpy(s_rendered_text[r], s_rows[r].text, sizeof(s_rendered_text[r]) - 1);
        s_rendered_text[r][sizeof(s_rendered_text[r]) - 1] = '\0';
        lv_label_set_text(lbl, s_rows[r].text);
    }
}

static void update_cursor_visual_internal(bool show_cursor)
{
    if (!s_cursor_lbl) return;
    if (show_cursor && s_prompt_active && !s_in_alt_screen && s_cursor_visible) {
        lv_obj_set_pos(s_cursor_lbl, 3 + s_cursor_col * TERM_CHAR_WIDTH, s_cursor_row * TERM_LINE_HEIGHT);
        lv_obj_clear_flag(s_cursor_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cursor_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_all_rows_internal(void)
{
    for (int r = 0; r < TERM_ROWS; r++) {
        apply_row_visual(r);
    }
}

static void scroll_up_one_line_internal(void)
{
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        s_rows[r] = s_rows[r + 1];
    }
    memset(&s_rows[TERM_ROWS - 1], 0, sizeof(term_row_data_t));
    s_rows[TERM_ROWS - 1].style = TERM_STYLE_NORMAL;
    s_active_row = TERM_ROWS - 1;
    if (s_prompt_active && s_prompt_start_row > 0) {
        s_prompt_start_row--;
    }
    if (!s_in_alt_screen) {
        s_scroll_count++;
    }
}

// Escape '#' for LVGL recolor mode on a slice of command text [start_idx .. start_idx + len)
static void escape_hash_slice(char *dst, size_t dst_size, const char *src, size_t len)
{
    size_t j = 0;
    for (size_t i = 0; i < len && src[i] != '\0' && j + 3 < dst_size; i++) {
        if (src[i] == '#') {
            dst[j++] = '#';
            dst[j++] = '#';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

// Render the active command across 1 or more wrapped lines using VT100 delayed-wrap rules.
// The cursor is rendered via a dedicated 6x8 LVGL overlay label so blinking never redraws row text.
static void render_prompt_multiline_internal(bool with_cursor)
{
    // Visible prefix: "ken@cyd:" (8) + cwd + "$ " (2) -> 10 + strlen(cwd) chars
    size_t cwd_len = strlen(s_current_cwd);
    size_t prefix_vis_len = 10 + cwd_len;
    size_t first_line_cap = (TERM_COLS > prefix_vis_len) ? (TERM_COLS - prefix_vis_len) : 20;
    size_t cont_line_cap  = TERM_COLS;

    size_t cmd_len = strlen(s_current_cmd);
    size_t total_vis_len = prefix_vis_len + cmd_len;

    // VT100 delayed wrap: a new line is allocated only when a character lands on it
    int needed_rows = (total_vis_len <= TERM_COLS)
                        ? 1
                        : (int)((total_vis_len + TERM_COLS - 1) / TERM_COLS);
    if (needed_rows > 4) needed_rows = 4;

    bool scrolled = false;
    while (s_prompt_start_row + needed_rows - 1 >= TERM_ROWS) {
        scroll_up_one_line_internal();
        scrolled = true;
    }

    // If command shrank (e.g. during backspace), clear any previously used continuation rows
    if (needed_rows < s_prompt_num_rows) {
        for (int r = s_prompt_start_row + needed_rows; r < s_prompt_start_row + s_prompt_num_rows; r++) {
            if (r >= 0 && r < TERM_ROWS) {
                s_rows[r].text[0] = '\0';
                s_rows[r].style = TERM_STYLE_NORMAL;
                if (!scrolled) {
                    apply_row_visual(r);
                }
            }
        }
    }
    s_prompt_num_rows = needed_rows;
    s_active_row = s_prompt_start_row + needed_rows - 1;

    // Format each row of the command (without embedding the cursor in the label string)
    size_t offset = 0;
    for (int line_idx = 0; line_idx < needed_rows; line_idx++) {
        int r = s_prompt_start_row + line_idx;
        size_t cap = (line_idx == 0) ? first_line_cap : cont_line_cap;
        size_t avail = (offset < cmd_len) ? (cmd_len - offset) : 0;
        size_t take = (avail > cap) ? cap : avail;

        char escaped_slice[72];
        escape_hash_slice(escaped_slice, sizeof(escaped_slice), s_current_cmd + offset, take);
        offset += take;

        if (line_idx == 0) {
            snprintf(s_rows[r].text, sizeof(s_rows[r].text),
                     "#4ade80 ken@cyd#:#38bdf8 %s#$ %s",
                     s_current_cwd, escaped_slice);
        } else {
            snprintf(s_rows[r].text, sizeof(s_rows[r].text),
                     "%s", escaped_slice);
        }
        s_rows[r].style = TERM_STYLE_PROMPT;
        if (!scrolled) {
            apply_row_visual(r);
        }
    }

    if (scrolled) {
        refresh_all_rows_internal();
    }

    // Compute exact cursor cell coordinates (row, col)
    int cursor_line_idx = needed_rows - 1;
    size_t line_start_vis = (size_t)cursor_line_idx * TERM_COLS;
    size_t col_on_line = (total_vis_len > line_start_vis) ? (total_vis_len - line_start_vis) : 0;
    if (col_on_line >= TERM_COLS) {
        col_on_line = TERM_COLS - 1;
    }
    s_cursor_row = s_prompt_start_row + cursor_line_idx;
    s_cursor_col = (int)col_on_line;
    update_cursor_visual_internal(with_cursor);
}

static void cursor_blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_cursor_visible = !s_cursor_visible;
    update_cursor_visual_internal(s_prompt_active);
}

esp_err_t terminal_ui_init(void)
{
    s_lvgl_mux = xSemaphoreCreateMutex();
    if (!s_lvgl_mux) return ESP_FAIL;

    xSemaphoreTake(s_lvgl_mux, portMAX_DELAY);

    s_scr = lv_scr_act();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x09090b), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    memset(s_rows, 0, sizeof(s_rows));
    memset(s_rendered_text, 0, sizeof(s_rendered_text));
    memset(s_style_initialized, 0, sizeof(s_style_initialized));
    s_active_row = 0;
    s_scroll_count = 0;

    for (int r = 0; r < TERM_ROWS; r++) {
        lv_obj_t *lbl = lv_label_create(s_scr);
        lv_obj_set_pos(lbl, 2, r * TERM_LINE_HEIGHT);
        lv_obj_set_size(lbl, LCD_H_RES - 4, TERM_LINE_HEIGHT);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(lbl, &lv_font_term_6x8, LV_PART_MAIN);
        lv_obj_set_style_pad_top(lbl, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_bottom(lbl, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_left(lbl, 1, LV_PART_MAIN);
        lv_label_set_text(lbl, "");
        s_row_labels[r] = lbl;
    }

    // Dedicated 6x8 pixel cursor overlay so blinking never invalidates full row labels
    s_cursor_lbl = lv_label_create(s_scr);
    lv_obj_set_size(s_cursor_lbl, TERM_CHAR_WIDTH, TERM_LINE_HEIGHT);
    lv_obj_set_style_text_font(s_cursor_lbl, &lv_font_term_6x8, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_cursor_lbl, lv_color_hex(0x4ade80), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_cursor_lbl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_cursor_lbl, 0, LV_PART_MAIN);
    lv_label_set_text_static(s_cursor_lbl, "_");
    lv_obj_add_flag(s_cursor_lbl, LV_OBJ_FLAG_HIDDEN);

    s_cursor_timer = lv_timer_create(cursor_blink_timer_cb, 450, NULL);

    xSemaphoreGive(s_lvgl_mux);
    ESP_LOGI(TAG, "Terminal UI initialized (%d rows x %d cols, 6x8 non-bold font)", TERM_ROWS, TERM_COLS);
    return ESP_OK;
}

void terminal_ui_timer_handler(void)
{
    if (!s_lvgl_mux) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        lv_timer_handler();
        xSemaphoreGive(s_lvgl_mux);
    }
}

void terminal_ui_clear(void)
{
    if (!s_lvgl_mux) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_rows, 0, sizeof(s_rows));
        s_active_row = 0;
        s_prompt_active = false;
        s_prompt_start_row = 0;
        s_prompt_num_rows = 1;
        update_cursor_visual_internal(false);
        refresh_all_rows_internal();
        xSemaphoreGive(s_lvgl_mux);
    }
}

void terminal_ui_push_line(const char *text, term_line_style_t style)
{
    if (!s_lvgl_mux || !text) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool scrolled = false;
        if (s_prompt_active) {
            // Clear any continuation rows of the active prompt before overwriting
            for (int r = s_prompt_start_row + 1; r < s_prompt_start_row + s_prompt_num_rows; r++) {
                if (r >= 0 && r < TERM_ROWS) {
                    s_rows[r].text[0] = '\0';
                    s_rows[r].style = TERM_STYLE_NORMAL;
                    apply_row_visual(r);
                }
            }
            s_active_row = s_prompt_start_row;
            s_prompt_active = false;
            s_prompt_num_rows = 1;
            update_cursor_visual_internal(false);
        } else if (s_active_row > 0 || strlen(s_rows[0].text) > 0) {
            if (s_active_row < TERM_ROWS - 1) {
                s_active_row++;
            } else {
                scroll_up_one_line_internal();
                scrolled = true;
            }
        }

        // Strict clamp to TERM_COLS (52 chars) for non-recolor lines
        size_t max_copy = (style == TERM_STYLE_PROMPT) ? (sizeof(s_rows[0].text) - 1) : TERM_COLS;
        strncpy(s_rows[s_active_row].text, text, max_copy);
        s_rows[s_active_row].text[max_copy] = '\0';
        s_rows[s_active_row].style = style;
        if (scrolled) {
            refresh_all_rows_internal();
        } else {
            apply_row_visual(s_active_row);
        }

        xSemaphoreGive(s_lvgl_mux);
    }
}

void terminal_ui_set_line(int row, const char *text, term_line_style_t style)
{
    if (!s_lvgl_mux || !text || row < 0 || row >= TERM_ROWS) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t max_copy = (style == TERM_STYLE_PROMPT) ? (sizeof(s_rows[0].text) - 1) : TERM_COLS;
        strncpy(s_rows[row].text, text, max_copy);
        s_rows[row].text[max_copy] = '\0';
        s_rows[row].style = style;
        apply_row_visual(row);
        xSemaphoreGive(s_lvgl_mux);
    }
}

int terminal_ui_get_active_row(void)
{
    return s_active_row;
}

void terminal_ui_start_prompt(const char *cwd_short)
{
    if (!s_lvgl_mux) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (cwd_short && strlen(cwd_short) > 0) {
            strncpy(s_current_cwd, cwd_short, sizeof(s_current_cwd) - 1);
            s_current_cwd[sizeof(s_current_cwd) - 1] = '\0';
        }
        s_current_cmd[0] = '\0';

        bool scrolled = false;
        if (!s_prompt_active) {
            if (s_active_row > 0 || strlen(s_rows[0].text) > 0) {
                if (s_active_row < TERM_ROWS - 1) {
                    s_active_row++;
                } else {
                    scroll_up_one_line_internal();
                    scrolled = true;
                }
            }
            s_prompt_active = true;
            s_prompt_start_row = s_active_row;
            s_prompt_num_rows = 1;
        }

        // Keep cursor solid while starting/typing and reset blink period
        s_cursor_visible = true;
        if (s_cursor_timer) {
            lv_timer_reset(s_cursor_timer);
        }

        render_prompt_multiline_internal(true);
        if (scrolled) {
            refresh_all_rows_internal();
        }
        xSemaphoreGive(s_lvgl_mux);
    }
}

void terminal_ui_update_command(const char *cmd_text, bool show_cursor)
{
    if (!s_lvgl_mux || !cmd_text) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool scrolled = false;
        if (!s_prompt_active) {
            if (s_active_row < TERM_ROWS - 1) {
                s_active_row++;
            } else {
                scroll_up_one_line_internal();
                scrolled = true;
            }
            s_prompt_active = true;
            s_prompt_start_row = s_active_row;
            s_prompt_num_rows = 1;
        }
        strncpy(s_current_cmd, cmd_text, sizeof(s_current_cmd) - 1);
        s_current_cmd[sizeof(s_current_cmd) - 1] = '\0';

        // Keep cursor solid during active typing / backspacing so blink never fights keystrokes!
        s_cursor_visible = true;
        if (s_cursor_timer) {
            lv_timer_reset(s_cursor_timer);
        }

        render_prompt_multiline_internal(show_cursor);
        if (scrolled) {
            refresh_all_rows_internal();
        }
        xSemaphoreGive(s_lvgl_mux);
    }
}

void terminal_ui_finish_command(const char *final_cmd_text)
{
    if (!s_lvgl_mux) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (final_cmd_text) {
            strncpy(s_current_cmd, final_cmd_text, sizeof(s_current_cmd) - 1);
            s_current_cmd[sizeof(s_current_cmd) - 1] = '\0';
        }
        render_prompt_multiline_internal(false);
        s_prompt_active = false;
        update_cursor_visual_internal(false);
        xSemaphoreGive(s_lvgl_mux);
    }
}

void terminal_ui_enter_alt_screen(void)
{
    if (!s_lvgl_mux) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!s_in_alt_screen) {
            memcpy(s_saved_rows, s_rows, sizeof(s_rows));
            s_saved_active_row = s_active_row;
            s_in_alt_screen = true;
        }
        memset(s_rows, 0, sizeof(s_rows));
        s_active_row = 0;
        s_prompt_active = false;
        s_prompt_start_row = 0;
        s_prompt_num_rows = 1;
        update_cursor_visual_internal(false);
        refresh_all_rows_internal();
        xSemaphoreGive(s_lvgl_mux);
    }
}

void terminal_ui_exit_alt_screen(void)
{
    if (!s_lvgl_mux) return;
    if (xSemaphoreTake(s_lvgl_mux, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_in_alt_screen) {
            memcpy(s_rows, s_saved_rows, sizeof(s_rows));
            s_active_row = s_saved_active_row;
            s_in_alt_screen = false;
            s_prompt_active = false;
            s_prompt_start_row = s_active_row;
            s_prompt_num_rows = 1;
            update_cursor_visual_internal(false);
            refresh_all_rows_internal();
        }
        xSemaphoreGive(s_lvgl_mux);
    }
}

bool terminal_ui_is_alt_screen(void)
{
    return s_in_alt_screen;
}

int terminal_ui_get_scroll_count(void)
{
    return s_scroll_count;
}

void terminal_ui_reset_scroll_count(void)
{
    s_scroll_count = 0;
}
