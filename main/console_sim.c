#include "console_sim.h"
#include "terminal_ui.h"
#include "google_quota.h"
#include "weather_client.h"
#include "sntp_time.h"
#include "settings_mgr.h"
#include "wifi_portal.h"
#include "display_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_system.h"

static const char *TAG = "console_sim";

// Embedded source files via EMBED_TXTFILES in main/CMakeLists.txt
extern const uint8_t main_c_start[]         asm("_binary_embed_main_txt_start");
extern const uint8_t main_c_end[]           asm("_binary_embed_main_txt_end");
extern const uint8_t config_h_start[]       asm("_binary_config_h_start");
extern const uint8_t config_h_end[]         asm("_binary_config_h_end");
extern const uint8_t terminal_ui_c_start[]  asm("_binary_embed_terminal_ui_txt_start");
extern const uint8_t terminal_ui_c_end[]    asm("_binary_embed_terminal_ui_txt_end");
extern const uint8_t console_sim_c_start[]  asm("_binary_embed_console_sim_txt_start");
extern const uint8_t console_sim_c_end[]    asm("_binary_embed_console_sim_txt_end");
extern const uint8_t google_quota_c_start[] asm("_binary_embed_google_quota_txt_start");
extern const uint8_t google_quota_c_end[]   asm("_binary_embed_google_quota_txt_end");

typedef struct {
    const char *path;
    const char *start;
    const char *end;
} embed_file_t;

static const embed_file_t s_embed_files[] = {
    { "main/main.c",         (const char *)main_c_start,         (const char *)main_c_end },
    { "main/config.h",       (const char *)config_h_start,       (const char *)config_h_end },
    { "main/terminal_ui.c",  (const char *)terminal_ui_c_start,  (const char *)terminal_ui_c_end },
    { "main/console_sim.c",  (const char *)console_sim_c_start,  (const char *)console_sim_c_end },
    { "main/google_quota.c", (const char *)google_quota_c_start, (const char *)google_quota_c_end },
};
#define NUM_EMBED_FILES (sizeof(s_embed_files) / sizeof(s_embed_files[0]))

typedef enum {
    SIM_STATE_BOOT = 0,
    SIM_STATE_MINUTE_DATE_TYPE,
    SIM_STATE_MINUTE_DATE_EXEC,
    SIM_STATE_MINUTE_CURL_TYPE,
    SIM_STATE_MINUTE_CURL_EXEC,
    SIM_STATE_MINUTE_WX_TYPE,
    SIM_STATE_MINUTE_WX_EXEC,
    SIM_STATE_IDLE_PROMPT,
    SIM_STATE_SKIT_TYPING,
    SIM_STATE_SKIT_TYPO_PAUSE,
    SIM_STATE_SKIT_BACKSPACE,
    SIM_STATE_SKIT_HESITATE_CANCEL,
    SIM_STATE_SKIT_OUTPUT_LINES,
    SIM_STATE_SKIT_INLINE_PANIC,
    SIM_STATE_FULLSCREEN_APP,
    SIM_STATE_SCROLLOUT_TYPE,
    SIM_STATE_SCROLLOUT_DUMP
} sim_state_t;

typedef enum {
    FS_APP_NONE = 0,
    FS_APP_VIM,
    FS_APP_NETHACK,
    FS_APP_INVADERS,
    FS_APP_SNAKE,
    FS_APP_HTOP,
    FS_APP_MATRIX
} fullscreen_app_t;

static sim_state_t s_state = SIM_STATE_BOOT;
static int64_t s_next_action_ms = 0;
static int s_last_triggered_min = -1;
static int s_last_scrollout_min = -1;

// Typing buffer state (supports long multi-line commands up to 140 chars)
static char s_target_cmd[140] = "";
static char s_typed_cmd[140] = "";
static int  s_type_idx = 0;

// Human cadence & typo / backspace state
static char s_typo_cmd[140] = "";
static int  s_backspace_count = 0;
static bool s_did_typo_in_this_cmd = false;
static int  s_cmd_tempo_pct = 100;
static int  s_burst_remaining = 0;
static int  s_hesitate_erased_cnt = 0;
static int  s_bs_burst_remaining = 0;

// Multi-line output queue for skits (supports 34-row screen scroll-out at 63 cols)
#define MAX_OUT_QUEUE 40
static char s_out_lines[MAX_OUT_QUEUE][72];
static term_line_style_t s_out_styles[MAX_OUT_QUEUE];
static int s_out_count = 0;
static int s_out_idx = 0;
static int s_out_line_delay_ms = 75;

// Inline panic state (overwrite last N lines after brief pause)
static int s_inline_panic_lines = 0;

// Fullscreen app state
static fullscreen_app_t s_fs_app = FS_APP_NONE;
static int s_fs_step = 0;
static int s_fs_max_steps = 0;
static int s_fs_last_clock_sec = -1;
static bool s_pinned_top_clock_active = false;

// Scroll budget tracking:
// On a 34-row screen, `date` printed at row 33 can scroll up 31 times before falling off row 0!
static int s_max_scrolls_allowed = TERM_ROWS - 3;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t rand_range(uint32_t min_val, uint32_t max_val)
{
    if (max_val <= min_val) return min_val;
    return min_val + (esp_random() % (max_val - min_val + 1));
}

static void get_time_strings(char *full_date_out, size_t full_sz,
                             char *hms_out, size_t hms_sz,
                             struct tm *tm_out)
{
    time_t now;
    time(&now);
    struct tm tm_local = {0};
    localtime_r(&now, &tm_local);
    if (tm_local.tm_year < (2024 - 1900)) {
        int64_t up_sec = esp_timer_get_time() / 1000000;
        tm_local.tm_year = 2026 - 1900;
        tm_local.tm_mon = 10;
        tm_local.tm_mday = 7;
        tm_local.tm_hour = (12 + (up_sec / 3600)) % 24;
        tm_local.tm_min = (up_sec / 60) % 60;
        tm_local.tm_sec = up_sec % 60;
        tm_local.tm_wday = 6;
    }
    if (tm_out) *tm_out = tm_local;
    if (full_date_out && full_sz > 0) {
        strftime(full_date_out, full_sz, " %a %b %d %H:%M:%S %Z %Y (epoch:%s) ", &tm_local);
    }
    if (hms_out && hms_sz > 0) {
        strftime(hms_out, hms_sz, "%H:%M:%S", &tm_local);
    }
}

static int count_embed_lines(int file_idx)
{
    if (file_idx < 0 || file_idx >= (int)NUM_EMBED_FILES) return 0;
    const char *p = s_embed_files[file_idx].start;
    const char *end = s_embed_files[file_idx].end;
    int lines = 0;
    while (p < end && *p != '\0') {
        if (*p == '\n') lines++;
        p++;
    }
    return lines;
}

static int extract_embed_lines(int file_idx, int start_line, int count, bool with_line_num, term_line_style_t style)
{
    if (file_idx < 0 || file_idx >= (int)NUM_EMBED_FILES) return 0;
    const char *p = s_embed_files[file_idx].start;
    const char *end = s_embed_files[file_idx].end;
    int cur_line = 1;
    int extracted = 0;

    while (p < end && *p != '\0' && extracted < count && s_out_count < MAX_OUT_QUEUE) {
        const char *line_start = p;
        while (p < end && *p != '\0' && *p != '\n' && *p != '\r') {
            p++;
        }
        size_t len = (size_t)(p - line_start);

        if (cur_line >= start_line) {
            char raw[80];
            if (len >= sizeof(raw)) len = sizeof(raw) - 1;
            memcpy(raw, line_start, len);
            raw[len] = '\0';

            const char *trimmed = raw;
            if (strlen(raw) > 48 && strncmp(raw, "        ", 8) == 0) {
                trimmed = raw + 4;
            }

            if (with_line_num) {
                snprintf(s_out_lines[s_out_count], sizeof(s_out_lines[0]), "%3d:%-.47s", cur_line, trimmed);
            } else {
                snprintf(s_out_lines[s_out_count], sizeof(s_out_lines[0]), "%-.51s", trimmed);
            }
            s_out_styles[s_out_count] = style;
            s_out_count++;
            extracted++;
        }

        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
        cur_line++;
    }
    return extracted;
}

static int grep_embed_lines(int file_idx, const char *needle, int max_matches, term_line_style_t style)
{
    if (file_idx < 0 || file_idx >= (int)NUM_EMBED_FILES || !needle) return 0;
    const char *p = s_embed_files[file_idx].start;
    const char *end = s_embed_files[file_idx].end;
    int cur_line = 1;
    int matched = 0;

    while (p < end && *p != '\0' && matched < max_matches && s_out_count < MAX_OUT_QUEUE) {
        const char *line_start = p;
        while (p < end && *p != '\0' && *p != '\n' && *p != '\r') {
            p++;
        }
        size_t len = (size_t)(p - line_start);

        char raw[96];
        size_t copy_len = (len < sizeof(raw) - 1) ? len : (sizeof(raw) - 1);
        memcpy(raw, line_start, copy_len);
        raw[copy_len] = '\0';

        if (strstr(raw, needle) != NULL) {
            const char *trimmed = raw;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
            snprintf(s_out_lines[s_out_count], sizeof(s_out_lines[0]), "%3d: %-.46s", cur_line, trimmed);
            s_out_styles[s_out_count] = style;
            s_out_count++;
            matched++;
        }

        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
        cur_line++;
    }
    return matched;
}

static void queue_output_line(const char *text, term_line_style_t style)
{
    if (s_out_count >= MAX_OUT_QUEUE) return;
    strncpy(s_out_lines[s_out_count], text, sizeof(s_out_lines[0]) - 1);
    s_out_lines[s_out_count][sizeof(s_out_lines[0]) - 1] = '\0';
    s_out_styles[s_out_count] = style;
    s_out_count++;
}

static bool is_shifted_or_symbol(char c)
{
    return (c == '|' || c == '"' || c == '\'' || c == '~' || c == '$' ||
            c == '{' || c == '}' || c == '(' || c == ')' || c == '_' ||
            c == ':' || c == '?' || c == '*' || c == '&' || c == '<' ||
            c == '>' || c == '+' || c == '#' || c == '@' || c == '%' ||
            c == '^' || c == '=' || c == '\\');
}

// Compute realistic human inter-keystroke delay (in ms) after typing `buf[idx_typed]`
static uint32_t human_type_delay_ms(const char *buf, int idx_typed)
{
    char c = buf[idx_typed];
    char next_c = buf[idx_typed + 1];

    // End of command: human pauses briefly to glance at the command before pressing Enter
    if (next_c == '\0') {
        return rand_range(120, 360);
    }

    // Double / repeated character (e.g. "--", "ll", "ss", "&&", "00"): rapid double-tap!
    if (next_c == c) {
        return rand_range(14, 34);
    }

    uint32_t base_ms = 50;

    if (c == ' ') {
        // Just typed a space; transitioning to the next token/flag/pipe
        if (next_c == '-' || next_c == '|' || next_c == '>' || next_c == '"' || next_c == '\'') {
            // Thinking pause before a flag, pipe, redirection, or quoted string
            if (rand_range(1, 100) <= 28) {
                base_ms = rand_range(260, 560);
            } else {
                base_ms = rand_range(110, 250);
            }
        } else if (rand_range(1, 100) <= 18) {
            base_ms = rand_range(190, 410);
        } else {
            base_ms = rand_range(55, 145);
        }
        s_burst_remaining = 0;
    } else if (is_shifted_or_symbol(next_c)) {
        // Reaching for Shift + symbol or pinky key
        base_ms = rand_range(80, 195);
        s_burst_remaining = 0;
    } else if (next_c == '/' || next_c == '.' || next_c == '-') {
        // Path separator or flag dash
        base_ms = rand_range(55, 140);
        s_burst_remaining = 0;
    } else if (next_c >= '0' && next_c <= '9') {
        // Reaching up to the number row (faster if already typing digits)
        if (c >= '0' && c <= '9') {
            base_ms = rand_range(25, 68);
        } else {
            base_ms = rand_range(75, 175);
        }
        s_burst_remaining = 0;
    } else if (c >= 'a' && c <= 'z' && next_c >= 'a' && next_c <= 'z') {
        // Consecutive lowercase letters: alternate between muscle-memory bursts and normal rolls
        if (s_burst_remaining > 0) {
            s_burst_remaining--;
            base_ms = rand_range(15, 38);
        } else {
            uint32_t roll = rand_range(1, 100);
            if (roll <= 35) {
                // Start a 3..6 char muscle-memory burst
                s_burst_remaining = (int)rand_range(2, 5);
                base_ms = rand_range(18, 42);
            } else if (roll <= 88) {
                base_ms = rand_range(36, 96);
            } else {
                // Occasional mid-word finger hesitation
                base_ms = rand_range(135, 290);
            }
        }
    } else {
        base_ms = rand_range(42, 115);
    }

    uint32_t scaled = (base_ms * (uint32_t)s_cmd_tempo_pct) / 100;
    if (scaled < 12) scaled = 12;
    return scaled;
}

