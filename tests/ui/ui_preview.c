// SPDX-License-Identifier: GPL-2.0-only
// Host preview/regression: renders OLED states to PBM files.
#include "ui.h"
#include "usio_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void save(const char *name, const uint8_t *fb) {
    FILE *f = fopen(name, "wb"); assert(f);
    fprintf(f, "P1\n%d %d\n", UI_WIDTH, UI_HEIGHT);
    for (int y = 0; y < UI_HEIGHT; y++) {
        for (int x = 0; x < UI_WIDTH; x++) fputc((fb[x + UI_WIDTH * (y >> 3)] >> (y & 7)) & 1 ? '1' : '0', f);
        fputc('\n', f);
    }
    fclose(f);
}
int main(void) {
    static uint8_t fb[UI_BYTES]; char name[64];
    for (unsigned p = 0; p < USIO_PROFILE_COUNT; p++) {
        ui_state_t s = {.profile = (uint8_t)p, .uptime_s = 640 + p * 997};
        if (p == USIO_DEADSTORM) s.dx = true;
        if (p == USIO_TEKKEN) s.test = true;
        ui_render(&s, fb); snprintf(name, sizeof(name), "main-%02u.pbm", p); save(name, fb);
    }
    // DX badge under the title (DX exists only on Deadstorm, a one-line title).
    for (unsigned p = 0; p < 1; p++) {
        ui_state_t d = {.profile = USIO_DEADSTORM, .uptime_s = 77, .dx = true};
        ui_render(&d, fb); snprintf(name, sizeof(name), "dx-%u.pbm", p); save(name, fb);
        ui_state_t off = d; off.dx = false; static uint8_t ref[UI_BYTES]; ui_render(&off, ref);
        assert(memcmp(fb, ref, UI_BYTES)); // The badge is visible.
    }
    ui_state_t frozen = {.profile = USIO_DEADSTORM, .uptime_s = 640, .frozen = true, .blink_on = false};
    ui_render(&frozen, fb); save("frozen-off.pbm", fb);
    unsigned lit_off = 0, lit_on = 0;
    for (int x = 0; x < UI_WIDTH; x++) lit_off += __builtin_popcount(fb[x] & 0xfe);
    assert((fb[0 + UI_WIDTH * 7] & 0x80) && (fb[127 + UI_WIDTH * 7] & 0x80)); // White bar edge to edge.
    frozen.blink_on = true; ui_render(&frozen, fb);
    for (int x = 0; x < UI_WIDTH; x++) lit_on += __builtin_popcount(fb[x] & 0xfe);
    assert(lit_on > lit_off + 20); // The frozen clock digits blink.
    for (unsigned sel = 0; sel < USIO_PROFILE_COUNT; sel += 5) {
        ui_state_t m = {.menu = true, .selected = (uint8_t)sel};
        ui_render(&m, fb); snprintf(name, sizeof(name), "menu-%02u.pbm", sel); save(name, fb);
    }
    puts("PASS UI render");
    return 0;
}
