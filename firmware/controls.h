// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONIGIRI_USIO_CONTROLS_H
#define ONIGIRI_USIO_CONTROLS_H
#include "usio_protocol.h"

enum {
    USIO_CONTROL_UP, USIO_CONTROL_DOWN, USIO_CONTROL_LEFT, USIO_CONTROL_RIGHT,
    USIO_CONTROL_ACTION1, USIO_CONTROL_ACTION2, USIO_CONTROL_ACTION3,
    USIO_CONTROL_ACTION4, USIO_CONTROL_ACTION5, USIO_CONTROL_ENTER,
    USIO_CONTROL_SERVICE, USIO_CONTROL_TEST, USIO_CONTROL_COIN, USIO_CONTROL_L, USIO_CONTROL_COUNT
};
extern const uint8_t usio_control_pins[USIO_CONTROL_COUNT];
// Debounced active-low GPIOs are represented by set bits. The physical binding
// is deliberately profile-independent; only the outgoing wire format varies.
void usio_controls_from_gpio(uint32_t pressed, bool enabled, usio_player_t players[2]);
#endif
