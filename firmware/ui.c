// SPDX-License-Identifier: GPL-2.0-only
// OLED layout: clock line, game title in the large font, white bottom bar.
#include "ui.h"
#include "ui_fonts.h"
#include "usio_protocol.h"
#include <stdio.h>
#include <string.h>

#ifndef ONIGIRI_VERSION
#define ONIGIRI_VERSION "dev"
#endif
enum { BAR_TOP = 55, CLOCK_TOP = 1, HINT_TOP = BAR_TOP - 7 };
#define HINT "HOLD + AND - FOR GAME SELECT"

static void pixel(uint8_t *fb, int x, int y, bool on) {
    if (x < 0 || y < 0 || x >= UI_WIDTH || y >= UI_HEIGHT) return;
    uint8_t *b = &fb[x + UI_WIDTH * (y >> 3)], m = (uint8_t)(1u << (y & 7));
    if (on) *b |= m; else *b &= (uint8_t)~m;
}
static void fill(uint8_t *fb, int x, int y, int w, int h, bool on) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) pixel(fb, i, j, on);
}
static unsigned glyph(char c) {
    unsigned char u = (unsigned char)c;
    return (u < UI_FONT_FIRST || u > UI_FONT_LAST ? '?' : u) - UI_FONT_FIRST;
}

// Bar font, 3x5 + 1 px spacing.
static int tiny_width(const char *s) { int n = (int)strlen(s); return n ? n * 4 - 1 : 0; }
static void tiny(uint8_t *fb, int x, int y, const char *s, bool on) {
    for (; *s; s++, x += 4)
        for (int col = 0; col < 3; col++) {
            uint8_t bits = ui_tiny_cols[glyph(*s) * 3 + col];
            for (int row = 0; row < 5; row++) if (bits & (1u << row)) pixel(fb, x + col, y + row, on);
        }
}

// Clock font, 5x7 + 1 px spacing; the label is drawn bold and slanted.
static int clock_width(const char *s, bool label) { int n = (int)strlen(s); return n ? n * 6 - 1 + (label ? n + 1 : 0) : 0; }
static int clock_text(uint8_t *fb, int x, int y, const char *s, bool label) {
    for (; *s; s++) {
        for (int col = 0; col < 5; col++) {
            uint8_t bits = ui_clock_cols[glyph(*s) * 5 + col];
            for (int row = 0; row < 8; row++) {
                if (!(bits & (1u << row))) continue;
                int slant = label ? (7 - row) / 3 : 0;
                pixel(fb, x + col + slant, y + row, true);
                if (label) pixel(fb, x + col + slant + 1, y + row, true);
            }
        }
        x += label ? 7 : 6;
    }
    return x;
}

