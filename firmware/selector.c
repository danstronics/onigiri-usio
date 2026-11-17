// SPDX-License-Identifier: GPL-2.0-only
#include "selector.h"
#include <string.h>
void usio_selector_init(usio_selector_t *s, usio_profile_t p) { memset(s, 0, sizeof(*s)); s->active = s->selected = p; }
bool usio_selector_update(usio_selector_t *s, bool plus, bool minus, bool up, bool down, uint32_t now) {
    bool changed = false;
    if (s->wait_release) {
        if (!plus && !minus && !up && !down) s->wait_release = false;
    } else if (s->menu) {
        unsigned at = usio_menu_position(s->selected);
        if (up && !s->previous_up) s->selected = usio_menu_profile(at + USIO_PROFILE_COUNT - 1);
        if (down && !s->previous_down) s->selected = usio_menu_profile(at + 1);
        if (plus && !s->previous_plus) {
            changed = s->active != s->selected; s->active = s->selected; s->menu = false; s->wait_release = true;
        } else if (minus && !s->previous_minus) { s->selected = s->active; s->menu = false; s->wait_release = true; }
    } else if (plus && minus) {
        if (!s->combo) { s->combo = true; s->combo_since = now; }
        if ((uint32_t)(now - s->combo_since) >= 2000) { s->menu = true; s->selected = s->active; s->combo = false; s->wait_release = true; }
    } else if (s->combo) { s->combo = false; s->wait_release = plus || minus; }
    s->previous_plus = plus; s->previous_minus = minus; s->previous_up = up; s->previous_down = down;
    return changed;
}
bool usio_selector_captures(const usio_selector_t *s) { return s->menu || s->combo || s->wait_release; }