// Compute realistic human Backspace cadence when erasing a long line:
// Initial discrete taps -> OS key-repeat hold delay -> rapid key-repeat burst -> mid-line release/pause -> final careful taps
static uint32_t human_hesitate_bs_delay_ms(int remaining_chars, char erased_char)
{
    s_hesitate_erased_cnt++;

    // Last 1..3 characters at the start of the prompt: release hold and tap carefully so terminal bell doesn't ring
    if (remaining_chars <= 3) {
        s_bs_burst_remaining = 0;
        return rand_range(70, 155);
    }

    // First 1..3 characters erased: discrete manual taps on Backspace key
    if (s_hesitate_erased_cnt == 1) {
        return rand_range(95, 185);
    }
    if (s_hesitate_erased_cnt == 2) {
        // Either one more manual tap or immediately hold down Backspace (OS auto-repeat initial delay)
        if (rand_range(0, 1) == 0) {
            return rand_range(75, 145);
        } else {
            s_bs_burst_remaining = (int)rand_range(8, 16);
            return rand_range(230, 360);
        }
    }
    if (s_hesitate_erased_cnt == 3 && s_bs_burst_remaining == 0) {
        // Hold down Backspace: OS key-repeat delay before fast repeat starts
        s_bs_burst_remaining = (int)rand_range(9, 18);
        return rand_range(220, 350);
    }

    // Currently holding down Backspace (or in a fast tap burst)
    if (s_bs_burst_remaining > 0) {
        s_bs_burst_remaining--;
        // Occasionally release early at a space or slash as if reconsidering whether to keep the prefix
        if ((erased_char == ' ' || erased_char == '/') && remaining_chars > 6 && rand_range(1, 100) <= 22) {
            s_bs_burst_remaining = 0;
            return rand_range(210, 480);
        }
        if (s_bs_burst_remaining == 0) {
            // Released Backspace briefly mid-line before tapping/holding again
            return rand_range(160, 420);
        }
        // Fast OS key-repeat rate with slight jitter
        return rand_range(15, 32);
    }

    // Starting a new burst or doing a couple of manual taps mid-line
    if (rand_range(1, 100) <= 65) {
        // Hold down Backspace again (short re-press delay then fast burst)
        s_bs_burst_remaining = (int)rand_range(6, 15);
        return rand_range(140, 260);
    }
    return rand_range(55, 130);
}

static void begin_typing_command(const char *cmd, bool allow_typo)
{
    strncpy(s_target_cmd, cmd, sizeof(s_target_cmd) - 1);
    s_target_cmd[sizeof(s_target_cmd) - 1] = '\0';
    s_typed_cmd[0] = '\0';
    s_type_idx = 0;
    s_did_typo_in_this_cmd = false;
    s_cmd_tempo_pct = (int)rand_range(68, 135);
    s_burst_remaining = 0;
    s_hesitate_erased_cnt = 0;
    s_bs_burst_remaining = 0;

    size_t len = strlen(cmd);
    if (allow_typo && len >= 8 && rand_range(1, 100) <= 30) {
        int typo_pos = (int)rand_range(2, (uint32_t)(len > 18 ? 14 : len - 3));
        int num_wrong = (int)rand_range(1, 3);
        memcpy(s_typo_cmd, cmd, typo_pos);
        char c0 = cmd[typo_pos];
        char w1 = (c0 >= 'a' && c0 < 'z') ? (c0 + 1) : 'k';
        char w2 = (c0 == 'e') ? 'r' : 'e';
        char w3 = (c0 == 'i') ? 'o' : 'i';
        s_typo_cmd[typo_pos] = w1;
        if (num_wrong >= 2) s_typo_cmd[typo_pos + 1] = w2;
        if (num_wrong >= 3) s_typo_cmd[typo_pos + 2] = w3;
        s_typo_cmd[typo_pos + num_wrong] = '\0';
        s_backspace_count = num_wrong;
    } else {
        s_typo_cmd[0] = '\0';
        s_backspace_count = 0;
    }

    terminal_ui_start_prompt("~/agy");
}

// ============================================================================
// Fullscreen Interactive TUI Renderers (52 cols x 30 rows)
// Always shows current live clock [HH:MM:SS] in the status bar!
// ============================================================================

static void update_fullscreen_clock_bar(void)
{
    char hms[16];
    get_time_strings(NULL, 0, hms, sizeof(hms), NULL);
    char bar[128];

    switch (s_fs_app) {
    case FS_APP_VIM:
        snprintf(bar, sizeof(bar), " NORMAL | main/console_sim.c [+] 42,%d | [%s]",
                 10 + s_fs_step, hms);
        terminal_ui_set_line(TERM_ROWS - 2, bar, TERM_STYLE_STATUS_BAR);
        break;
    case FS_APP_NETHACK:
        snprintf(bar, sizeof(bar), " Ken the Hacker St:18/02 HP:%2d(24) AC:3 Dlvl:5 [%s]",
                 20 - (s_fs_step % 9), hms);
        terminal_ui_set_line(TERM_ROWS - 1, bar, TERM_STYLE_STATUS_BAR);
        break;
    case FS_APP_INVADERS:
        snprintf(bar, sizeof(bar), " === SPACE INVADERS === SCORE:%04d HI:2400 [%s]",
                 420 + s_fs_step * 30, hms);
        terminal_ui_set_line(0, bar, TERM_STYLE_STATUS_BAR);
        break;
    case FS_APP_SNAKE:
        snprintf(bar, sizeof(bar), " [SNAKE v1.4] LEN:%2d APPLES:%d SPEED:TURBO [%s]",
                 6 + (s_fs_step / 2), 4 + s_fs_step, hms);
        terminal_ui_set_line(0, bar, TERM_STYLE_STATUS_BAR);
        break;
    case FS_APP_HTOP:
        snprintf(bar, sizeof(bar), " htop - cyd-esp32 (Xtensa LX6 240MHz)  Up [%s]", hms);
        terminal_ui_set_line(0, bar, TERM_STYLE_STATUS_BAR);
        break;
    case FS_APP_MATRIX:
        snprintf(bar, sizeof(bar), " [cmatrix -b] FOLLOW THE WHITE RABBIT...  [%s]", hms);
        terminal_ui_set_line(0, bar, TERM_STYLE_DATE_HIGHLIGHT);
        break;
    default:
        break;
    }
}

