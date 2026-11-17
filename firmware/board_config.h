// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONIGIRI_BOARD_CONFIG_H
#define ONIGIRI_BOARD_CONFIG_H
// ITAIKO-firmware include/GlobalConfiguration.h (Onigiri). Active-low buttons to GND.
// Top row, left to right: L, -, Share | Home, +, R.
enum {
    PIN_UP = 27, PIN_DOWN = 7, PIN_LEFT = 26, PIN_RIGHT = 14,
    PIN_BUTTON1 = 5, PIN_BUTTON2 = 1, PIN_BUTTON3 = 2, PIN_BUTTON4 = 6,
    PIN_BUTTON5 = 0, PIN_START = 3, PIN_SERVICE = 28, PIN_TEST = 4, PIN_COIN = 15, PIN_L = 29,
    PIN_OLED_SDA = 8, PIN_OLED_SCL = 9, OLED_ADDRESS = 0x3c,
};
#endif
