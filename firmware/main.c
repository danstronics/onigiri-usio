// SPDX-License-Identifier: GPL-2.0-only
#include "board_config.h"
#include "controls.h"
#include "selector.h"
#include "ui.h"
#include "usio_flash.h"
#include "usio_usb.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "ssd1306/ssd1306.h"
#include <string.h>

// The clock stops (and blinks) once the System has issued no USIO command
// for this long: a crashed or hung game stops polling its I/O board.
enum { USB_SILENCE_MS = 2000, BLINK_MS = 500, DEBOUNCE_MS = 8, MARQUEE_MS = 40 };

static usio_protocol_t protocol;
static queue_t display_queue;
static bool stable[USIO_CONTROL_COUNT], raw_before[USIO_CONTROL_COUNT];
static uint32_t raw_changed[USIO_CONTROL_COUNT];
static volatile bool display_ready;

static void display_core(void) {
    if (!flash_safe_execute_core_init()) panic("Display core flash safety failed");
    gpio_set_function(PIN_OLED_SDA, GPIO_FUNC_I2C); gpio_set_function(PIN_OLED_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_OLED_SDA); gpio_pull_up(PIN_OLED_SCL); i2c_init(i2c0, 400000);
    uint8_t probe[] = {0, 0xe3}; ssd1306_t display = {0};
    bool present = i2c_write_timeout_us(i2c0, OLED_ADDRESS, probe, sizeof(probe), false, 2000) == (int)sizeof(probe);
    if (present) present = ssd1306_init(&display, UI_WIDTH, UI_HEIGHT, OLED_ADDRESS, i2c0);
    display_ready = true;
    while (true) {
        ui_state_t state;
        queue_remove_blocking(&display_queue, &state);
        if (!present) continue;
        // Reprobe with a timeout: an unplugged/stuck OLED must not affect USB servicing.
        if (i2c_write_timeout_us(i2c0, OLED_ADDRESS, probe, sizeof(probe), false, 2000) != (int)sizeof(probe)) { present = false; continue; }
        ui_render(&state, display.buffer);
        ssd1306_show(&display);
    }
}
static void update_buttons(uint32_t now) {
    for (unsigned i = 0; i < USIO_CONTROL_COUNT; i++) {
        bool raw = !gpio_get(usio_control_pins[i]);
        if (raw != raw_before[i]) { raw_before[i] = raw; raw_changed[i] = now; }
        if ((uint32_t)(now - raw_changed[i]) >= DEBOUNCE_MS) stable[i] = raw;
    }
}
static void start_profile(usio_profile_t profile, uint8_t options) {
    usio_protocol_init(&protocol, profile, usio_flash_open(profile));
    protocol.dx_on = profile == USIO_DEADSTORM && (options & USIO_OPTION_DX);
    usio_usb_init(&protocol);
}
int main(void) {
    for (unsigned i = 0; i < USIO_CONTROL_COUNT; i++) {
        gpio_init(usio_control_pins[i]); gpio_set_dir(usio_control_pins[i], GPIO_IN); gpio_pull_up(usio_control_pins[i]);
    }
    queue_init(&display_queue, sizeof(ui_state_t), 4); multicore_launch_core1(display_core);
    while (!display_ready) tight_loop_contents();
    usio_profile_t profile = usio_flash_saved_profile();
    uint8_t options = usio_flash_saved_options();
    start_profile(profile, options);
    usio_selector_t selector; usio_selector_init(&selector, profile);
    ui_state_t shown; memset(&shown, 0xff, sizeof(shown));
    bool release_all = false, options_dirty = false, seen_commands = false;
    uint32_t last_sample = UINT32_MAX, last_commands = 0, last_command_ms = 0;
    while (true) {
        usio_usb_task();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now != last_sample) {
            last_sample = now; update_buttons(now);
            bool changed = usio_selector_update(&selector, stable[USIO_CONTROL_ENTER], stable[USIO_CONTROL_SERVICE],
                                               stable[USIO_CONTROL_UP], stable[USIO_CONTROL_DOWN], now);
            bool capture = usio_selector_captures(&selector);
            if (capture) release_all = true;
            uint32_t pressed = 0;
            for (unsigned i = 0; i < USIO_CONTROL_COUNT; i++)
                if (stable[i]) pressed |= 1u << usio_control_pins[i];
            if (!capture && !pressed) release_all = false;
            usio_player_t players[2] = {0};
            usio_controls_from_gpio(pressed, !capture && !release_all, players);
            bool dx_before = protocol.dx_on;
            usio_protocol_input(&protocol, players, now);
            if (protocol.dx_on != dx_before) {
                options = (uint8_t)((options & ~USIO_OPTION_DX) | (protocol.dx_on ? USIO_OPTION_DX : 0));
                options_dirty = true;
            }
            if (changed) {
                usio_usb_disconnect();
                if (!usio_flash_flush() || !usio_flash_save_selection(selector.active, options)) panic("Unable to save profile/storage");
                options_dirty = false;
                sleep_ms(300);
                start_profile(selector.active, options);
                release_all = true; seen_commands = false; last_commands = 0;
            }
            if (protocol.commands != last_commands) {
                last_commands = protocol.commands; last_command_ms = now; seen_commands = true;
            }
            bool frozen = seen_commands && ((uint32_t)(now - last_command_ms) >= USB_SILENCE_MS || !usio_usb_opened());
            ui_state_t view; memset(&view, 0, sizeof(view));   // memcmp below: no stray padding
            view.profile = (uint8_t)selector.active; view.selected = (uint8_t)selector.selected;
            view.menu = selector.menu; view.uptime_s = (frozen ? last_command_ms : now) / 1000;
            view.frozen = frozen; view.blink_on = frozen && (now / BLINK_MS) % 2 == 0;
            view.test = protocol.test_on; view.dx = protocol.dx_on;
            if (selector.menu) view.scroll = (uint16_t)(now / MARQUEE_MS);   // One pixel per step.
            if (memcmp(&view, &shown, sizeof(view)) && queue_try_add(&display_queue, &view)) shown = view;
        }
        if (usio_usb_idle()) {
            // Persist the DX choice only between transfers, like bookkeeping.
            if (options_dirty && usio_flash_save_selection(selector.active, options)) options_dirty = false;
            usio_flash_tick(false);
        }
        tight_loop_contents();
    }
}
