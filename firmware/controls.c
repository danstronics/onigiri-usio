// SPDX-License-Identifier: GPL-2.0-only
#include "controls.h"
#include "board_config.h"
#include <string.h>

const uint8_t usio_control_pins[USIO_CONTROL_COUNT] = {
    PIN_UP, PIN_DOWN, PIN_LEFT, PIN_RIGHT,
    PIN_BUTTON1, PIN_BUTTON2, PIN_BUTTON3, PIN_BUTTON4, PIN_BUTTON5,
    PIN_START, PIN_SERVICE, PIN_TEST, PIN_COIN, PIN_L
};
static bool down(uint32_t pressed, unsigned pin) { return (pressed & (1u << pin)) != 0; }
void usio_controls_from_gpio(uint32_t pressed, bool enabled, usio_player_t players[2]) {
    memset(players, 0, 2 * sizeof(*players));
    if (!enabled) return;
    players[0] = (usio_player_t){
        .up = down(pressed, PIN_UP), .down = down(pressed, PIN_DOWN),
        .left = down(pressed, PIN_LEFT), .right = down(pressed, PIN_RIGHT),
        .start = down(pressed, PIN_START), .service = down(pressed, PIN_SERVICE),
        .test = down(pressed, PIN_TEST), .coin = down(pressed, PIN_COIN),
        .button = {down(pressed, PIN_BUTTON1), down(pressed, PIN_BUTTON2),
                   down(pressed, PIN_BUTTON3), down(pressed, PIN_BUTTON4), down(pressed, PIN_BUTTON5)},
        .l = down(pressed, PIN_L)
    };
}