static void render_fullscreen_step(void)
{
    char buf[128];
    update_fullscreen_clock_bar();

    if (s_fs_app == FS_APP_VIM) {
        if (s_fs_step == 0) {
            s_out_count = 0;
            extract_embed_lines(3, 1, 24, true, TERM_STYLE_CODE);
            for (int r = 0; r < 24 && r < s_out_count; r++) {
                terminal_ui_set_line(r, s_out_lines[r], TERM_STYLE_CODE);
            }
            for (int r = 24; r < TERM_ROWS - 2; r++) {
                terminal_ui_set_line(r, "~", TERM_STYLE_QUOTA);
            }
            terminal_ui_set_line(TERM_ROWS - 1, "-- INSERT --", TERM_STYLE_NORMAL);
        } else if (s_fs_step <= 6) {
            static const char *vim_edits[] = {
                " 25: // TODO: look busy while compiling",
                " 25: // TODO: look busy while compiling kernel...",
                " 25: // wait why is the manager walking over here",
                " 25: // act natural, type random pointers *p++",
                " 25: // OH NO HE IS RIGHT BEHIND ME!!",
                " 25: // ESC ESC :q!"
            };
            terminal_ui_set_line(24, vim_edits[s_fs_step - 1], TERM_STYLE_GAME);
        } else if (s_fs_step == 7) {
            terminal_ui_set_line(TERM_ROWS - 1, ":q!  (discarding unsaved slacking notes)", TERM_STYLE_ALERT);
        }
    } else if (s_fs_app == FS_APP_NETHACK) {
        const char *log_msgs[] = {
            "You hear the footsteps of your project manager...",
            "The grid bug bites! You swing your +2 keyboard.",
            "The red dragon breathes fire! Your cloak singes!",
            "You quaff a potion of double espresso (+4 speed).",
            "Wait, someone is peeking at your terminal screen!",
            "Really save and quit? [yn] y (PANIC BOSS KEY!)"
        };
        int msg_idx = (s_fs_step < 6) ? s_fs_step : 5;
        terminal_ui_set_line(0, log_msgs[msg_idx],
                             (msg_idx >= 4) ? TERM_STYLE_ALERT : TERM_STYLE_CHAT);

        terminal_ui_set_line(2,  "    ----------------          Inventory:", TERM_STYLE_CODE);
        terminal_ui_set_line(3,  "    |......$.......|          a) +2 mech keyboard", TERM_STYLE_CODE);
        snprintf(buf, sizeof(buf), "    |...%s@....D....|          b) potion of coffee",
                 (s_fs_step % 2 == 0) ? "." : "");
        terminal_ui_set_line(4,  buf, TERM_STYLE_GAME);
        terminal_ui_set_line(5,  "    |....%.........+#######   c) scroll of git push", TERM_STYLE_CODE);
        terminal_ui_set_line(6,  "    ----------------      #   d) cursed Jira ticket", TERM_STYLE_CODE);
        terminal_ui_set_line(7,  "                          #", TERM_STYLE_CODE);
        terminal_ui_set_line(8,  "                   -------+----", TERM_STYLE_CODE);
        terminal_ui_set_line(9,  "                   |....<.....|", TERM_STYLE_CODE);
        terminal_ui_set_line(10, "                   |..d....*..|", TERM_STYLE_CODE);
        terminal_ui_set_line(11, "                   ------------", TERM_STYLE_CODE);
    } else if (s_fs_app == FS_APP_INVADERS) {
        int shift = s_fs_step % 7;
        char pad[10] = "";
        for (int i = 0; i < shift; i++) pad[i] = ' ';
        pad[shift] = '\0';

        snprintf(buf, sizeof(buf), "    %s<o>   \\m/   <o>   \\m/   <o>   \\m/   <o>", pad);
        terminal_ui_set_line(3, buf, TERM_STYLE_GAME);
        snprintf(buf, sizeof(buf), "    %s\\w/   <o>   \\w/   <o>   \\w/   <o>   \\w/", pad);
        terminal_ui_set_line(5, buf, TERM_STYLE_QUOTA);
        snprintf(buf, sizeof(buf), "    %s/v\\   /v\\   /v\\   /v\\   /v\\   /v\\   /v\\", pad);
        terminal_ui_set_line(7, buf, TERM_STYLE_CHAT);

        for (int r = 9; r <= 20; r++) {
            terminal_ui_set_line(r, "", TERM_STYLE_NORMAL);
        }
        int bullet_row = 20 - ((s_fs_step * 2) % 11);
        snprintf(buf, sizeof(buf), "                  %s!", pad);
        terminal_ui_set_line(bullet_row, buf, TERM_STYLE_ALERT);

        terminal_ui_set_line(21, "     [###]       [###]       [###]       [###]", TERM_STYLE_CODE);
        snprintf(buf, sizeof(buf), "                 %s/_^_\\", pad);
        terminal_ui_set_line(23, buf, TERM_STYLE_PROMPT);
        terminal_ui_set_line(25, " [SPACE] Fire   [H/L] Move   [^C] Instant Boss Key", TERM_STYLE_CODE);
        if (s_fs_step == s_fs_max_steps - 1) {
            terminal_ui_set_line(27, " ^C^C  [!] BOSS DETECTED - KILLED DISPLAY!", TERM_STYLE_ALERT);
        }
    } else if (s_fs_app == FS_APP_SNAKE) {
        terminal_ui_set_line(2, " +------------------------------------------------+", TERM_STYLE_CODE);
        for (int r = 3; r <= 20; r++) {
            terminal_ui_set_line(r, " |                                                |", TERM_STYLE_CODE);
        }
        terminal_ui_set_line(21, " +------------------------------------------------+", TERM_STYLE_CODE);

        int head_x = 8 + (s_fs_step * 3);
        if (head_x > 34) head_x = 34;
        char snake_line[56];
        memset(snake_line, ' ', sizeof(snake_line));
        snake_line[0] = ' ';
        snake_line[1] = '|';
        snake_line[50] = '|';
        snake_line[51] = '\0';
        const char *body = "ooooooooo@";
        memcpy(&snake_line[head_x], body, strlen(body));
        if (head_x + 11 < 48) {
            snake_line[42] = '*';
        }
        terminal_ui_set_line(11, snake_line, TERM_STYLE_GAME);

        if (s_fs_step == s_fs_max_steps - 1) {
            terminal_ui_set_line(24, " ^Z  [1]+ Stopped  python3 -m snake --fullscreen", TERM_STYLE_ALERT);
        }
    } else if (s_fs_app == FS_APP_HTOP) {
        uint32_t free_kb = esp_get_free_heap_size() / 1024;
        uint32_t used_kb = (320 > free_kb) ? (320 - free_kb) : 140;
        int cpu0 = 18 + (int)rand_range(0, 35);
        int cpu1 = 12 + (int)rand_range(0, 25);

        snprintf(buf, sizeof(buf), " 0[||||||||||||           %2d.4%%] Tasks: 14, 2 thr; 1 run", cpu0);
        terminal_ui_set_line(2, buf, TERM_STYLE_GAME);
        snprintf(buf, sizeof(buf), " 1[|||||||                %2d.1%%] Load avg: 0.18 0.11 0.07", cpu1);
        terminal_ui_set_line(3, buf, TERM_STYLE_GAME);
        snprintf(buf, sizeof(buf), " Mem[||||||||||||||| %3luK/320K] Swp[          0K/0K]", (unsigned long)used_kb);
        terminal_ui_set_line(4, buf, TERM_STYLE_QUOTA);

        terminal_ui_set_line(6,  "  PID USER     PRI  NI  RES S CPU% MEM%   TIME+ COMMAND", TERM_STYLE_STATUS_BAR);
        terminal_ui_set_line(7,  "   42 ken       20   0  48K R 14.2 15.0  0:12.4 console_sim", TERM_STYLE_NORMAL);
        terminal_ui_set_line(8,  "   18 ken       20   0  64K S  6.5 20.0  0:08.1 lvgl_gui_drv", TERM_STYLE_NORMAL);
        terminal_ui_set_line(9,  "   29 root      19  -1  32K S  2.1 10.0  0:03.9 agy_quotad", TERM_STYLE_NORMAL);
        terminal_ui_set_line(10, "   11 root      20   0  24K S  1.0  7.5  0:01.2 wpa_supplicant", TERM_STYLE_NORMAL);
        terminal_ui_set_line(11, "   33 www-data  20   0  16K S  0.4  5.0  0:00.5 httpd_portal", TERM_STYLE_CODE);
        terminal_ui_set_line(12, "    7 root     RT    0   8K S  0.2  2.5  0:04.0 esp_timer", TERM_STYLE_CODE);
        terminal_ui_set_line(13, "    1 root      20   0  12K S  0.0  3.7  0:00.1 /sbin/init", TERM_STYLE_CODE);
        terminal_ui_set_line(TERM_ROWS - 1, " F1Help F2Setup F3Search F5Tree F6Sort F9Kill F10Quit", TERM_STYLE_STATUS_BAR);
    } else if (s_fs_app == FS_APP_MATRIX) {
        static const char hex_chars[] = "0123456789ABCDEF$#@%&*+=~:;<>?/|\\";
        for (int r = 2; r < TERM_ROWS - 2; r++) {
            char mline[54];
            for (int c = 0; c < 50; c++) {
                if ((c % 2 == 0) && ((r + s_fs_step + c) % 4 != 0)) {
                    mline[c] = hex_chars[rand_range(0, sizeof(hex_chars) - 2)];
                } else {
                    mline[c] = ' ';
                }
            }
            mline[50] = '\0';
            terminal_ui_set_line(r, mline, (r % 5 == 0) ? TERM_STYLE_QUOTA : TERM_STYLE_GAME);
        }
        if (s_fs_step == s_fs_max_steps - 1) {
            terminal_ui_set_line(TERM_ROWS - 1, " ^C (clearing matrix rain before manager notices!)", TERM_STYLE_ALERT);
        }
    }
}

// ============================================================================
// Massive Command Tables: Uncorrected Typos, Windows Mistakes & Mundane Linux
// ============================================================================

typedef struct {
    const char *cmd;
    const char *out1;
    term_line_style_t style1;
    const char *out2;
    term_line_style_t style2;
} static_skit_t;

// 1) Uncorrected Typos Executed As-Is (human didn't notice before hitting Enter!)
static const static_skit_t s_uncorrected_typo_skits[] = {
    { "gti status",
      "bash: gti: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "git stauts",
      "git: 'stauts' is not a git command. See 'git --help'.", TERM_STYLE_ALERT,
      "The most similar command is: status", TERM_STYLE_CODE },
    { "git comit -m \"fix flicker\"",
      "git: 'comit' is not a git command. See 'git --help'.", TERM_STYLE_ALERT,
      "The most similar command is: commit", TERM_STYLE_CODE },
    { "git psuh origin main",
      "git: 'psuh' is not a git command. See 'git --help'.", TERM_STYLE_ALERT,
      "The most similar command is: push", TERM_STYLE_CODE },
    { "git brnach -a",
      "git: 'brnach' is not a git command. See 'git --help'.", TERM_STYLE_ALERT,
      "The most similar command is: branch", TERM_STYLE_CODE },
    { "git checkuot -b feat/wx",
      "git: 'checkuot' is not a git command. See 'git --help'.", TERM_STYLE_ALERT,
      "The most similar command is: checkout", TERM_STYLE_CODE },
    { "gerp -rn \"lv_font\" main/",
      "bash: gerp: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "grpe -i \"quota\" main/google_quota.c",
      "bash: grpe: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "pyhton3 -m http.server 8080",
      "bash: pyhton3: command not found (did you mean python3?)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "cd..",
      "bash: cd..: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "sduo systemctl restart agy_quotad",
      "bash: sduo: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "caat main/config.h",
      "bash: caat: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "mkdri -p build/cache",
      "bash: mkdri: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "chomd +x scripts/flash.sh",
      "bash: chomd: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "idf.py biuld",
      "esp-idf: error: action 'biuld' is not supported (did you mean 'build'?)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "idf.py menucnofig",
      "esp-idf: error: action 'menucnofig' is not supported", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "tial -n 20 /var/log/syslog",
      "bash: tial: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "cuel -s https://api.open-meteo.com/v1/forecast",
      "bash: cuel: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "whomi",
      "bash: whomi: command not found (did you mean whoami?)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "unmae -a",
      "bash: unmae: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { ":wq",
      "bash: :wq: command not found (you are not in vim!)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { ":q!",
      "bash: :q!: event not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "tar -xzf release_v1.2.tar.gz",
      "tar (child): release_v1.2.tar.gz: Cannot open: No such file or directory", TERM_STYLE_ALERT,
      "tar: Error is not recoverable: exiting now", TERM_STYLE_ALERT },
    { "namp -sP 192.168.1.0/24",
      "bash: namp: command not found (did you mean nmap?)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "dockre ps -a",
      "bash: dockre: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "apt-get isntall jq",
      "E: Invalid operation isntall", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL }
};
#define NUM_UNCORRECTED_TYPO_SKITS (sizeof(s_uncorrected_typo_skits) / sizeof(s_uncorrected_typo_skits[0]))

// 2) Accidental Windows / DOS / PowerShell Commands Typed on Linux!
static const static_skit_t s_windows_cmd_skits[] = {
    { "dir /w",
      "dir: cannot access '/w': No such file or directory", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "dir /s /b *.c",
      "dir: cannot access '/s': No such file or directory", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "ipconfig /all",
      "bash: ipconfig: command not found (use 'ip addr' or 'ifconfig')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "ipconfig /flushdns",
      "bash: ipconfig: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "cls",
      "bash: cls: command not found (did you mean 'clear'?)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "tracert 8.8.8.8",
      "bash: tracert: command not found (use 'traceroute' or 'mtr')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "tasklist /fi \"imagename eq agy*\"",
      "bash: tasklist: command not found (use 'ps aux' or 'pgrep')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "taskkill /F /IM agy_quotad.exe",
      "bash: taskkill: command not found (use 'kill -9' or 'pkill')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "findstr /s /i \"weather\" main\\*.c",
      "bash: findstr: command not found (use 'grep -rn')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "del /f /q build\\*.bin",
      "bash: del: command not found (use 'rm -f')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "copy sdkconfig sdkconfig.bak",
      "bash: copy: command not found (use 'cp')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "ren main\\config.h config_old.h",
      "bash: ren: command not found (use 'mv')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "type main\\console_sim.c",
      "bash: type: main\\console_sim.c: not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "Get-Process | Sort-Object CPU -Descending",
      "bash: Get-Process: command not found (PowerShell is not installed)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "Get-ChildItem -Path main -Filter *.c",
      "bash: Get-ChildItem: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "Select-String -Pattern \"TODO\" main/*.c",
      "bash: Select-String: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "notepad main/config.h",
      "bash: notepad: command not found (try 'vim' or 'nano')", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "explorer.exe .",
      "bash: explorer.exe: command not found (not running on WSL!)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "systeminfo | findstr /B /C:\"OS Name\"",
      "bash: systeminfo: command not found", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL },
    { "ping -t 192.168.1.1",
      "ping: invalid option -- 't' (on Linux ping runs until Ctrl-C by default)", TERM_STYLE_ALERT, NULL, TERM_STYLE_NORMAL }
};
#define NUM_WINDOWS_CMD_SKITS (sizeof(s_windows_cmd_skits) / sizeof(s_windows_cmd_skits[0]))

