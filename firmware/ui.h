// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONIGIRI_UI_H
#define ONIGIRI_UI_H
#include <stdbool.h>
#include <stdint.h>
enum { UI_WIDTH = 128, UI_HEIGHT = 64, UI_BYTES = UI_WIDTH * UI_HEIGHT / 8 };
typedef struct {
    uint8_t profile, selected;
    bool menu;
    uint32_t uptime_s;   // Shown clock value (frozen value while USB is silent).
    bool frozen;         // The System stopped issuing USIO commands.
    bool blink_on;       // Blink phase for the frozen clock.
    bool test, dx;
} ui_state_t;
// Renders into an SSD1306 page-ordered buffer (x + 128 * (y / 8), bit y % 8).
void ui_render(const ui_state_t *, uint8_t fb[UI_BYTES]);
#endif
