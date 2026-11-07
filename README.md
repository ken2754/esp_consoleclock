# ESP32 CYD Bash Console Clock & Antigravity Quota Dashboard (`esp_consoleclock`)

A simulated live bash terminal dashboard for the **ESP32 Cheap Yellow Display (CYD / ESP32-2432S028, 320x240)**.
It looks as if someone is actively logged into a Linux console typing commands, inspecting code, chatting with colleagues, and occasionally slacking off with terminal games before panicking when the boss walks by.

## Features

- **High-Density 52×30 Non-Bold Terminal Engine**:
  - Uses a custom 1bpp crisp `6×8` pixel monospace font (`lv_font_term_6x8`), packing **52 columns × 30 rows** onto the 320×240 screen.
  - Long commands (including URLs) wrap naturally across multiple terminal rows with flicker-free dirty-checked LVGL rendering.
- **Every Minute at `:00`**:
  - Types `date` and prints the current local time with a **high-contrast highlighted bar**.
  - Types a realistic `curl` one-liner and prints the 5 **Antigravity / Google AI Quota buckets** (`G5`, `GW`, `C5`, `CW`, `AW`) in ultra-compact 1–2 line cryptic/abbreviated CLI formats (compact status, minified JSON, or `sysctl` key=value).
- **Seconds `:05` to `:57` (Random Hacker / Slacker Skits with Scroll Budget Protection)**:
  - **Self-Referential Code Inspection (`EMBED_TXTFILES`)**: Runs `sed`, `head`, `cat -n`, `grep`, and `wc -l` against its own actual C source code (`main.c`, `config.h`, `terminal_ui.c`, `console_sim.c`, `google_quota.c`) embedded in flash.
  - **Human-like Typing & Hesitation**: Variable keystroke cadence, realistic typos corrected with `Backspace`, typing dangerous commands (`sudo rm -rf ...`, `git push --force`) then hesitating and erasing them, or simply idling with a blinking cursor `_`.
  - **Colleague Messaging**: Sends and receives `write`, `wall`, `irc-send`, `slack`, `talk`, and `mail` messages.
  - **Full-Screen TUI Games & `vim` with Live Embedded Clock**:
    - Opens `vim`, `nethack`, `invaders`, `snake`, `htop`, or `cmatrix`.
    - While in a full-screen TUI, a **live `[HH:MM:SS]` clock** is always visible in the status bar / HUD.
    - Panics (`:q!`, `^C`, `clear`) to hide the game; even if the screen is wiped via `clear`, a `tmux` status bar at the top keeps the current time visible on screen.
- **Every Minute at `:58` (Rapid Scroll-Out)**:
  - Runs a random `sed` or `grep` over its embedded C source files, dumping 31 lines rapidly to scroll the previous minute's time and quota off the top of the screen just before `:00`.
- **Hardware & Configuration Compatibility**:
  - Shares the exact pinout, ST7789/ILI9341 auto-fallback display driver, XPT2046 touch driver, NVS namespace (`agyclock`), Web Portal (`/save`), and `tools/get_token.py` with `esp_agy_clock`.
  - Tap the top of the screen (`y < 90`) to cycle backlight brightness (`0% -> 25% -> 50% -> 100%`).
  - Tap the middle/bottom of the screen to trigger an immediate background quota refresh and next console action.

## Build & Flash

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```
