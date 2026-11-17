// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONIGIRI_USIO_PROTOCOL_H
#define ONIGIRI_USIO_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "card_reader.h"

enum { USIO_PAGE_SIZE = 65536, USIO_PAGE_COUNT = 16, USIO_FRAME_SIZE = 384 };
// Preserve the original five indices: saved selections and SRAM banks use them.
typedef enum {
    USIO_TAIKO, USIO_TEKKEN, USIO_SHOOTER, USIO_DEADSTORM, USIO_H050,
    USIO_TEKKEN6BR, USIO_GUNDAM, USIO_DARK_ESCAPE, USIO_SAILOR_ZOMBIE,
    USIO_DRAGONBALL, USIO_TAG2_UNLIMITED, USIO_PROFILE_COUNT
} usio_profile_t;
#define USIO_TAG2_VANILLA USIO_TEKKEN
typedef enum { USIO_LAYOUT_TAIKO, USIO_LAYOUT_FIGHTING, USIO_LAYOUT_SHOOTER, USIO_LAYOUT_USJ } usio_layout_t;
typedef struct {
    const char *name, *board;
    uint16_t pid, version;
    usio_layout_t layout;
} usio_profile_info_t;
typedef struct {
    bool up, down, left, right, start, service, coin, test;
    bool button[5];
    bool l;  // Top-left L: system toggles (Deadstorm DX cabinet)
} usio_player_t;
typedef struct {
    void *context;
    bool (*read)(void *, uint32_t address, uint8_t *, size_t);
    bool (*write)(void *, uint32_t address, const uint8_t *, size_t);
    bool (*clear)(void *);
} usio_memory_t;
typedef struct {
    usio_profile_t profile;
    usio_memory_t memory;
    usio_player_t players[2];
    uint16_t coins[2], gun_x[2], gun_y[2], aim_ticks[2];
    bool test_on, previous_coin[2], previous_test, dx_on, previous_l;
    uint8_t pending_hits[2];
    uint8_t header[6], header_used, channel;
    uint16_t reg, length;
    uint32_t write_used, read_position;
    bool writing, reading;
    uint8_t write_data[65535];
    uint8_t frame[USIO_FRAME_SIZE], last_taiko[96];
    usio_nfc_t readers[2];
    uint32_t malformed_commands, unsupported_reads, unsupported_writes, memory_errors;
    uint32_t time_ms;
    uint32_t commands;
} usio_protocol_t;
enum { USIO_EVENT_NONE = 0, USIO_EVENT_READ = 1, USIO_EVENT_WRITE = 2, USIO_EVENT_INIT = 4 };
const char *usio_profile_name(usio_profile_t);
uint16_t usio_profile_pid(usio_profile_t);
const usio_profile_info_t *usio_profile_info(usio_profile_t);
const char *usio_profile_board(usio_profile_t);
uint16_t usio_profile_version(usio_profile_t);
usio_layout_t usio_profile_layout(usio_profile_t);
// Offset of the first USIO node in the 0x1000 input block for this game.
unsigned usio_profile_input_base(usio_profile_t);
// Games with a mounted gun read P1 X/Y from analog channels 0/1 of that node.
bool usio_profile_has_gun(usio_profile_t);
void usio_protocol_init(usio_protocol_t *, usio_profile_t, usio_memory_t);
void usio_protocol_reset_transport(usio_protocol_t *);
void usio_protocol_input(usio_protocol_t *, const usio_player_t players[2], uint32_t time_ms);
unsigned usio_protocol_feed(usio_protocol_t *, const uint8_t *, size_t);
size_t usio_protocol_read(usio_protocol_t *, uint8_t *, size_t);
#endif
