// SPDX-License-Identifier: GPL-2.0-only
#include "usio_protocol.h"

// Versions from game executable comparisons are numeric register values, not
// USB bcdDevice and not original Namco firmware images. 007e is the baseline
// RPCS3 response where a game-specific required version has not been proven.
static const usio_profile_info_t profiles[USIO_PROFILE_COUNT] = {
    [USIO_TAIKO] = {"Taiko", "USIO PCB rev00", 0x0910, 0x007e, USIO_LAYOUT_TAIKO},
    [USIO_TEKKEN] = {"Tag 2 Vanilla", "USIO PCB rev00", 0x0910, 0x007e, USIO_LAYOUT_FIGHTING},
    [USIO_SHOOTER] = {"Razing Storm", "USIO PCB rev00", 0x0910, 0x007e, USIO_LAYOUT_SHOOTER},
    [USIO_DEADSTORM] = {"Deadstorm Pirates", "USIO PCB rev00", 0x0910, 0x007e, USIO_LAYOUT_SHOOTER},
    [USIO_H050] = {"Tekken 6", "H050 USJ(C) PCB rev00", 0x0900, 0x2d84, USIO_LAYOUT_USJ},
    [USIO_TEKKEN6BR] = {"Tekken 6 BR", "H050 USJ(C) PCB rev00", 0x0900, 0x2e85, USIO_LAYOUT_USJ},
    [USIO_GUNDAM] = {"Gundam Maxi Boost", "USIO PCB rev00", 0x0910, 0x0203, USIO_LAYOUT_FIGHTING},
    [USIO_DARK_ESCAPE] = {"Dark Escape 4D", "USIO PCB rev00", 0x0910, 0x007e, USIO_LAYOUT_SHOOTER},
    [USIO_SAILOR_ZOMBIE] = {"Sailor Zombie AKB48", "USIO PCB rev00", 0x0910, 0x007e, USIO_LAYOUT_SHOOTER},
    [USIO_DRAGONBALL] = {"Dragon Ball Zenkai", "USIO PCB rev00", 0x0910, 0x007e, USIO_LAYOUT_FIGHTING},
    [USIO_TAG2_UNLIMITED] = {"Tag 2 Unlimited", "USIO PCB rev00", 0x0910, 0x007e, USIO_LAYOUT_FIGHTING},
};
const usio_profile_info_t *usio_profile_info(usio_profile_t p) {
    return &profiles[(unsigned)p < USIO_PROFILE_COUNT ? p : USIO_TAIKO];
}
const char *usio_profile_name(usio_profile_t p) { return usio_profile_info(p)->name; }
const char *usio_profile_board(usio_profile_t p) { return usio_profile_info(p)->board; }
uint16_t usio_profile_pid(usio_profile_t p) { return usio_profile_info(p)->pid; }
uint16_t usio_profile_version(usio_profile_t p) { return usio_profile_info(p)->version; }
usio_layout_t usio_profile_layout(usio_profile_t p) { return usio_profile_info(p)->layout; }
// All titles share Namco's USIO SDK: node N starts at input+0x80*N. Deadstorm
// reads node 1 (0x80; executable 0x325394, verified on a System 357). Dark Escape's
// game loop reads node 1 analog channels raw (0x27c69c); the same SDK and
// cabinet I/O put Razing Storm and Sailor Zombie there too. Tekken-family
// titles keep their hardware-verified node 2 (0x100).
unsigned usio_profile_input_base(usio_profile_t p) {
    return usio_profile_layout(p)==USIO_LAYOUT_SHOOTER ? 0x80 : 0x100;
}
bool usio_profile_has_gun(usio_profile_t p) {
    return usio_profile_layout(p)==USIO_LAYOUT_SHOOTER;
}