// 3) Massive Pool of Mundane, Everyday Linux / Git / Dev / Network / Hardware Commands
static const static_skit_t s_mundane_cmd_skits[] = {
    { "pwd",
      "/home/ken/git/esp_consoleclock", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "whoami && id -un",
      "ken", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "id",
      "uid=1000(ken) gid=1000(ken) groups=1000(ken),20(dialout),27(sudo)", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "hostname -f",
      "cyd-esp32.localdomain", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "ls -lh build/esp_consoleclock.bin",
      "-rw-r--r-- 1 ken ken 948K Nov 07 22:45 build/esp_consoleclock.bin", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "df -h / /boot/firmware",
      "Filesystem      Size  Used Avail Use% Mounted on", TERM_STYLE_CODE,
      "/dev/mtdblock3  3.0M  948K  2.1M  32% /", TERM_STYLE_NORMAL },
    { "du -sh build/ main/ managed_components/",
      "84M\tbuild/    156K\tmain/    24M\tmanaged_components/", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "mount | grep -E '(spiffs|nvs|proc)'",
      "nvs on /dev/mtdblock1 type nvs (rw,noatime) | proc on /proc type proc (rw)", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "lsblk -o NAME,SIZE,TYPE,MOUNTPOINT",
      "mtdblock0  4M  rom  | mtdblock1 24K part [nvs] | mtdblock3 3M part /", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "cat /proc/version",
      "Linux version 6.8.0-xtensa-lx6 (gcc 13.2.0 (crosstool-NG esp-13.2.0)) #1 SMP PREEMPT", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "cat /proc/loadavg",
      "0.14 0.09 0.05 1/18 412", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "grep -E '^(model name|cpu MHz|BogoMIPS)' /proc/cpuinfo | head -n 2",
      "model name : Tensilica Xtensa LX6 dual-core @ 240.000 MHz (480.00 BogoMIPS)", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "cat /etc/os-release | head -n 2",
      "PRETTY_NAME=\"ESP-IDF v5.3.1 / Debian GNU/Linux 12 (bookworm) xtensa\"", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "cat /etc/resolv.conf",
      "nameserver 192.168.1.1   nameserver 8.8.8.8   options edns0 trust-ad", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "cat /etc/hosts | head -n 2",
      "127.0.0.1 localhost   192.168.1.10 workstation.local   192.168.4.1 cyd-setup", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "ip route show default",
      "default via 192.168.1.1 dev wlan0 proto dhcp src 192.168.1.48 metric 100", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "ip neigh show dev wlan0",
      "192.168.1.1 lladdr 74:da:38:bc:11:02 REACHABLE | 192.168.1.10 lladdr a8:a1:59:40:1f:9e STALE", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "iw dev wlan0 link | head -n 2",
      "Connected to 74:da:38:bc:11:02 (on wlan0)  freq: 2437  signal: -54 dBm  tx: 72.2 MBit/s", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "dig +short api.open-meteo.com A",
      "94.130.142.35", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "dig +short daily-cloudcode-pa.googleapis.com A",
      "142.250.196.106", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "curl -sI https://api.open-meteo.com/v1/forecast | head -n 2",
      "HTTP/2 200   content-type: application/json; charset=utf-8", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "ntpstat",
      "synchronised to NTP server (216.239.35.0) at stratum 2 | time correct to within 4 ms", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "chronyc tracking | head -n 2",
      "Reference ID    : D8EF2300 (time.google.com)   System time : 0.000142819 seconds fast", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "git branch -vv",
      "* main 9b8c4e1 [origin/main: ahead 1] feat: 5x7 font (63x34), weather curl & 140+ cmds", TERM_STYLE_GAME, NULL, TERM_STYLE_NORMAL },
    { "git remote -v",
      "origin\tgit@github.com:ken/esp_consoleclock.git (fetch/push)", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "git stash list",
      "stash@{0}: WIP on main: e71b409 experiment with 4x6 pixel glyphs", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "git tag -l -n1",
      "v0.1.0  Initial CYD console clock   v0.2.0  5x7 dense font & Open-Meteo", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "git log --stat -n 1 --oneline",
      "9b8c4e1 feat: 5x7 font, weather curl & command expansion | 8 files changed, 840 insertions(+)", TERM_STYLE_CHAT, NULL, TERM_STYLE_NORMAL },
    { "git reflog -n 2",
      "9b8c4e1 HEAD@{0}: commit: feat: 5x7 font & weather curl | e71b409 HEAD@{1}: commit: fix backspace", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "git config --list | grep -E '^(user|core\\.editor)'",
      "user.name=ken   user.email=ken@cyd.local   core.editor=vim", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "idf.py --version",
      "ESP-IDF v5.3.1-dirty", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "idf.py size-components | head -n 3",
      "Archive File                      DRAM .data & .bss   IRAM   Flash code & rodata   Total", TERM_STYLE_CODE,
      "liblvgl.a                                     11240      0                174210  185450", TERM_STYLE_NORMAL },
    { "esptool.py --port /dev/ttyUSB0 chip_id",
      "esptool.py v4.7.0 | Chip is ESP32-D0WD-V3 (revision v3.1) | Features: WiFi, BT, Dual Core, 240MHz", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "xtensa-esp32-elf-gcc --version | head -n 1",
      "xtensa-esp32-elf-gcc (crosstool-NG esp-13.2.0_20230928) 13.2.0", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "cmake --version | head -n 1 && ninja --version",
      "cmake version 3.28.3  |  1.11.1", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "python3 --version && jq --version",
      "Python 3.11.9  |  jq-1.7.1", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "env | grep -E '^(TERM|SHELL|IDF_PATH|LANG)'",
      "TERM=xterm-256color  SHELL=/bin/bash  IDF_PATH=/home/ken/esp/esp-idf  LANG=en_US.UTF-8", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "alias",
      "alias ll='ls -alF'  alias gs='git status -sb'  alias gd='git diff'  alias idf='idf.py'", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "history | tail -n 2",
      "  498  curl -s https://api.open-meteo.com/v1/forecast | jq -c", TERM_STYLE_CODE,
      "  499  idf.py -p /dev/ttyUSB0 flash monitor", TERM_STYLE_CODE },
    { "jobs -l",
      "[1]+    29 Running                 ./agy_quotad --daemon &", TERM_STYLE_NORMAL,
      "[2]-    31 Running                 ./weather_client --poll 900s &", TERM_STYLE_NORMAL },
    { "ulimit -a | grep -E '(stack|open files)'",
      "stack size (kbytes, -s) 8192  |  open files (-n) 1024", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "vmstat 1 1",
      "procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----", TERM_STYLE_CODE,
      " r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st", TERM_STYLE_NORMAL },
    { "lsmod | head -n 3",
      "Module                  Size  Used by", TERM_STYLE_CODE,
      "ili9341_spi            16384  1   |   xpt2046_touch  12288  1   |   esp32_wifi  143360  2", TERM_STYLE_NORMAL },
    { "lsusb",
      "Bus 001 Device 004: ID 1a86:7523 QinHeng Electronics CH340 serial converter", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "systemctl list-timers --no-pager | head -n 2",
      "NEXT                        LEFT     LAST                        PASSED   UNIT", TERM_STYLE_CODE,
      "Sat 2026-11-07 23:00:00 JST 12min    Sat 2026-11-07 22:45:00 JST 3min ago agy-wx.timer", TERM_STYLE_NORMAL },
    { "journalctl -u agy_quotad -n 2 --no-pager",
      "Nov 07 22:45:01 cyd-esp32 agy_quotad[29]: [HTTP 200] retrieveUserQuotaSummary ok (184ms)", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "crontab -l | grep -v '^#'",
      "*/15 * * * * /usr/local/bin/weather_sync --quiet", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "w -h",
      "ken      pts/0    192.168.1.10     21:04    0.00s  0.14s  0.02s ./console_sim", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "last -n 2",
      "ken      pts/0        192.168.1.10     Sat Nov  7 21:04   still logged in", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "find main/ -maxdepth 1 -name '*.c' | paste -sd ' ' -",
      "main/console_sim.c main/display_driver.c main/google_quota.c main/main.c main/terminal_ui.c main/weather_client.c", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "file build/esp_consoleclock.elf",
      "build/esp_consoleclock.elf: ELF 32-bit LSB executable, Tensilica Xtensa, version 1 (SYSV), statically linked", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "nm -S --size-sort build/esp_consoleclock.elf | tail -n 2",
      "3ffb8420 000009c0 b s_lines   |   3f408100 00001a40 r glyph_bitmap_5x7", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "openssl version -a | head -n 1",
      "OpenSSL 3.0.14 4 Jun 2024 (Library: OpenSSL 3.0.14 / mbedTLS 3.6.0)", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "echo 'scale=10; 4*a(1)' | bc -l",
      "3.1415926532", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "python3 -c \"import math,time; print(f'epoch={int(time.time())} tau={math.tau:.6f}')\"",
      "epoch=1762523280 tau=6.283185", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL },
    { "gpio readall | grep -E '(MOSI|SCLK|CS|DC|BL|IRQ)'",
      "GPIO13:MOSI(1)  GPIO14:SCLK(0)  GPIO15:CS(1)  GPIO2:DC(1)  GPIO21:BL(PWM)  GPIO36:IRQ(1)", TERM_STYLE_CODE, NULL, TERM_STYLE_NORMAL },
    { "i2cdetect -y 0 | head -n 2",
      "     0  1  2  3  4  5  6  7  8  9  a  b  c  d  e  f", TERM_STYLE_CODE,
      "00:          -- -- -- -- -- -- -- -- -- -- -- -- --", TERM_STYLE_CODE },
    { "vcgencmd measure_temp || cat /sys/class/thermal/thermal_zone0/temp",
      "44800 (44.8 degC)", TERM_STYLE_NORMAL, NULL, TERM_STYLE_NORMAL }
};
#define NUM_MUNDANE_CMD_SKITS (sizeof(s_mundane_cmd_skits) / sizeof(s_mundane_cmd_skits[0]))

