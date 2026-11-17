// SPDX-License-Identifier: GPL-2.0-only
// Protocol and input layouts adapted from RPCS3 Emu/Io/usio.cpp; see NOTICE.md.
#include "usio_protocol.h"
#include "usio_identification.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
void usio_protocol_reset_transport(usio_protocol_t *u) {
    u->header_used = 0; u->writing = u->reading = false;
    u->read_position = u->write_used = 0;
}
void usio_protocol_init(usio_protocol_t *u, usio_profile_t p, usio_memory_t memory) {
    memset(u, 0, sizeof(*u)); u->profile = p; u->memory = memory;
    for (unsigned i = 0; i < 2; i++) u->gun_x[i] = u->gun_y[i] = 0x8000;
}
void usio_protocol_input(usio_protocol_t *u, const usio_player_t players[2], uint32_t time_ms) {
    // Capture edges at GPIO sampling time, not at the host polling rate.
    for (unsigned i = 0; i < 2; i++) {
        const usio_player_t *p = &players[i];
        if (p->coin && !u->previous_coin[i]) u->coins[i]++;
        u->previous_coin[i] = p->coin;
        for (unsigned j = 0; j < 4; j++)
            if (p->button[j] && !u->players[i].button[j]) u->pending_hits[i] |= (uint8_t)(1u << j);
        // D-pad aiming (no ADC): every 8 ms the crosshair moves by a step that
        // starts fine (192/65536) and accelerates to 1536 after ~400 ms held,
        // so a tap corrects precisely and a hold crosses the screen in ~0.4 s.
        if (usio_profile_has_gun(u->profile) && time_ms / 8 != u->time_ms / 8) {
            int dx = p->right - p->left, dy = p->down - p->up;
            if (!dx && !dy) u->aim_ticks[i] = 0;
            else if (u->aim_ticks[i] < 50) u->aim_ticks[i]++;
            int step = 192 + (1536 - 192) * u->aim_ticks[i] / 50;
            int x = u->gun_x[i] + dx * step;
            int y = u->gun_y[i] + dy * step;
            u->gun_x[i] = (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x);
            u->gun_y[i] = (uint16_t)(y < 0 ? 0 : y > 65535 ? 65535 : y);
        }
    }
    if (players[0].test && !u->previous_test) u->test_on = !u->test_on;
    u->previous_test = players[0].test;
    // L toggles Deadstorm's DX cabinet switch; it never reaches the game as a key.
    if (u->profile == USIO_DEADSTORM && players[0].l && !u->previous_l) u->dx_on = !u->dx_on;
    u->previous_l = players[0].l;
    memcpy(u->players, players, sizeof(u->players)); u->time_ms = time_ms;
}
static void taiko_frame(usio_protocol_t *u) {
    memset(u->frame, 0, sizeof(u->frame));
    const usio_player_t *p = &u->players[0];
    uint16_t digital = (u->test_on ? 0x80 : 0) | (p->start ? 0x200 : 0) |
        (p->down ? 0x1000 : 0) | (p->up ? 0x2000 : 0) | (p->service ? 0x4000 : 0);
    put16(u->frame, digital); put16(u->frame + 16, u->coins[0]);
    for (unsigned player = 0; player < 2; player++) {
        for (unsigned key = 0; key < 4; key++)
            if (u->pending_hits[player] & (1u << key)) put16(u->frame + 32 + player * 8 + key * 2, 0xffff);
        u->pending_hits[player] = 0;
    }
    memcpy(u->last_taiko, u->frame, sizeof(u->last_taiko));
}
// Shooter switches as consumed by the shared SDK titles. Deadstorm's game code
// (button accessors 0x324eac/0x324ec0) tests 0x400000|0x800000 for the P1 gun
// triggers (RPCS3 shooter Left/Right), 0x200000 for P1 Start, 0x20000 for the
// extra button, 0x200 Enter, 0x2000/0x1000 menu Up/Down, and 0x100 (accessor
// 0x324ffc) for the DX cabinet. The D-pad aims; it no longer fires.
static uint64_t shooter_bits(const usio_protocol_t *u, const usio_player_t *p) {
    uint64_t d = 0;
    if (p->start) d |= 0x200;          // +     : menu Enter
    if (p->up) d |= 0x2000;            // D-pad : menu Up / Down (and aim)
    if (p->down) d |= 0x1000;
    if (p->button[2]) d |= 0x800000;   // South : fire
    if (p->button[3]) d |= 0x400000;   // East  : fire (second trigger)
    if (p->button[0]) d |= 0x200000;   // West  : game Start
    if (p->button[1]) d |= 0x20000;    // North : extra button
    if (u->profile == USIO_DEADSTORM && u->dx_on) d |= 0x100;
    return d;
}
static void tekken_frame(usio_protocol_t *u) {
    memset(u->frame, 0, sizeof(u->frame));
    bool shooter = usio_profile_layout(u->profile) == USIO_LAYOUT_SHOOTER;
    bool legacy = usio_profile_layout(u->profile) == USIO_LAYOUT_USJ;
    const unsigned input_base = usio_profile_input_base(u->profile);
    uint64_t digital = u->test_on ? 0x80 : 0;
    uint16_t lm = u->test_on ? 0x1000 : 0;
    for (unsigned i = 0; i < 2; i++) {
        const usio_player_t *p = &u->players[i]; unsigned shift = i * 24;
        if (p->service && i == 0) digital |= 0x4000;
        put16(u->frame + input_base + 0x20 + i * 4, u->gun_x[i]);
        put16(u->frame + input_base + 0x22 + i * 4, u->gun_y[i]);
        if (shooter) {
            if (i == 0) {
                digital |= shooter_bits(u, p);
                if (p->start) lm |= 0x800;
                if (p->up) lm |= 0x200;
                if (p->down) lm |= 0x400;
                if (p->button[1]) lm |= 0x100;
            }
            continue;
        }
        if (p->start) digital |= (legacy ? 0x8000ULL : 0x800000ULL) << shift;
        if (p->up) digital |= (legacy ? 0x2000ULL : 0x200000ULL) << shift;
        if (p->down) digital |= (legacy ? 0x1000ULL : 0x100000ULL) << shift;
        if (p->left) digital |= (legacy ? 0x800ULL : 0x80000ULL) << shift;
        if (p->right) digital |= (legacy ? 0x400ULL : 0x40000ULL) << shift;
        if (p->button[0]) digital |= (legacy ? 0x200ULL : 0x20000ULL) << shift;
        if (p->button[1]) digital |= (legacy ? 0x100ULL : 0x10000ULL) << shift;
        if (p->button[2]) digital |= (legacy ? 0x800000ULL : 0x40000000ULL) << shift;
        if (p->button[3]) digital |= (legacy ? 0x400000ULL : 0x20000000ULL) << shift;
        // Legacy fifth-button wiring was not verified. Tekken6 uses four attacks.
        if (p->button[4] && !legacy) digital |= 0x80000000ULL << shift;
        if (i == 0) {
            if (p->start) lm |= 0x800;
            if (p->up) lm |= 0x200;
            if (p->down) lm |= 0x400;
            if (p->left) lm |= 0x2000;
            if (p->right) lm |= 0x4000;
            if (p->button[0]) lm |= 0x100;
        }
    }
    put16(u->frame, lm); u->frame[2] = 0x10;
    put64(u->frame + input_base, digital); put16(u->frame + input_base + 0x10, u->coins[0]);
    // Second I/O board is idle. This firmware wires one player's controls.
}
static bool valid_memory(const usio_protocol_t *u) {
    return u->channel >= 2 && u->channel - 2 < USIO_PAGE_COUNT && (uint32_t)u->reg + u->length <= USIO_PAGE_SIZE;
}
static void begin_read(usio_protocol_t *u) {
    memset(u->frame, 0, sizeof(u->frame)); u->read_position = 0; u->reading = true;
    if (u->channel != 0) return;
    switch (u->reg) {
    case 0x0000:
        memcpy(u->frame, USIO_KEEPALIVE, sizeof(USIO_KEEPALIVE));
        put16(u->frame, (uint16_t)u->time_ms);
        put16(u->frame + 0x14, usio_profile_version(u->profile)); break;
    case 0x0080: case 0x0090:
        memcpy(u->frame, USIO_CARD_STATUS, sizeof(USIO_CARD_STATUS));
        put16(u->frame + 2, u->readers[u->reg == 0x0090].output_size); break;
    case 0x1000: tekken_frame(u); break;
    case 0x1080: taiko_frame(u); break;
    case 0x1100: memcpy(u->frame, u->last_taiko, sizeof(u->last_taiko)); break;
    case 0x1800: case 0x1880:
        memcpy(u->frame, USIO_FIRMWARE_INFO + u->reg - 0x1800, sizeof(USIO_FIRMWARE_INFO) - (u->reg - 0x1800)); break;
    case 0x7000: case 0x7800: (void)usio_nfc_take(&u->readers[u->reg == 0x7800], u->frame, u->length < sizeof(u->frame) ? u->length : sizeof(u->frame)); break;
    default: u->unsupported_reads++; break;
    }
}
static void finish_write(usio_protocol_t *u) {
    if (u->channel == 0 && (u->reg == 0x7400 || u->reg == 0x7c00)) {
        usio_nfc_request(&u->readers[u->reg == 0x7c00], u->write_data, u->length);
    } else if (valid_memory(u)) {
        if (!u->memory.write || !u->memory.write(u->memory.context, (uint32_t)(u->channel - 2) * USIO_PAGE_SIZE + u->reg, u->write_data, u->length)) u->memory_errors++;
    } else if (u->channel == 0 && u->reg == 0x000a && u->length == 2 && get16(u->write_data) == 0x6666) {
        if (!u->memory.clear || !u->memory.clear(u->memory.context)) u->memory_errors++;
    } else if (!(u->channel == 0 && (u->reg == 0x0002 || u->reg == 0x0028 ||
                 (u->reg >= 0x0048 && u->reg <= 0x007a)))) u->unsupported_writes++;
    u->writing = false;
}
unsigned usio_protocol_feed(usio_protocol_t *u, const uint8_t *data, size_t size) {
    unsigned events = 0;
    while (size) {
        if (u->writing) {
            size_t n = u->length - u->write_used; if (n > size) n = size;
            memcpy(u->write_data + u->write_used, data, n); u->write_used += (uint32_t)n; data += n; size -= n;
            if (u->write_used == u->length) { finish_write(u); events |= USIO_EVENT_WRITE; }
            continue;
        }
        if (u->reading) { u->malformed_commands++; return events; } // Commands are serialized by the USIO protocol.
        size_t n = 6 - u->header_used; if (n > size) n = size;
        memcpy(u->header + u->header_used, data, n); u->header_used += (uint8_t)n; data += n; size -= n;
        if (u->header_used != 6) continue;
        u->commands++;
        u->header_used = 0; u->channel = u->header[0] & 15; u->reg = get16(u->header + 2); u->length = get16(u->header + 4);
        uint8_t command = u->header[0] & 0xf0;
        if (command == 0x90) {
            if (u->header[1] != ((~(u->reg >> 8)) & 0xf0)) { u->malformed_commands++; continue; }
            u->write_used = 0; u->writing = true;
            if (!u->length) { finish_write(u); events |= USIO_EVENT_WRITE; }
        } else if (command == 0x10) { begin_read(u); events |= USIO_EVENT_READ; }
        else if (command == 0xa0) {
            if (u->channel == 0 && u->reg == 0x000a && (!u->memory.clear || !u->memory.clear(u->memory.context))) u->memory_errors++;
            events |= USIO_EVENT_INIT;
        } else u->malformed_commands++;
    }
    return events;
}
size_t usio_protocol_read(usio_protocol_t *u, uint8_t *data, size_t capacity) {
    if (!u->reading) return 0;
    size_t n = u->length - u->read_position; if (n > capacity) n = capacity;
    memset(data, 0, n);
    if (valid_memory(u)) {
        if (!u->memory.read || !u->memory.read(u->memory.context,
            (uint32_t)(u->channel - 2) * USIO_PAGE_SIZE + u->reg + u->read_position, data, n)) u->memory_errors++;
    } else if (u->channel == 0 && u->read_position < sizeof(u->frame)) {
        size_t available = sizeof(u->frame) - u->read_position;
        memcpy(data, u->frame + u->read_position, n < available ? n : available);
    }
    u->read_position += (uint32_t)n;
    if (u->read_position == u->length) u->reading = false;
    return n;
}