// Title font, proportional.
static int title_width(const char *s, size_t n) {
    int w = 0;
    for (size_t i = 0; i < n; i++) w += ui_title_advance[glyph(s[i])];
    return w ? w - 1 : 0;
}
static void title_text(uint8_t *fb, int x, int y, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const uint16_t *rows = &ui_title_rows[glyph(s[i]) * UI_TITLE_HEIGHT];
        for (int row = 0; row < UI_TITLE_HEIGHT; row++)
            for (int col = 0; col < 16; col++) if (rows[row] & (0x8000u >> col)) pixel(fb, x + col, y + row, true);
        x += ui_title_advance[glyph(s[i])];
    }
}
// Greedy word wrap into at most three centred lines. The block (title plus
// `footer` reserved pixels) is centred vertically; returns the y below the text.
static int title_block(uint8_t *fb, const char *name, int top, int bottom, int footer) {
    const char *start[3]; size_t len[3]; int lines = 0;
    const char *p = name;
    while (*p && lines < 3) {
        while (*p == ' ') p++;
        size_t fit = 0, last_space = 0;
        for (size_t n = 1; p[n - 1]; n++) {
            if (title_width(p, n) > UI_WIDTH - 2) break;
            fit = n;
            if (p[n] == ' ' || p[n] == 0) last_space = n;
        }
        size_t take = p[fit] == 0 ? fit : last_space ? last_space : fit;
        if (!take) break;
        start[lines] = p; len[lines] = take; lines++; p += take;
    }
    int line_h = UI_TITLE_HEIGHT + 1, block = lines * line_h - 1;
    int y = top + (bottom - top - block - footer) / 2;
    if (y < top) y = top;
    for (int i = 0; i < lines; i++, y += line_h)
        title_text(fb, (UI_WIDTH - title_width(start[i], len[i])) / 2, y, start[i], len[i]);
    return y - 1;
}
// White badge with black text in the clock font, e.g. "DX" under the title.
enum { BADGE_H = 11, BADGE_GAP = 2 };
static void badge(uint8_t *fb, int y, const char *text) {
    int w = (int)strlen(text) * 6 - 1 + 8, x = (UI_WIDTH - w) / 2;
    fill(fb, x, y, w, BADGE_H, true);
    pixel(fb, x, y, false); pixel(fb, x + w - 1, y, false);                     // Rounded corners.
    pixel(fb, x, y + BADGE_H - 1, false); pixel(fb, x + w - 1, y + BADGE_H - 1, false);
    for (int col = 0, cx = x + 4; text[col]; col++, cx += 6)
        for (int c = 0; c < 5; c++) {
            uint8_t bits = ui_clock_cols[glyph(text[col]) * 5 + c];
            for (int row = 0; row < 8; row++) if (bits & (1u << row)) pixel(fb, cx + c, y + 2 + row, false);
        }
}
static void format_clock(char *out, size_t size, uint32_t s) {
    if (s >= 3600) snprintf(out, size, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
    else snprintf(out, size, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}
static void bar(uint8_t *fb, int top, const char *left, const char *right) {
    fill(fb, 0, top, UI_WIDTH, UI_HEIGHT - top, true);
    if (left) tiny(fb, 3, top + 2, left, false);
    if (right) tiny(fb, UI_WIDTH - 3 - tiny_width(right), top + 2, right, false);
}

static void main_screen(const ui_state_t *s, uint8_t *fb) {
    char clock[24]; format_clock(clock, sizeof(clock), s->uptime_s);
    const char *label = "uptime", *sep = " : ";
    int w = clock_width(label, true) + clock_width(sep, false) + clock_width(clock, false);
    int x = (UI_WIDTH - w) / 2;
    x = clock_text(fb, x, CLOCK_TOP, label, true) + 1;
    x = clock_text(fb, x, CLOCK_TOP, sep, false);
    if (!s->frozen || s->blink_on) clock_text(fb, x, CLOCK_TOP, clock, false);
    int below = title_block(fb, usio_profile_name((usio_profile_t)s->profile), CLOCK_TOP + 9, HINT_TOP - 1,
                            s->dx ? BADGE_GAP + BADGE_H : 0);
    if (s->dx) badge(fb, below + BADGE_GAP, "DX");
    tiny(fb, (UI_WIDTH - tiny_width(HINT)) / 2, HINT_TOP, HINT, true);
    bar(fb, BAR_TOP, s->test ? "TEST" : NULL, "onigiri-usio");
}

static void menu_screen(const ui_state_t *s, uint8_t *fb) {
    bar(fb, 0, "GAME SELECT", "v" ONIGIRI_VERSION);
    fill(fb, 0, 9, UI_WIDTH, UI_HEIGHT - 9, false);
    enum { ROWS = 5, ROW_H = 9, LIST_TOP = 10, TRACK_H = ROWS * ROW_H, SCROLL_X = UI_WIDTH - 3, LIST_W = UI_WIDTH - 5 };
    unsigned first = s->selected >= ROWS - 1 ? s->selected - (ROWS - 2) : 0;
    if (first + ROWS > USIO_PROFILE_COUNT) first = USIO_PROFILE_COUNT - ROWS;
    // Scroll bar: dotted track, thumb sized to the visible share of the list.
    for (int y = LIST_TOP; y < LIST_TOP + TRACK_H; y += 2) pixel(fb, SCROLL_X + 1, y, true);
    int thumb = TRACK_H * ROWS / USIO_PROFILE_COUNT;
    int thumb_y = LIST_TOP + (TRACK_H - thumb) * (int)first / (USIO_PROFILE_COUNT - ROWS);
    fill(fb, SCROLL_X, thumb_y, 3, thumb, true);
    for (unsigned i = 0; i < ROWS; i++) {
        unsigned p = first + i; int y = LIST_TOP + (int)i * ROW_H;
        bool sel = p == s->selected;
        if (sel) fill(fb, 0, y, LIST_W - 1, ROW_H, true);
        const char *name = usio_profile_name((usio_profile_t)p);
        for (int col = 0, x = 3; name[col] && x + 5 < LIST_W - 2; col++, x += 6)
            for (int c = 0; c < 5; c++) {
                uint8_t bits = ui_clock_cols[glyph(name[col]) * 5 + c];
                for (int row = 0; row < 8; row++) if (bits & (1u << row)) pixel(fb, x + c, y + 1 + row, !sel);
            }
    }
    tiny(fb, 3, UI_HEIGHT - 5, "+ OK", true);
    tiny(fb, UI_WIDTH - 3 - tiny_width("- ESC"), UI_HEIGHT - 5, "- ESC", true);
}

void ui_render(const ui_state_t *s, uint8_t fb[UI_BYTES]) {
    memset(fb, 0, UI_BYTES);
    if (s->menu) menu_screen(s, fb); else main_screen(s, fb);
}