// ============================================================================
// Pick a random skit for :05 to :57 (150+ diverse scenarios!)
// ============================================================================
static void pick_and_start_random_skit(int sec_now)
{
    s_out_count = 0;
    s_out_idx = 0;
    s_out_line_delay_ms = 80;
    s_inline_panic_lines = 0;

    int scrolls_so_far = terminal_ui_get_scroll_count();
    int budget_left = s_max_scrolls_allowed - scrolls_so_far;

    if (budget_left < 4 && !s_pinned_top_clock_active) {
        uint32_t low_choice = rand_range(0, 5);
        if (low_choice <= 2) {
            terminal_ui_start_prompt("~/agy");
            s_state = SIM_STATE_IDLE_PROMPT;
            s_next_action_ms = now_ms() + rand_range(2500, 5500);
            return;
        } else if (low_choice <= 4) {
            static const char *hesitate_cmds[] = {
                "rm -rf build/ managed_components/ sdkconfig",
                "git push --force-with-lease origin main",
                "curl -X POST https://prod.internal/api/v2/rollback",
                "killall -9 agy_quotad wpa_supplicant",
                "dd if=/dev/urandom of=/dev/mtdblock0 bs=4k",
                "wall 'going home early today, server is on fire'"
            };
            const char *c = hesitate_cmds[rand_range(0, 5)];
            begin_typing_command(c, false);
            s_state = SIM_STATE_SKIT_HESITATE_CANCEL;
            s_next_action_ms = now_ms() + 80;
            return;
        } else if (sec_now >= 46) {
            terminal_ui_start_prompt("~/agy");
            s_state = SIM_STATE_IDLE_PROMPT;
            s_next_action_ms = now_ms() + 2000;
            return;
        }
    }

    // Choose category with weighted probabilities so the user sees huge variety:
    // - 0..7    (8%):  Fullscreen TUI Apps (Vim, NetHack, Invaders, Snake, htop, Matrix)
    // - 8..21   (14%): Self-referential EMBED_TXTFILES source inspection & diffs
    // - 22..37  (16%): Uncorrected Typos Executed As-Is -> bash/git/idf errors (26 commands)
    // - 38..51  (14%): Accidental Windows / DOS / PowerShell commands -> errors (20 commands)
    // - 52..79  (28%): Massive Pool of Mundane Linux / Git / Dev / Net commands (58 commands)
    // - 80..85  (6%):  Secret Messaging / Chatting with colleagues
    // - 86..90  (5%):  Inline Mini-Game / Toy + Panic In-Place Erase
    // - 91..94  (4%):  Idle cursor blinking OR hesitation backspace cancel
    // - 95..99  (5%):  Live ESP32 dynamic telemetry (heap, uptime, IP, sensors)
    uint32_t roll = (budget_left < 4 && !s_pinned_top_clock_active) ? rand_range(0, 7) : rand_range(0, 99);

    // 1. Fullscreen TUI Apps (Vim, NetHack, Invaders, Snake, htop, Matrix)
    if (roll <= 7 && sec_now < 46) {
        uint32_t app_pick = rand_range(0, 5);
        switch (app_pick) {
        case 0:
            begin_typing_command("vim +42 main/console_sim.c", true);
            s_fs_app = FS_APP_VIM;
            s_fs_max_steps = 8;
            break;
        case 1:
            begin_typing_command("nethack -u ken -DEC", false);
            s_fs_app = FS_APP_NETHACK;
            s_fs_max_steps = 6;
            break;
        case 2:
            begin_typing_command("./invaders --fullscreen --nosound", false);
            s_fs_app = FS_APP_INVADERS;
            s_fs_max_steps = 7;
            break;
        case 3:
            begin_typing_command("python3 -m snake --speed turbo", true);
            s_fs_app = FS_APP_SNAKE;
            s_fs_max_steps = 6;
            break;
        case 4:
            begin_typing_command("htop -d 10", false);
            s_fs_app = FS_APP_HTOP;
            s_fs_max_steps = 5;
            break;
        case 5:
        default:
            begin_typing_command("cmatrix -b -u 4 -C green", false);
            s_fs_app = FS_APP_MATRIX;
            s_fs_max_steps = 6;
            break;
        }
        s_fs_step = 0;
        s_state = SIM_STATE_SKIT_TYPING;
        s_next_action_ms = now_ms() + 60;
        return;
    }

    s_fs_app = FS_APP_NONE;

    // 2. Uncorrected Typos Executed As-Is (no backspace correction -> bash/git error!)
    if (roll >= 22 && roll <= 37) {
        uint32_t idx = rand_range(0, NUM_UNCORRECTED_TYPO_SKITS - 1);
        const static_skit_t *sk = &s_uncorrected_typo_skits[idx];
        begin_typing_command(sk->cmd, false); // false so the intentional typo in sk->cmd is typed & executed as-is!
        if (sk->out1) queue_output_line(sk->out1, sk->style1);
        if (sk->out2) queue_output_line(sk->out2, sk->style2);
        s_state = SIM_STATE_SKIT_TYPING;
        s_next_action_ms = now_ms() + 60;
        return;
    }

    // 3. Accidental Windows / DOS / PowerShell Commands
    if (roll >= 38 && roll <= 51) {
        uint32_t idx = rand_range(0, NUM_WINDOWS_CMD_SKITS - 1);
        const static_skit_t *sk = &s_windows_cmd_skits[idx];
        begin_typing_command(sk->cmd, false);
        if (sk->out1) queue_output_line(sk->out1, sk->style1);
        if (sk->out2) queue_output_line(sk->out2, sk->style2);
        s_state = SIM_STATE_SKIT_TYPING;
        s_next_action_ms = now_ms() + 60;
        return;
    }

    // 4. Massive Pool of Mundane Everyday Linux / Git / Dev / Net Commands
    if (roll >= 52 && roll <= 79) {
        uint32_t idx = rand_range(0, NUM_MUNDANE_CMD_SKITS - 1);
        const static_skit_t *sk = &s_mundane_cmd_skits[idx];
        begin_typing_command(sk->cmd, true);
        if (sk->out1) queue_output_line(sk->out1, sk->style1);
        if (sk->out2) queue_output_line(sk->out2, sk->style2);
        s_state = SIM_STATE_SKIT_TYPING;
        s_next_action_ms = now_ms() + 60;
        return;
    }

    // 5. Self-referential EMBED_TXTFILES source inspection & diffs
    if (roll >= 8 && roll <= 21) {
        int f_idx = (int)rand_range(0, NUM_EMBED_FILES - 1);
        int total_l = count_embed_lines(f_idx);
        int max_show = (budget_left > 7) ? 4 : 2;
        int start_l = (total_l > 20) ? (int)rand_range(1, total_l - 10) : 1;
        char cmd_buf[120];
        uint32_t sub = rand_range(6, 15);

        if (sub == 6 || sub == 7) {
            snprintf(cmd_buf, sizeof(cmd_buf), "sed -n '%d,%dp' %s",
                     start_l, start_l + max_show - 1, s_embed_files[f_idx].path);
            begin_typing_command(cmd_buf, true);
            extract_embed_lines(f_idx, start_l, max_show, false, TERM_STYLE_CODE);
        } else if (sub == 8) {
            snprintf(cmd_buf, sizeof(cmd_buf), "head -n %d %s", max_show, s_embed_files[f_idx].path);
            begin_typing_command(cmd_buf, true);
            extract_embed_lines(f_idx, 1, max_show, false, TERM_STYLE_CODE);
        } else if (sub == 9) {
            snprintf(cmd_buf, sizeof(cmd_buf), "cat -n %s | sed -n '%d,%dp'",
                     s_embed_files[f_idx].path, start_l, start_l + max_show - 1);
            begin_typing_command(cmd_buf, true);
            extract_embed_lines(f_idx, start_l, max_show, true, TERM_STYLE_CODE);
        } else if (sub == 10) {
            snprintf(cmd_buf, sizeof(cmd_buf), "grep -n \"esp_\" %s | head -n 3", s_embed_files[f_idx].path);
            begin_typing_command(cmd_buf, true);
            if (grep_embed_lines(f_idx, "esp_", 3, TERM_STYLE_CODE) == 0) {
                extract_embed_lines(f_idx, 1, 3, true, TERM_STYLE_CODE);
            }
        } else if (sub == 11) {
            begin_typing_command("wc -l main/*.c main/*.h", true);
            char line1[128], line2[128];
            snprintf(line1, sizeof(line1), " %4d main.c  %4d config.h  %4d weather_client.c",
                     count_embed_lines(0), count_embed_lines(1), 285);
            snprintf(line2, sizeof(line2), " %4d terminal_ui.c  %4d console_sim.c  %4d google_quota.c",
                     count_embed_lines(2), count_embed_lines(3), count_embed_lines(4));
            queue_output_line(line1, TERM_STYLE_NORMAL);
            queue_output_line(line2, TERM_STYLE_NORMAL);
        } else if (sub == 12) {
            begin_typing_command("git diff -U1 main/config.h", true);
            queue_output_line("@@ -26,3 +26,3 @@", TERM_STYLE_QUOTA);
            queue_output_line("-#define TERM_COLS 52", TERM_STYLE_ALERT);
            queue_output_line("+#define TERM_COLS 63", TERM_STYLE_GAME);
        } else if (sub == 13) {
            begin_typing_command("ls -la main/", true);
            queue_output_line("-rw-r--r-- 1 ken staff  4120 Nov 07 22:30 main.c", TERM_STYLE_NORMAL);
            queue_output_line("-rw-r--r-- 1 ken staff 10840 Nov 07 22:35 weather_client.c", TERM_STYLE_NORMAL);
            queue_output_line("-rw-r--r-- 1 ken staff 46200 Nov 07 22:45 console_sim.c", TERM_STYLE_NORMAL);
        } else if (sub == 14) {
            snprintf(cmd_buf, sizeof(cmd_buf), "sha256sum %s", s_embed_files[f_idx].path);
            begin_typing_command(cmd_buf, true);
            snprintf(cmd_buf, sizeof(cmd_buf), "%08lx%08lx9a4f..  %s",
                     (unsigned long)esp_random(), (unsigned long)esp_random(), s_embed_files[f_idx].path);
            queue_output_line(cmd_buf, TERM_STYLE_CODE);
        } else {
            begin_typing_command("xxd -g 1 -l 16 build/esp_consoleclock.bin", true);
            queue_output_line("00000000: e9 05 02 20 84 12 08 40 ee 00 00 00 09 1f 00 00  ... ...@........", TERM_STYLE_CODE);
        }
        s_state = SIM_STATE_SKIT_TYPING;
        s_next_action_ms = now_ms() + 60;
        return;
    }

    // 6. Secret Messaging / Chatting with colleagues
    if (roll >= 80 && roll <= 85) {
        switch (rand_range(16, 24)) {
        case 16:
            begin_typing_command("write alice pts/2 <<< \"coffee break in 5m?\"", true);
            queue_output_line("[sent 22B to alice@pts/2]", TERM_STYLE_CODE);
            queue_output_line("Message from alice@pts/2: \"heck yes, double shot!\"", TERM_STYLE_CHAT);
            break;
        case 17:
            begin_typing_command("wall \"who broke the esp32 build on main?!\"", true);
            queue_output_line("Broadcast message from ken@cyd (pts/0):", TERM_STYLE_CHAT);
            queue_output_line("Message from bob@pts/1: \"my bad, pushing fix now\"", TERM_STYLE_CHAT);
            break;
        case 18:
            begin_typing_command("irc-send #firmware \"5x7 font fits 63x34 chars!\"", true);
            queue_output_line("<@ci-bot> [esp_consoleclock #428] BUILD SUCCESS", TERM_STYLE_GAME);
            break;
        case 19:
            begin_typing_command("msg sarah \"did argon weekly quota reset yet?\"", true);
            queue_output_line("sarah@pts/3: \"check the curl one-liner above!\"", TERM_STYLE_CHAT);
            break;
        case 20:
            begin_typing_command("slack post #random \"ramen or curry for lunch?\"", true);
            queue_output_line("[@dave, @erin, @bob reacted with :ramen: :curry:]", TERM_STYLE_CHAT);
            break;
        case 21:
            begin_typing_command("mail -s \"PCB rev3 gerbers\" fab@lab.local < rev3.zip", true);
            queue_output_line("Queued 250 OK id=9A4F1C -> fab@lab.local (148KB)", TERM_STYLE_NORMAL);
            break;
        case 22:
            begin_typing_command("talk dave@192.168.1.55", true);
            queue_output_line("[Ringing your party again on 192.168.1.55...]", TERM_STYLE_CODE);
            queue_output_line("dave: \"stuck in sprint retro, save me!!\"", TERM_STYLE_CHAT);
            break;
        case 23:
            begin_typing_command("nc -u 192.168.1.255 9000 <<< \"anybody playing quake?\"", true);
            queue_output_line("192.168.1.42:9000 -> \"join 192.168.1.42:27960 after 6pm\"", TERM_STYLE_CHAT);
            break;
        case 24:
        default:
            begin_typing_command("echo \"debugging spi dma timing...\" > ~/.plan", true);
            queue_output_line("[~/.plan updated: 29 bytes written]", TERM_STYLE_CODE);
            break;
        }
        s_state = SIM_STATE_SKIT_TYPING;
        s_next_action_ms = now_ms() + 60;
        return;
    }

    // 7. Inline Mini-Game / Toy + Panic In-Place Erase!
    if (roll >= 86 && roll <= 90) {
        switch (rand_range(25, 31)) {
        case 25:
            begin_typing_command("./slots --bet 100 --turbo", false);
            queue_output_line(" [ BAR |  7  | CHERRY ]  -100 coins (bal: 400)", TERM_STYLE_GAME);
            queue_output_line(" [  7  |  7  |   7    ]  JACKPOT!! +5,000 coins!", TERM_STYLE_CHAT);
            s_inline_panic_lines = 2;
            break;
        case 26:
            begin_typing_command("cowsay \"act busy, someone is walking by\"", false);
            queue_output_line(" < act busy, someone is walking by >", TERM_STYLE_CHAT);
            queue_output_line("   \\   ^__^    (oo)\\_______//", TERM_STYLE_GAME);
            s_inline_panic_lines = 2;
            break;
        case 27:
            begin_typing_command("poker-cli --table high-roller --all-in", false);
            queue_output_line(" Hero: [As Ks]  Board: [Qs Js 10s 2d 9c]  Pot:$42k", TERM_STYLE_GAME);
            queue_output_line(" ROYAL FLUSH! Dealer pushes $42,000 to ken!", TERM_STYLE_CHAT);
            s_inline_panic_lines = 2;
            break;
        case 28:
            begin_typing_command("fortune -s", false);
            queue_output_line(" \"Never test for an error condition you don't handle.\"", TERM_STYLE_ALERT);
            s_inline_panic_lines = 1;
            break;
        case 29:
            begin_typing_command("factor 3133742069", false);
            queue_output_line("3133742069: 19 73 2259367", TERM_STYLE_GAME);
            s_inline_panic_lines = 1;
            break;
        case 30:
            begin_typing_command("rig | head -n 2", false);
            queue_output_line("Sherlock Holmes, 221B Baker St, London NW1 6XE", TERM_STYLE_CHAT);
            s_inline_panic_lines = 1;
            break;
        case 31:
        default:
            begin_typing_command("sl -alF", false);
            queue_output_line("    ====        ________                ___________ ", TERM_STYLE_GAME);
            queue_output_line("  _D _|  |_______/        \\__I_I_____===__|_________|", TERM_STYLE_GAME);
            s_inline_panic_lines = 2;
            break;
        }
        s_state = SIM_STATE_SKIT_TYPING;
        s_next_action_ms = now_ms() + 60;
        return;
    }

    // 8. Idle cursor blinking OR hesitation backspace cancel
    if (roll >= 91 && roll <= 94) {
        if (roll <= 92) {
            terminal_ui_start_prompt("~/agy");
            s_state = SIM_STATE_IDLE_PROMPT;
            s_next_action_ms = now_ms() + rand_range(3000, 6500);
        } else {
            static const char *cancel_cmds[] = {
                "sudo rm -rf --no-preserve-root /var/lib/docker",
                "git reset --hard HEAD~12 && git push -f",
                "steam -applaunch 427520 --windowed",
                "ssh -i ~/.ssh/id_ed25519 root@prod-db-master",
                "curl -s https://news.ycombinator.com | html2text"
            };
            begin_typing_command(cancel_cmds[rand_range(0, 4)], false);
            s_state = SIM_STATE_SKIT_HESITATE_CANCEL;
            s_next_action_ms = now_ms() + 70;
        }
        return;
    }

    // 9. Live ESP32 dynamic system, network & dev telemetry commands (63-char output!)
    char sys_buf[128];
    switch (rand_range(37, 47)) {
    case 37: {
        begin_typing_command("uptime", true);
        int64_t up_min = (esp_timer_get_time() / 1000000) / 60;
        char hms[16];
        get_time_strings(NULL, 0, hms, sizeof(hms), NULL);
        snprintf(sys_buf, sizeof(sys_buf), " %s up %lld min, 2 users, load average: 0.14, 0.09, 0.05",
                 hms, (long long)up_min);
        queue_output_line(sys_buf, TERM_STYLE_NORMAL);
        break;
    }
    case 38: {
        begin_typing_command("free -k", true);
        uint32_t free_k = esp_get_free_heap_size() / 1024;
        uint32_t min_k = esp_get_minimum_free_heap_size() / 1024;
        uint32_t used_k = (320 > free_k) ? (320 - free_k) : 120;
        queue_output_line("              total      used      free  min_free    cached", TERM_STYLE_CODE);
        snprintf(sys_buf, sizeof(sys_buf), "Mem:           320K     %4luK     %4luK     %4luK       24K",
                 (unsigned long)used_k, (unsigned long)free_k, (unsigned long)min_k);
        queue_output_line(sys_buf, TERM_STYLE_NORMAL);
        break;
    }
    case 39: {
        begin_typing_command("ip -4 -br addr show wlan0", true);
        if (wifi_portal_is_connected()) {
            snprintf(sys_buf, sizeof(sys_buf), "wlan0            UP             %s/24 metric 100", wifi_portal_get_ip());
        } else {
            snprintf(sys_buf, sizeof(sys_buf), "wlan0            AP             %s/24 ssid:%s", PORTAL_IP_ADDR, PORTAL_AP_SSID);
        }
        queue_output_line(sys_buf, TERM_STYLE_NORMAL);
        break;
    }
    case 40:
        begin_typing_command("git status -sb", true);
        queue_output_line("## main...origin/main [ahead 1]", TERM_STYLE_QUOTA);
        queue_output_line(" M main/console_sim.c   M main/weather_client.c   M main/config.h", TERM_STYLE_GAME);
        break;
    case 41:
        begin_typing_command("git log --oneline -n 2", true);
        queue_output_line("9b8c4e1 feat(ui): ultra-dense 5x7 font (63x34) & weather curl", TERM_STYLE_CHAT);
        queue_output_line("e71b409 feat(sim): human typing cadence & 150+ command skits", TERM_STYLE_CODE);
        break;
    case 42:
        begin_typing_command("uname -a", true);
        queue_output_line("Linux cyd-esp32 6.8.0-xtensa #1 SMP PREEMPT esp32 xtensa GNU/Linux", TERM_STYLE_NORMAL);
        break;
    case 43:
        begin_typing_command("ping -c 1 -W 1 8.8.8.8", true);
        queue_output_line("64 bytes from 8.8.8.8: icmp_seq=1 ttl=118 time=11.8 ms", TERM_STYLE_NORMAL);
        break;
    case 44:
        begin_typing_command("ss -tulnp | head -n 3", true);
        queue_output_line("Netid State  Local Addr:Port   Peer Addr:Port  Process", TERM_STYLE_CODE);
        queue_output_line("tcp   LISTEN 0.0.0.0:80        0.0.0.0:*       users:((\"httpd\",pid=33))", TERM_STYLE_NORMAL);
        break;
    case 45:
        begin_typing_command("sensors", true);
        queue_output_line("esp32_soc-isa-0000: vdd33: +3.31 V  cpu_temp: +44.8 C (crit = +105.0 C)", TERM_STYLE_NORMAL);
        break;
    case 46:
        begin_typing_command("dmesg | tail -n 2", true);
        queue_output_line("[ 1420.11] spi_master: SPI2 DMA @ 40MHz (ili9341/st7789 320x240)", TERM_STYLE_CODE);
        queue_output_line("[ 1421.04] mbedtls: ECDHE-RSA-AES128-GCM-SHA256 handshake ok", TERM_STYLE_CODE);
        break;
    case 47:
    default:
        begin_typing_command("ps aux | grep -E '(agy|lvgl|wx)'", true);
        queue_output_line("ken   42 14.2 15.0 ./console_sim --cols 63 --rows 34 --font 5x7", TERM_STYLE_NORMAL);
        queue_output_line("root  29  2.1 10.0 ./agy_quotad --interval 60s --wx-open-meteo", TERM_STYLE_NORMAL);
        break;
    }

    s_state = SIM_STATE_SKIT_TYPING;
    s_next_action_ms = now_ms() + 60;
}

