// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONIGIRI_USIO_SELECTOR_H
#define ONIGIRI_USIO_SELECTOR_H
#include "usio_protocol.h"
typedef struct {
    usio_profile_t active, selected;
    bool menu, combo, wait_release, previous_plus, previous_minus, previous_up, previous_down;
    uint32_t combo_since;
} usio_selector_t;
void usio_selector_init(usio_selector_t *, usio_profile_t);
// Returns true when a different profile has been confirmed.
bool usio_selector_update(usio_selector_t *, bool plus, bool minus, bool up, bool down, uint32_t time_ms);
bool usio_selector_captures(const usio_selector_t *);
#endif