// ============================================================================
// Abbreviated, Cryptic, Line-Saving Antigravity Quota Output (1 to 2 lines!)
// ============================================================================
static void compact_reset_tag(const char *reset_str, char *out, size_t out_sz)
{
    if (!reset_str || strlen(reset_str) == 0) {
        snprintf(out, out_sz, "..");
        return;
    }
    if (strncmp(reset_str, "Reset ", 6) == 0) {
        const char *p = reset_str + 6;
        // If format is "MM/DD HH:MM", take just "MM/DD" or "MM/DD_HHh" to save space
        const char *sp = strchr(p, ' ');
        if (sp) {
            size_t dlen = (size_t)(sp - p);
            if (dlen < out_sz) {
                memcpy(out, p, dlen);
                out[dlen] = '\0';
                return;
            }
        }
        snprintf(out, out_sz, "%s", p);
    } else if (strcmp(reset_str, "Active") == 0) {
        snprintf(out, out_sz, "act");
    } else if (strcmp(reset_str, "pending") == 0) {
        snprintf(out, out_sz, "..");
    } else {
        snprintf(out, out_sz, "%.8s", reset_str);
    }
}

static void prepare_quota_curl_output(uint32_t fmt_variant)
{
    s_out_count = 0;
    s_out_idx = 0;
    s_out_line_delay_ms = 55;

    const google_quota_data_t *q = google_quota_get_latest();
    char line[160];

    int p_g5 = (int)(q->limit_5h.remaining_fraction * 100.0f + 0.5f);
    int p_gw = (int)(q->limit_weekly.remaining_fraction * 100.0f + 0.5f);
    int p_c5 = (int)(q->limit_claude_5h.remaining_fraction * 100.0f + 0.5f);
    int p_cw = (int)(q->limit_claude_wk.remaining_fraction * 100.0f + 0.5f);
    int p_ar = (int)(q->limit_argon.remaining_fraction * 100.0f + 0.5f);

    char r_g5[12], r_gw[12], r_cw[12], r_ar[12];
    compact_reset_tag(q->limit_5h.reset_time_str, r_g5, sizeof(r_g5));
    compact_reset_tag(q->limit_weekly.reset_time_str, r_gw, sizeof(r_gw));
    compact_reset_tag(q->limit_claude_wk.reset_time_str, r_cw, sizeof(r_cw));
    compact_reset_tag(q->limit_argon.reset_time_str, r_ar, sizeof(r_ar));

    if (fmt_variant == 0) {
        // Ultra-dense single-line telemetry format (fits all 5 buckets + resets in 1 line of 52 cols!)
        snprintf(line, sizeof(line), "G5:%d%%@%s GW:%d%%@%s C5:%d%% CW:%d%% AR:%d%%@%s",
                 p_g5, r_g5, p_gw, r_gw, p_c5, p_cw, p_ar, r_ar);
        queue_output_line(line, TERM_STYLE_QUOTA);
    } else if (fmt_variant == 1) {
        // Compact 2-line minified JSON response (fits inside 52 cols!)
        snprintf(line, sizeof(line), "{\"g5\":\"%d%%@%s\",\"gw\":\"%d%%@%s\",\"ar\":\"%d%%@%s\",",
                 p_g5, r_g5, p_gw, r_gw, p_ar, r_ar);
        queue_output_line(line, TERM_STYLE_QUOTA);
        snprintf(line, sizeof(line), " \"c5\":\"%d%%\",\"cw\":\"%d%%@%s\",\"status\":%s}",
                 p_c5, p_cw, r_cw, q->has_error ? "\"WARN\"" : "\"OK\"");
        queue_output_line(line, TERM_STYLE_QUOTA);
    } else {
        // Cryptic key=val telemetry dump (2 lines on 52-col screen)
        snprintf(line, sizeof(line), "agy.gem={5h:%d%%(%s) wk:%d%%(%s)} argon=%d%%(%s)",
                 p_g5, r_g5, p_gw, r_gw, p_ar, r_ar);
        queue_output_line(line, TERM_STYLE_QUOTA);
        snprintf(line, sizeof(line), "agy.3p ={5h:%d%% wk:%d%%(%s)} [%s]",
                 p_c5, p_cw, r_cw,
                 (q->has_error && !q->limit_5h.valid) ? q->error_msg : "200_OK");
        queue_output_line(line, TERM_STYLE_QUOTA);
    }
}

// ============================================================================
// Terse 1-Line Weather Forecast Output for Every Minute (:00) via `curl`
// ============================================================================
static void prepare_weather_curl_output(uint32_t fmt_variant)
{
    s_out_count = 0;
    s_out_idx = 0;
    s_out_line_delay_ms = 50;

    const weather_data_t *wx = weather_client_get_latest();
    const app_settings_t *cfg = settings_mgr_get();
    const char *loc = (wx->location[0]) ? wx->location :
                      (cfg->weather_location[0] ? cfg->weather_location : "Tokyo");
    char line[160];

    if (!wx->valid) {
        snprintf(line, sizeof(line), "[wx:%.12s] (%s) --.-C Hi:--/Lo:-- PoP:--%%",
                 loc, wx->cond_short[0] ? wx->cond_short : "SYNC");
        queue_output_line(line, TERM_STYLE_CHAT);
        return;
    }

    const char *c_now = wx->cond_short[0] ? wx->cond_short : "CLR";
    const char *c_tmr = wx->tmrw_cond[0] ? wx->tmrw_cond : "CLR";

    if (fmt_variant == 0) {
        // Terse 1-line summary (<= 52 chars): e.g. "[Yokohama] CLR 11.5C(20/11) H50% P22% | tmr:CLR 19/11"
        snprintf(line, sizeof(line), "[%.9s] %s %.1fC(%.0f/%.0f) H%d%% P%d%% tmr:%s %.0f/%.0f",
                 loc, c_now, wx->temp_c, wx->max_c, wx->min_c,
                 wx->humidity, wx->pop_pct,
                 c_tmr, wx->tmrw_max_c, wx->tmrw_min_c);
    } else if (fmt_variant == 1) {
        // Minified 1-line JSON format (<= 52 chars)
        snprintf(line, sizeof(line), "{\"loc\":\"%.9s\",\"wx\":\"%s\",\"t\":%.1f,\"hl\":\"%.0f/%.0f\",\"p\":%d}",
                 loc, c_now, wx->temp_c, wx->max_c, wx->min_c, wx->pop_pct);
    } else {
        // wttr.in style 1-line telemetry (<= 52 chars)
        snprintf(line, sizeof(line), "%.9s: %s %+.1fC(H:%.0f/L:%.0f) W:%.1fm/s R:%d%%->%s",
                 loc, c_now, wx->temp_c, wx->max_c, wx->min_c,
                 wx->wind_mps, wx->pop_pct, c_tmr);
    }
    queue_output_line(line, TERM_STYLE_CHAT);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t console_sim_init(void)
{
    terminal_ui_clear();
    terminal_ui_push_line("Linux cyd-esp32 6.8.0-xtensa #1 SMP PREEMPT esp32", TERM_STYLE_CODE);
    terminal_ui_push_line("Last login: ttyS0 from 192.168.1.10 (52x30 vt100)", TERM_STYLE_CODE);

    s_state = SIM_STATE_BOOT;
    s_next_action_ms = now_ms() + 400;
    s_last_triggered_min = -1;
    s_last_scrollout_min = -1;
    s_pinned_top_clock_active = false;

    ESP_LOGI(TAG, "Console simulator initialized");
    return ESP_OK;
}

void console_sim_notify_wifi(bool connected, const char *ip_str)
{
    if (terminal_ui_is_alt_screen()) return;
    char msg[72];
    if (connected && ip_str) {
        snprintf(msg, sizeof(msg), "[netif] wlan0: bound to %s/24 (dhcp)", ip_str);
    } else if (ip_str) {
        snprintf(msg, sizeof(msg), "[wifi] SoftAP active: %s (%s)", PORTAL_AP_SSID, ip_str);
    } else {
        snprintf(msg, sizeof(msg), "[wifi] wlan0: carrier lost, reconnecting...");
    }
    terminal_ui_push_line(msg, TERM_STYLE_CODE);
    if (s_state == SIM_STATE_IDLE_PROMPT) {
        terminal_ui_start_prompt("~/agy");
    }
}

void console_sim_notify_touch(int x, int y)
{
    (void)x;
    if (y < 90) {
        uint8_t b = display_driver_cycle_brightness();
        if (!terminal_ui_is_alt_screen()) {
            char msg[64];
            snprintf(msg, sizeof(msg), "[sys] ledc backlight pwm duty set to %u%%", b);
            terminal_ui_push_line(msg, TERM_STYLE_CODE);
            if (s_state == SIM_STATE_IDLE_PROMPT) {
                terminal_ui_start_prompt("~/agy");
            }
        }
    } else {
        google_quota_request_refresh();
        weather_client_request_refresh();
        if (s_state == SIM_STATE_IDLE_PROMPT) {
            s_next_action_ms = now_ms();
        }
    }
}

void console_sim_tick(void)
{
    struct tm tm_now;
    char full_date[64];
    char hms[16];
    get_time_strings(full_date, sizeof(full_date), hms, sizeof(hms), &tm_now);

    int sec = tm_now.tm_sec;
    int min_key = tm_now.tm_hour * 60 + tm_now.tm_min;
    int64_t cur_ms = now_ms();

    // Keep live clock updated every second if in fullscreen app OR if a panic-wipe pinned clock bar is active!
    if (s_fs_last_clock_sec != sec) {
        s_fs_last_clock_sec = sec;
        if (terminal_ui_is_alt_screen()) {
            update_fullscreen_clock_bar();
        } else if (s_pinned_top_clock_active && sec < 58) {
            char tmux_bar[64];
            snprintf(tmux_bar, sizeof(tmux_bar), " [tmux 0:bash* 1:vim-] cyd-esp32        [%s]", hms);
            terminal_ui_set_line(0, tmux_bar, TERM_STYLE_STATUS_BAR);
        }
    }

    // ========================================================================
    // 1. PRIORITY EVENT: Second :00 -> `date` (Highlighted) + `curl` Quota + `curl` Weather!
    // ========================================================================
    if ((sec == 0 && s_last_triggered_min != min_key) || s_state == SIM_STATE_BOOT) {
        s_last_triggered_min = min_key;
        s_pinned_top_clock_active = false;

        if (terminal_ui_is_alt_screen()) {
            terminal_ui_exit_alt_screen();
            s_fs_app = FS_APP_NONE;
        }

        if (s_state == SIM_STATE_SKIT_TYPING || s_state == SIM_STATE_SKIT_HESITATE_CANCEL) {
            strncat(s_typed_cmd, "^C", sizeof(s_typed_cmd) - strlen(s_typed_cmd) - 1);
            terminal_ui_finish_command(s_typed_cmd);
        }

        static const char *date_cmds[] = {
            "date",
            "date +\"%a %b %d %H:%M:%S %Z %Y (epoch:%s)\"",
            "date -R"
        };
        begin_typing_command(date_cmds[rand_range(0, 2)], false);
        s_state = SIM_STATE_MINUTE_DATE_TYPE;
        s_next_action_ms = cur_ms + 35;
        return;
    }

    // ========================================================================
    // 2. PRIORITY EVENT: Second :58 -> Random `sed` / `grep` Scroll-Out!
    // ========================================================================
    if (sec >= 58 && s_last_scrollout_min != min_key &&
        s_state != SIM_STATE_MINUTE_DATE_TYPE &&
        s_state != SIM_STATE_MINUTE_DATE_EXEC &&
        s_state != SIM_STATE_MINUTE_CURL_TYPE &&
        s_state != SIM_STATE_MINUTE_CURL_EXEC &&
        s_state != SIM_STATE_MINUTE_WX_TYPE &&
        s_state != SIM_STATE_MINUTE_WX_EXEC &&
        s_state != SIM_STATE_SCROLLOUT_TYPE &&
        s_state != SIM_STATE_SCROLLOUT_DUMP) {

        s_last_scrollout_min = min_key;
        s_pinned_top_clock_active = false;

        if (terminal_ui_is_alt_screen()) {
            terminal_ui_exit_alt_screen();
            s_fs_app = FS_APP_NONE;
        }

        if (s_state == SIM_STATE_SKIT_TYPING || s_state == SIM_STATE_SKIT_HESITATE_CANCEL) {
            strncat(s_typed_cmd, "^C", sizeof(s_typed_cmd) - strlen(s_typed_cmd) - 1);
            terminal_ui_finish_command(s_typed_cmd);
        }

        int f_idx = (int)rand_range(0, NUM_EMBED_FILES - 1);
        int total_l = count_embed_lines(f_idx);
        int start_l = (total_l > 40) ? (int)rand_range(1, total_l - 34) : 1;

        s_out_count = 0;
        s_out_idx = 0;
        char scroll_cmd[100];

        if (rand_range(0, 1) == 0) {
            snprintf(scroll_cmd, sizeof(scroll_cmd), "sed -n '%d,%dp' %s",
                     start_l, start_l + 30, s_embed_files[f_idx].path);
            extract_embed_lines(f_idx, start_l, 31, true, TERM_STYLE_CODE);
        } else {
            snprintf(scroll_cmd, sizeof(scroll_cmd), "grep -n -E \"(void|int|char|if|static)\" %s",
                     s_embed_files[f_idx].path);
            if (grep_embed_lines(f_idx, " ", 31, TERM_STYLE_CODE) < 28) {
                extract_embed_lines(f_idx, start_l, 31, true, TERM_STYLE_CODE);
            }
        }

        begin_typing_command(scroll_cmd, false);
        s_state = SIM_STATE_SCROLLOUT_TYPE;
        s_next_action_ms = cur_ms + 25;
        return;
    }

    // ========================================================================
    // 3. State Machine Execution
    // ========================================================================
    if (cur_ms < s_next_action_ms) {
        return;
    }

    switch (s_state) {
    case SIM_STATE_MINUTE_DATE_TYPE: {
        if (s_target_cmd[s_type_idx] != '\0') {
            int cur_i = s_type_idx;
            s_typed_cmd[s_type_idx] = s_target_cmd[s_type_idx];
            s_type_idx++;
            s_typed_cmd[s_type_idx] = '\0';
            terminal_ui_update_command(s_typed_cmd, true);
            // Fast but organic rhythm so :00 date completes promptly
            uint32_t d = human_type_delay_ms(s_target_cmd, cur_i) / 2;
            if (d < 14) d = 14;
            if (d > 85) d = 85;
            s_next_action_ms = cur_ms + d;
        } else {
            terminal_ui_finish_command(s_typed_cmd);
            s_state = SIM_STATE_MINUTE_DATE_EXEC;
            s_next_action_ms = cur_ms + rand_range(50, 110);
        }
        break;
    }

    case SIM_STATE_MINUTE_DATE_EXEC: {
        terminal_ui_push_line(full_date, TERM_STYLE_DATE_HIGHLIGHT);
        terminal_ui_reset_scroll_count();
        s_max_scrolls_allowed = TERM_ROWS - 3;

        static const char *curl_cmds[] = {
            "curl -s https://daily-cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary | jq -c",
            "curl -sH \"Authorization: Bearer $TOK\" $AGY_API/v1internal:retrieveUserQuotaSummary",
            "curl -sL $AGY_URL:retrieveUserQuotaSummary | awk -F: '{print $1,$2}'",
            "curl -s -X POST $AGY_QUOTA_URL -d '{\"project\":\"aicode-consumers\"}' | jq -r '.compact'"
        };
        begin_typing_command(curl_cmds[rand_range(0, 3)], false);
        s_state = SIM_STATE_MINUTE_CURL_TYPE;
        s_next_action_ms = cur_ms + rand_range(70, 150);
        break;
    }

    case SIM_STATE_MINUTE_CURL_TYPE: {
        if (s_target_cmd[s_type_idx] != '\0') {
            char c = s_target_cmd[s_type_idx];
            // Burst 2 chars inside familiar alphanumeric tokens, 1 char at spaces/punctuation
            int step = (c >= 'a' && c <= 'z' && s_target_cmd[s_type_idx + 1] >= 'a' && s_target_cmd[s_type_idx + 1] <= 'z' && rand_range(0, 2) > 0) ? 2 : 1;
            for (int k = 0; k < step && s_target_cmd[s_type_idx] != '\0'; k++) {
                s_typed_cmd[s_type_idx] = s_target_cmd[s_type_idx];
                s_type_idx++;
            }
            s_typed_cmd[s_type_idx] = '\0';
            terminal_ui_update_command(s_typed_cmd, true);
            uint32_t d = (c == ' ' || c == '|' || c == '-' || c == ':' || c == '/')
                           ? rand_range(38, 95)
                           : rand_range(12, 34);
            s_next_action_ms = cur_ms + d;
        } else {
            terminal_ui_finish_command(s_typed_cmd);
            prepare_quota_curl_output(rand_range(0, 2));
            s_state = SIM_STATE_MINUTE_CURL_EXEC;
            s_next_action_ms = cur_ms + rand_range(95, 190);
        }
        break;
    }

    case SIM_STATE_MINUTE_CURL_EXEC: {
        if (s_out_idx < s_out_count) {
            terminal_ui_push_line(s_out_lines[s_out_idx], s_out_styles[s_out_idx]);
            s_out_idx++;
            s_next_action_ms = cur_ms + 55;
        } else {
            // Right after Antigravity quota curl, fetch 1-line Weather Forecast via curl!
            const app_settings_t *cfg = settings_mgr_get();
            const char *loc = (cfg->weather_location[0] != '\0') ? cfg->weather_location : "Tokyo";
            char wx_cmd[128];
            uint32_t wx_fmt = rand_range(0, 2);
            if (wx_fmt == 0) {
                snprintf(wx_cmd, sizeof(wx_cmd), "curl -s \"https://api.open-meteo.com/v1/forecast?city=%.24s\" | jq -r '.wx'", loc);
            } else if (wx_fmt == 1) {
                snprintf(wx_cmd, sizeof(wx_cmd), "curl -s \"https://wttr.in/%.24s?format=%%l:+%%C+%%t+%%h+%%p\"", loc);
            } else {
                snprintf(wx_cmd, sizeof(wx_cmd), "curl -sL \"$WX_API/forecast?loc=%.24s&daily=1\" | jq -c", loc);
            }
            begin_typing_command(wx_cmd, false);
            s_state = SIM_STATE_MINUTE_WX_TYPE;
            s_next_action_ms = cur_ms + rand_range(80, 160);
        }
        break;
    }

    case SIM_STATE_MINUTE_WX_TYPE: {
        if (s_target_cmd[s_type_idx] != '\0') {
            char c = s_target_cmd[s_type_idx];
            int step = (c >= 'a' && c <= 'z' && s_target_cmd[s_type_idx + 1] >= 'a' && s_target_cmd[s_type_idx + 1] <= 'z' && rand_range(0, 2) > 0) ? 2 : 1;
            for (int k = 0; k < step && s_target_cmd[s_type_idx] != '\0'; k++) {
                s_typed_cmd[s_type_idx] = s_target_cmd[s_type_idx];
                s_type_idx++;
            }
            s_typed_cmd[s_type_idx] = '\0';
            terminal_ui_update_command(s_typed_cmd, true);
            uint32_t d = (c == ' ' || c == '|' || c == '-' || c == '?' || c == '&' || c == '/')
                           ? rand_range(35, 90)
                           : rand_range(12, 32);
            s_next_action_ms = cur_ms + d;
        } else {
            terminal_ui_finish_command(s_typed_cmd);
            prepare_weather_curl_output(rand_range(0, 2));
            s_state = SIM_STATE_MINUTE_WX_EXEC;
            s_next_action_ms = cur_ms + rand_range(85, 175);
        }
        break;
    }

    case SIM_STATE_MINUTE_WX_EXEC: {
        if (s_out_idx < s_out_count) {
            terminal_ui_push_line(s_out_lines[s_out_idx], s_out_styles[s_out_idx]);
            s_out_idx++;
            s_next_action_ms = cur_ms + 50;
        } else {
            terminal_ui_start_prompt("~/agy");
            s_state = SIM_STATE_IDLE_PROMPT;
            s_next_action_ms = cur_ms + rand_range(1100, 2400);
        }
        break;
    }

    case SIM_STATE_IDLE_PROMPT: {
        if (sec >= 57) {
            s_next_action_ms = cur_ms + 200;
        } else {
            pick_and_start_random_skit(sec);
        }
        break;
    }

    case SIM_STATE_SKIT_TYPING: {
        if (!s_did_typo_in_this_cmd && s_typo_cmd[0] != '\0') {
            size_t typo_len = strlen(s_typo_cmd);
            if ((size_t)s_type_idx < typo_len) {
                int cur_i = s_type_idx;
                s_typed_cmd[s_type_idx] = s_typo_cmd[s_type_idx];
                s_type_idx++;
                s_typed_cmd[s_type_idx] = '\0';
                terminal_ui_update_command(s_typed_cmd, true);
                s_next_action_ms = cur_ms + human_type_delay_ms(s_typo_cmd, cur_i);
                break;
            } else {
                s_did_typo_in_this_cmd = true;
                s_state = SIM_STATE_SKIT_TYPO_PAUSE;
                // Human notices the typo: sometimes quickly (220ms), sometimes after a beat (750ms)
                s_next_action_ms = cur_ms + rand_range(220, 750);
                break;
            }
        }

        if (s_target_cmd[s_type_idx] != '\0') {
            int cur_i = s_type_idx;
            s_typed_cmd[s_type_idx] = s_target_cmd[s_type_idx];
            s_type_idx++;
            s_typed_cmd[s_type_idx] = '\0';
            terminal_ui_update_command(s_typed_cmd, true);
            s_next_action_ms = cur_ms + human_type_delay_ms(s_target_cmd, cur_i);
        } else {
            terminal_ui_finish_command(s_typed_cmd);
            if (s_fs_app != FS_APP_NONE) {
                terminal_ui_enter_alt_screen();
                s_fs_step = 0;
                s_state = SIM_STATE_FULLSCREEN_APP;
                s_next_action_ms = cur_ms + 150;
            } else {
                s_state = SIM_STATE_SKIT_OUTPUT_LINES;
                s_next_action_ms = cur_ms + rand_range(90, 210);
            }
        }
        break;
    }

    case SIM_STATE_SKIT_TYPO_PAUSE: {
        s_state = SIM_STATE_SKIT_BACKSPACE;
        // Reaching right pinky up to Backspace key
        s_next_action_ms = cur_ms + rand_range(75, 165);
        break;
    }

    case SIM_STATE_SKIT_BACKSPACE: {
        if (s_backspace_count > 0 && s_type_idx > 0) {
            s_type_idx--;
            s_typed_cmd[s_type_idx] = '\0';
            s_backspace_count--;
            terminal_ui_update_command(s_typed_cmd, true);
            // Multi-tap Backspace cadence: double-tap can be quick (28..65ms) or deliberate (75..155ms)
            uint32_t d = (rand_range(0, 2) == 0) ? rand_range(75, 155) : rand_range(28, 68);
            s_next_action_ms = cur_ms + d;
        } else {
            s_state = SIM_STATE_SKIT_TYPING;
            // Moving fingers back from Backspace to home row before resuming
            s_next_action_ms = cur_ms + rand_range(120, 310);
        }
        break;
    }

    case SIM_STATE_SKIT_HESITATE_CANCEL: {
        if (s_backspace_count == 0 && s_target_cmd[s_type_idx] != '\0') {
            int cur_i = s_type_idx;
            s_typed_cmd[s_type_idx] = s_target_cmd[s_type_idx];
            s_type_idx++;
            s_typed_cmd[s_type_idx] = '\0';
            terminal_ui_update_command(s_typed_cmd, true);
            s_next_action_ms = cur_ms + human_type_delay_ms(s_target_cmd, cur_i);
        } else if (s_backspace_count == 0) {
            // Reached end of dangerous command; freeze and stare at screen before chickening out
            s_backspace_count = (s_type_idx > 0) ? s_type_idx : 1;
            s_target_cmd[0] = '\0';
            s_hesitate_erased_cnt = 0;
            s_bs_burst_remaining = 0;
            s_next_action_ms = cur_ms + rand_range(850, 1650);
        } else if (s_type_idx > 0) {
            s_type_idx--;
            char erased = s_typed_cmd[s_type_idx];
            s_typed_cmd[s_type_idx] = '\0';
            terminal_ui_update_command(s_typed_cmd, true);
            s_next_action_ms = cur_ms + human_hesitate_bs_delay_ms(s_type_idx, erased);
        } else {
            s_backspace_count = 0;
            s_state = SIM_STATE_IDLE_PROMPT;
            s_next_action_ms = cur_ms + rand_range(1400, 2900);
        }
        break;
    }

    case SIM_STATE_SKIT_OUTPUT_LINES: {
        if (s_out_idx < s_out_count) {
            terminal_ui_push_line(s_out_lines[s_out_idx], s_out_styles[s_out_idx]);
            s_out_idx++;
            s_next_action_ms = cur_ms + s_out_line_delay_ms;
            if (s_pinned_top_clock_active) {
                char tmux_bar[64];
                snprintf(tmux_bar, sizeof(tmux_bar), " [tmux 0:bash* 1:vim-] cyd-esp32        [%s]", hms);
                terminal_ui_set_line(0, tmux_bar, TERM_STYLE_STATUS_BAR);
            }
        } else if (s_inline_panic_lines > 0) {
            s_state = SIM_STATE_SKIT_INLINE_PANIC;
            s_next_action_ms = cur_ms + 1350;
        } else {
            terminal_ui_start_prompt("~/agy");
            s_state = SIM_STATE_IDLE_PROMPT;
            s_next_action_ms = cur_ms + rand_range(1600, 3800);
        }
        break;
    }

    case SIM_STATE_SKIT_INLINE_PANIC: {
        int cur_row = terminal_ui_get_active_row();
        for (int i = 0; i < s_inline_panic_lines; i++) {
            int r = cur_row - i;
            if (r >= 0) {
                if (i == 0) {
                    terminal_ui_set_line(r, "[ctest] 24/24 unit tests passed (0.08s)", TERM_STYLE_CODE);
                } else {
                    terminal_ui_set_line(r, "Checking heap integrity & stack highwater...", TERM_STYLE_CODE);
                }
            }
        }
        s_inline_panic_lines = 0;
        terminal_ui_start_prompt("~/agy");
        s_state = SIM_STATE_IDLE_PROMPT;
        s_next_action_ms = cur_ms + rand_range(1500, 3000);
        break;
    }

    case SIM_STATE_FULLSCREEN_APP: {
        if (s_fs_step < s_fs_max_steps && sec < 56) {
            render_fullscreen_step();
            s_fs_step++;
            s_next_action_ms = cur_ms + 850;
        } else {
            terminal_ui_exit_alt_screen();
            fullscreen_app_t finished_app = s_fs_app;
            s_fs_app = FS_APP_NONE;

            if (finished_app != FS_APP_HTOP && rand_range(0, 1) == 1) {
                terminal_ui_clear();
                s_pinned_top_clock_active = true;
                char tmux_bar[64];
                snprintf(tmux_bar, sizeof(tmux_bar), " [tmux 0:bash* 1:vim-] cyd-esp32        [%s]", hms);
                terminal_ui_push_line(tmux_bar, TERM_STYLE_STATUS_BAR);
                begin_typing_command("clear && git diff --stat", false);
                queue_output_line(" main/console_sim.c | 28 ++++++++++++++++++++--------", TERM_STYLE_GAME);
                queue_output_line(" 1 file changed, 20 insertions(+), 8 deletions(-)", TERM_STYLE_NORMAL);
                s_out_idx = 0;
                s_state = SIM_STATE_SKIT_TYPING;
                s_next_action_ms = cur_ms + 40;
            } else {
                terminal_ui_start_prompt("~/agy");
                s_state = SIM_STATE_IDLE_PROMPT;
                s_next_action_ms = cur_ms + rand_range(1500, 3000);
            }
        }
        break;
    }

    case SIM_STATE_SCROLLOUT_TYPE: {
        if (s_target_cmd[s_type_idx] != '\0') {
            for (int k = 0; k < 3 && s_target_cmd[s_type_idx] != '\0'; k++) {
                s_typed_cmd[s_type_idx] = s_target_cmd[s_type_idx];
                s_type_idx++;
            }
            s_typed_cmd[s_type_idx] = '\0';
            terminal_ui_update_command(s_typed_cmd, true);
            s_next_action_ms = cur_ms + 25;
        } else {
            terminal_ui_finish_command(s_typed_cmd);
            s_state = SIM_STATE_SCROLLOUT_DUMP;
            s_next_action_ms = cur_ms + 30;
        }
        break;
    }

    case SIM_STATE_SCROLLOUT_DUMP: {
        if (s_out_idx < s_out_count) {
            // Dump 3 lines per tick so 35 lines scroll out smoothly within ~0.9s
            for (int k = 0; k < 3 && s_out_idx < s_out_count; k++) {
                terminal_ui_push_line(s_out_lines[s_out_idx], s_out_styles[s_out_idx]);
                s_out_idx++;
            }
            s_next_action_ms = cur_ms + 40;
        } else {
            terminal_ui_start_prompt("~/agy");
            s_state = SIM_STATE_IDLE_PROMPT;
            s_next_action_ms = cur_ms + 400;
        }
        break;
    }

    default:
        s_state = SIM_STATE_IDLE_PROMPT;
        s_next_action_ms = cur_ms + 1000;
        break;
    }
}
