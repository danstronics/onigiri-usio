// SPDX-License-Identifier: GPL-2.0-only
#include "usio_protocol.h"
#include "selector.h"
#include "controls.h"
#include "board_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static usio_protocol_t u;
static uint8_t ram[USIO_PAGE_COUNT * USIO_PAGE_SIZE], output[65535], payload[65535];
static unsigned writes;
static bool read_memory(void *c, uint32_t at, uint8_t *p, size_t size) { (void)c; assert(at + size <= sizeof(ram)); memcpy(p, ram + at, size); return true; }
static bool write_memory(void *c, uint32_t at, const uint8_t *p, size_t size) { (void)c; assert(at + size <= sizeof(ram)); memcpy(ram + at, p, size); writes++; return true; }
static bool clear_memory(void *c) { (void)c; memset(ram, 0, sizeof(ram)); return true; }
static void setup(usio_profile_t profile) { usio_protocol_init(&u, profile, (usio_memory_t){NULL, read_memory, write_memory, clear_memory}); }
static unsigned command(uint8_t op, uint16_t reg, uint16_t size) {
    uint8_t h[] = {op, (uint8_t)((~(reg >> 8)) & 0xf0), (uint8_t)reg, (uint8_t)(reg >> 8), (uint8_t)size, (uint8_t)(size >> 8)};
    // Deliberately fragment the header across transfers.
    assert(usio_protocol_feed(&u, h, 2) == 0); return usio_protocol_feed(&u, h + 2, 4);
}
static void read_response(uint8_t channel, uint16_t reg, uint16_t size) {
    assert(command((uint8_t)(0x10 | channel), reg, size) == USIO_EVENT_READ);
    unsigned at = 0; memset(output, 0xa5, sizeof(output));
    while (u.reading) { unsigned n = (unsigned)usio_protocol_read(&u, output + at, 63); at += n; assert(at <= size); }
    assert(at == size);
}
static uint64_t le64(const uint8_t *p) { uint64_t v = 0; for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static unsigned input_base(void) { return usio_profile_input_base(u.profile); }
static void input_tests(void) {
    usio_player_t p[2] = {0}; setup(USIO_TEKKEN);
    p[0].coin = p[0].test = p[0].start = p[0].up = p[0].button[0] = true;
    usio_protocol_input(&u, p, 1); // A short coin press between host polls is still counted.
    p[0].coin = p[0].test = false; usio_protocol_input(&u, p, 2);
    read_response(0, 0x1000, 384);
    assert(le16(output + input_base()+0x10) == 1 && output[2] == 0x10);
    assert(le64(output + input_base()) == (0x80 | 0x800000 | 0x200000 | 0x20000));
    assert(le16(output) == (0x1000 | 0x800 | 0x200 | 0x100));
    p[0].up = p[0].start = p[0].button[0] = false;
    for (unsigned i = 0; i < 5; i++) {
        const uint64_t bits[] = {0x20000, 0x10000, 0x40000000, 0x20000000, 0x80000000};
        p[0].button[i] = true; usio_protocol_input(&u, p, 10 + i);
        read_response(0, 0x1000, 384); assert(le64(output + input_base()) == (bits[i] | 0x80)); p[0].button[i] = false;
    }
    p[0].test = true; usio_protocol_input(&u, p, 30); usio_protocol_input(&u, p, 31);
    read_response(0, 0x1000, 384); assert(!(le64(output + input_base()) & 0x80));
    assert(u.coins[0] == 1);
    usio_protocol_reset_transport(&u); assert(u.coins[0] == 1);
    setup(USIO_TAIKO); memset(p, 0, sizeof(p));
    p[0].button[0] = p[0].button[1] = p[0].button[2] = p[0].button[3] = true;
    usio_protocol_input(&u, p, 1); read_response(0, 0x1080, 96);
    for (unsigned i = 0; i < 4; i++) assert(le16(output + 32 + i * 2) == 0xffff);
    read_response(0, 0x1100, 96); assert(le16(output + 32) == 0xffff);
    read_response(0, 0x1080, 96); for (unsigned i = 0; i < 4; i++) assert(le16(output + 32 + i * 2) == 0);
    setup(USIO_SHOOTER); memset(p, 0, sizeof(p)); p[0].right = p[0].button[2] = true;
    usio_protocol_input(&u, p, 8); read_response(0, 0x1000, 384);
    assert(le16(output + input_base()+0x20) == 0x80da && le16(output + input_base()+0x22) == 0x8000);
    assert(le64(output + input_base()) == 0x800000); // South fires; the D-pad only aims.
    puts("PASS Taiko edges/cache; Tekken 1P button/coin/test frames; shooter digital aiming.");
}
static void tekken_conformance_tests(void) {
    // RPCS3 fd027c99 (2026-11-10), translate_input_tekken(): each player's
    // digital controls occupy the same word, separated by 24 bits.
    static const struct { size_t field; uint64_t bit; uint16_t lm; } cases[] = {
        {offsetof(usio_player_t, start), 0x800000, 0x800},
        {offsetof(usio_player_t, up), 0x200000, 0x200},
        {offsetof(usio_player_t, down), 0x100000, 0x400},
        {offsetof(usio_player_t, left), 0x80000, 0x2000},
        {offsetof(usio_player_t, right), 0x40000, 0x4000},
        {offsetof(usio_player_t, button[0]), 0x20000, 0x100},
        {offsetof(usio_player_t, button[1]), 0x10000, 0},
        {offsetof(usio_player_t, button[2]), 0x40000000, 0},
        {offsetof(usio_player_t, button[3]), 0x20000000, 0},
        {offsetof(usio_player_t, button[4]), 0x80000000, 0},
    };
    for (unsigned profile = 0; profile < 2; profile++) {
        for (unsigned player = 0; player < 2; player++) {
            for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
                usio_player_t p[2] = {0}; setup(profile ? USIO_TAG2_UNLIMITED : USIO_TEKKEN);
                *(bool *)((uint8_t *)&p[player] + cases[i].field) = true;
                usio_protocol_input(&u, p, 1); read_response(0, 0x1000, 384);
                assert(le64(output + input_base()) == (cases[i].bit << (player * 24)));
                assert(le16(output) == (player == 0 ? cases[i].lm : 0));
                assert(output[2] == 0x10 && le64(output + 0x80) == 0);
            }
        }
    }
    usio_player_t p[2] = {0}; setup(USIO_H050);
    p[0].service = true; usio_protocol_input(&u, p, 1);
    read_response(0, 0x1000, 384); assert(le64(output + input_base()) == 0x4000);
    p[0].service = false; p[1].service = true; usio_protocol_input(&u, p, 2);
    read_response(0, 0x1000, 384); assert(le64(output + input_base()) == 0);
    assert(usio_profile_pid(USIO_H050) == 0x0900 && usio_profile_pid(USIO_TEKKEN) == 0x0910);
    assert(!strcmp(usio_profile_name(USIO_H050), "Tekken 6"));
    puts("PASS RPCS3 Tekken digital conformance: 40 button/direction cases, service, idle second board, 0900/0910 identities.");
}
static void stream_tests(void) {
    setup(USIO_TEKKEN); unsigned previous = writes;
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 17 + i / 256);
    assert(command(0x9f, 1, 65535) == 0);
    for (unsigned at = 0; at < sizeof(payload);) {
        size_t n = sizeof(payload) - at; if (n > 64) n = 64;
        unsigned event = usio_protocol_feed(&u, payload + at, n); at += (unsigned)n;
        assert(event == (at == sizeof(payload) ? USIO_EVENT_WRITE : 0));
    }
    assert(writes == previous + 1 && !u.writing);
    read_response(15, 1, 65535); assert(!memcmp(payload, output, sizeof(payload)));
    // Invalid cross-page writes must not alter any backing byte.
    assert(command(0x92, 65530, 9) == 0); assert(usio_protocol_feed(&u, payload, 9) == USIO_EVENT_WRITE);
    assert(writes == previous + 1); read_response(2, 65530, 9);
    for (unsigned i = 0; i < 9; i++) assert(output[i] == 0);
    // Read padding and unknown registers must not leak the previous response.
    read_response(0, 0x1800, 1024); assert(!memcmp(output, "NBGI.;USIO01;Ver1.00;", 20));
    for (unsigned i = 384; i < 1024; i++) assert(output[i] == 0);
    read_response(0, 0x4321, 100); for (unsigned i = 0; i < 100; i++) assert(output[i] == 0);
    read_response(0, 0, 0); assert(!u.reading);
    uint8_t bad[] = {0x92, 0, 0, 0, 0, 0}; assert(!usio_protocol_feed(&u, bad, sizeof(bad)) && u.malformed_commands);
    assert(command(0xa0, 0x000a, 0) == USIO_EVENT_INIT); assert(ram[13 * USIO_PAGE_SIZE + 1] == 0);
    puts("PASS fragmented headers, 65,535-byte writes/reads, invalid ranges, zero/padded responses, SRAM clear.");
}
static void nfc_tests(void) {
    setup(USIO_TEKKEN);
    const uint8_t poll[] = {0, 0, 0xff, 4, 0xfc, 0xd4, 0x4a, 1, 0, 0xe1, 0};
    assert(command(0x90, 0x7400, sizeof(poll)) == 0);
    assert(usio_protocol_feed(&u, poll, sizeof(poll)) == USIO_EVENT_WRITE);
    read_response(0, 0x0080, 16); unsigned size = le16(output + 2); assert(size > 6 && size < 64);
    read_response(0, 0x7000, (uint16_t)size);
    assert(!memcmp(output, "\0\0\xff\0\xff\0", 6));
    assert(output[12] == 0x4b && output[13] == 0); // No card found.
    read_response(0, 0x0080, 16); assert(le16(output + 2) == 0);
    puts("PASS card reader framing, ACK, pending length and no-card polling.");
}
static void selector_tests(void) {
    usio_selector_t s; usio_selector_init(&s, USIO_TAIKO);
    assert(!usio_selector_update(&s, true, true, false, false, 10) && usio_selector_captures(&s));
    assert(!usio_selector_update(&s, true, true, false, false, 2009) && !s.menu);
    assert(!usio_selector_update(&s, true, true, false, false, 2010) && s.menu);
    usio_selector_update(&s, false, false, false, false, 2020);
    usio_selector_update(&s, false, false, false, true, 2030);
    assert(s.selected == USIO_H050); // Taiko is last in the menu; Down wraps to Tekken 6.
    usio_selector_update(&s, false, false, false, false, 2040);
    assert(usio_selector_update(&s, true, false, false, false, 2050));
    assert(s.active == USIO_H050 && !s.menu && usio_selector_captures(&s));
    usio_selector_update(&s, false, false, false, false, 2060); assert(!usio_selector_captures(&s));
    // Timer wrap and cancelling without selecting a profile.
    usio_selector_update(&s, true, true, false, false, UINT32_MAX - 999);
    usio_selector_update(&s, true, true, false, false, 1000); assert(s.menu);
    usio_selector_update(&s, false, false, false, false, 1010);
    usio_selector_update(&s, false, true, false, false, 1020); assert(!s.menu && s.active == USIO_H050);
    assert(usio_profile_pid(USIO_H050) == 0x0900 && usio_profile_pid(USIO_TEKKEN) == 0x0910);
    puts("PASS two-button menu hold/release, vertical selection, confirm/cancel, timer wrap and profile IDs.");
}
static void game_profile_tests(void) {
    static const struct { usio_profile_t profile; uint16_t pid, version; } identities[] = {
        {USIO_H050,0x0900,0x2d84}, {USIO_TEKKEN6BR,0x0900,0x2e85},
        {USIO_GUNDAM,0x0910,0x0203}, {USIO_DARK_ESCAPE,0x0910,0x007e},
        {USIO_SAILOR_ZOMBIE,0x0910,0x007e}, {USIO_DRAGONBALL,0x0910,0x007e},
        {USIO_TAG2_VANILLA,0x0910,0x007e}, {USIO_TAG2_UNLIMITED,0x0910,0x007e}
    };
    for (unsigned i=0;i<sizeof(identities)/sizeof(identities[0]);i++) {
        setup(identities[i].profile);
        assert(usio_profile_pid(u.profile)==identities[i].pid);
        read_response(0,0,64);
        assert(le16(output+0x14)==identities[i].version);
        assert(le16(output+0x12)==0x007e); // Version is not the adjacent status field.
    }
    static const struct { size_t field; uint64_t bit; } legacy[] = {
        {offsetof(usio_player_t,start),0x8000}, {offsetof(usio_player_t,up),0x2000},
        {offsetof(usio_player_t,down),0x1000}, {offsetof(usio_player_t,left),0x800},
        {offsetof(usio_player_t,right),0x400}, {offsetof(usio_player_t,button[0]),0x200},
        {offsetof(usio_player_t,button[1]),0x100}, {offsetof(usio_player_t,button[2]),0x800000},
        {offsetof(usio_player_t,button[3]),0x400000}
    };
    for(unsigned profile=0;profile<2;profile++) for(unsigned i=0;i<sizeof(legacy)/sizeof(legacy[0]);i++) {
        setup(profile?USIO_TEKKEN6BR:USIO_H050);usio_player_t p[2]={0};
        *(bool *)((uint8_t *)&p[0]+legacy[i].field)=true;
        usio_protocol_input(&u,p,10);read_response(0,0x1000,384);
        assert(le64(output+input_base())==legacy[i].bit);
    }
    for(unsigned profile=0;profile<USIO_PROFILE_COUNT;profile++) {
        if(usio_profile_layout((usio_profile_t)profile)!=USIO_LAYOUT_SHOOTER)continue;
        setup((usio_profile_t)profile);usio_player_t p[2]={0};
        p[0].button[0]=p[0].button[1]=p[0].right=true;
        usio_protocol_input(&u,p,8);read_response(0,0x1000,384);
        assert(le64(output+input_base())==(0x20000ULL|0x200000ULL)); // West Start, North extra.
        assert(input_base()==0x80 && le16(output+input_base()+0x20)==0x80da);
        // Input frame exposes no invented vital-sensor signal or wheel motion.
        assert(output[0xa9]==0 && output[0xab]==0 && output[0xb0]==0);
    }
    usio_selector_t s;usio_selector_init(&s,USIO_TAIKO);s.menu=true;
    for(unsigned i=1;i<USIO_PROFILE_COUNT;i++) {
        usio_selector_update(&s,false,false,false,true,100+i*2);
        usio_selector_update(&s,false,false,false,false,101+i*2);
        assert(s.selected==usio_menu_profile(i-1)); // Menu order, not profile numbers.
    }
    usio_selector_update(&s,false,false,false,true,200);assert(s.selected==USIO_TAIKO);
    puts("PASS per-game PID/version, Tekken6/BR legacy P1 masks, all shooter profiles, full selector traversal.");
}
static unsigned pressed_fields(const usio_player_t *p) {
    unsigned count = p->up + p->down + p->left + p->right + p->start + p->service + p->coin + p->test + p->l;
    for (unsigned i = 0; i < 5; i++) count += p->button[i];
    return count;
}
static void gpio_sample(uint32_t pins, bool enabled, uint32_t time) {
    usio_player_t p[2]; usio_controls_from_gpio(pins, enabled, p);
    assert(pressed_fields(&p[1]) == 0);
    usio_protocol_input(&u, p, time);
}
static void common_gpio_tests(void) {
    uint32_t all = 0;
    for (unsigned i = 0; i < USIO_CONTROL_COUNT; i++) {
        unsigned pin = usio_control_pins[i];
        assert(pin < 30 && !(all & (1u << pin))); all |= 1u << pin;
        usio_player_t p[2]; usio_controls_from_gpio(1u << pin, true, p);
        assert(pressed_fields(&p[0]) == 1 && pressed_fields(&p[1]) == 0);
    }
    for (unsigned profile = 0; profile < USIO_PROFILE_COUNT; profile++) {
        setup((usio_profile_t)profile);
        gpio_sample(1u << PIN_COIN, true, 1);
        gpio_sample(1u << PIN_COIN, true, 2); // Holding Coin never repeats.
        gpio_sample(0, true, 3);
        read_response(0, 0x1080, 96); assert(le16(output) == 0 && le16(output + 16) == 1);
        read_response(0, 0x1000, 384); assert(le64(output + input_base()) == 0 && le16(output + input_base()+0x10) == 1);
        gpio_sample(1u << PIN_TEST, true, 4);
        gpio_sample(1u << PIN_TEST, true, 5); // Test is latched on an edge.
        read_response(0, 0x1080, 96); assert(le16(output) == 0x80);
        read_response(0, 0x1000, 384); assert(le64(output + input_base()) == 0x80);
        gpio_sample(0, true, 6); gpio_sample(1u << PIN_TEST, true, 7);
        read_response(0, 0x1080, 96); assert(le16(output) == 0);
        gpio_sample(1u << PIN_SERVICE, true, 8);
        read_response(0, 0x1080, 96); assert(le16(output) == 0x4000);
        read_response(0, 0x1000, 384); assert(le64(output + input_base()) == 0x4000);
        assert(le16(output + input_base()+0x10) == 1); // Service is not a coin or an attack.
        gpio_sample(1u << PIN_START, true, 9);
        read_response(0, 0x1080, 96); assert(le16(output) == 0x200);
        read_response(0, 0x1000, 384);
        usio_layout_t layout = usio_profile_layout((usio_profile_t)profile);
        uint64_t start = layout == USIO_LAYOUT_USJ ? 0x8000 : layout == USIO_LAYOUT_SHOOTER ? 0x200 : 0x800000;
        assert(le64(output + input_base()) == start && le16(output) == 0x800);
        gpio_sample(1u << PIN_UP, true, 10);
        read_response(0, 0x1080, 96); assert(le16(output) == 0x2000);
        read_response(0, 0x1000, 384); assert(le16(output) == 0x200);
        gpio_sample(1u << PIN_DOWN, true, 11);
        read_response(0, 0x1080, 96); assert(le16(output) == 0x1000);
        read_response(0, 0x1000, 384); assert(le16(output) == 0x400);
        gpio_sample(all, false, 12); // Game Select captures every control.
        read_response(0, 0x1080, 96); assert(le16(output) == 0 && le16(output + 16) == 1);
        read_response(0, 0x1000, 384); assert(le64(output + input_base()) == 0 && le16(output + input_base()+0x10) == 1);
    }
    puts("PASS common GPIO controls on all 11 profiles: exclusive Coin/Test/Service, shared Enter/Start, menu D-pad, hold edges and selector capture.");
}
static void playable_gpio_tests(void) {
    static const unsigned directions[] = {PIN_UP, PIN_DOWN, PIN_LEFT, PIN_RIGHT};
    static const uint64_t fighting_dirs[] = {0x200000, 0x100000, 0x80000, 0x40000};
    static const uint64_t legacy_dirs[] = {0x2000, 0x1000, 0x800, 0x400};
    static const unsigned actions[] = {PIN_BUTTON1, PIN_BUTTON2, PIN_BUTTON3, PIN_BUTTON4, PIN_BUTTON5};
    static const uint64_t fighting_actions[] = {0x20000, 0x10000, 0x40000000, 0x20000000, 0x80000000};
    static const uint64_t legacy_actions[] = {0x200, 0x100, 0x800000, 0x400000};
    unsigned profiles = 0;
    for (unsigned profile = 0; profile < USIO_PROFILE_COUNT; profile++) {
        usio_layout_t layout = usio_profile_layout((usio_profile_t)profile);
        if (layout == USIO_LAYOUT_SHOOTER) continue;
        profiles++;
        for (unsigned i = 0; i < (layout == USIO_LAYOUT_TAIKO ? 2u : 4u); i++) {
            setup((usio_profile_t)profile); gpio_sample(1u << directions[i], true, 1);
            if (layout == USIO_LAYOUT_TAIKO) {
                read_response(0, 0x1080, 96); assert(le16(output) == (i ? 0x1000 : 0x2000));
            } else {
                read_response(0, 0x1000, 384);
                assert(le64(output + input_base()) == (layout == USIO_LAYOUT_USJ ? legacy_dirs[i] : fighting_dirs[i]));
            }
        }
        unsigned count = layout == USIO_LAYOUT_FIGHTING ? 5 : 4;
        for (unsigned i = 0; i < count; i++) {
            setup((usio_profile_t)profile); gpio_sample(1u << actions[i], true, 1);
            gpio_sample(0, true, 2); // A short hit must survive until the host polls.
            if (layout == USIO_LAYOUT_TAIKO) {
                read_response(0, 0x1080, 96);
                for (unsigned hit = 0; hit < 4; hit++) assert(le16(output + 32 + hit * 2) == (hit == i ? 0xffff : 0));
                assert(le16(output) == 0 && le16(output + 16) == 0);
            } else {
                // Fighting actions are level inputs, unlike queued Taiko hits.
                gpio_sample(1u << actions[i], true, 3); read_response(0, 0x1000, 384);
                assert(le64(output + input_base()) == (layout == USIO_LAYOUT_USJ ? legacy_actions[i] : fighting_actions[i]));
                assert(le16(output + input_base()+0x10) == 0 && !u.test_on);
            }
        }
    }
    assert(profiles == 7);
    puts("PASS physical GPIO-to-gameplay packets: D-pad and action buttons on all 7 non-shooter profiles, including four queued Taiko hits and legacy Tekken6/BR attacks.");
}
static void dpad_aiming(void) {
    const usio_profile_t guns[]={USIO_DEADSTORM,USIO_SHOOTER,USIO_DARK_ESCAPE,USIO_SAILOR_ZOMBIE};
    for(unsigned g=0;g<4;g++) {
        setup(guns[g]);assert(input_base()==0x80 && usio_profile_has_gun(guns[g]));
        usio_player_t p[2]; memset(p,0,sizeof(p));
        // A short tap moves a little; holding accelerates; release stops.
        p[0].left=true;
        for(uint32_t t=8;t<=40;t+=8)usio_protocol_input(&u,p,t);
        read_response(0,0x1000,384);unsigned tap=0x8000-le16(output+0xa0);
        assert(tap>0 && tap<2000 && le16(output+0xa2)==0x8000);
        for(uint32_t t=48;t<=1000;t+=8)usio_protocol_input(&u,p,t);
        read_response(0,0x1000,384);assert(le16(output+0xa0)==0); // Clamped at the edge.
        memset(p,0,sizeof(p));usio_protocol_input(&u,p,1008);
        p[0].down=true;usio_protocol_input(&u,p,1016);
        read_response(0,0x1000,384);assert(le16(output+0xa2)==0x8000+218); // Fine step again.
        p[0].down=false;p[0].up=true;
        for(uint32_t t=1024;t<=3000;t+=8)usio_protocol_input(&u,p,t);
        read_response(0,0x1000,384);assert(le16(output+0xa2)==0 && le16(output+0xa0)==0);
        // Player 2 gun stays centred; Deadstorm keeps its menu D-pad bits.
        assert(le16(output+0xa4)==0x8000 && le16(output+0xa6)==0x8000);
        if(guns[g]==USIO_DEADSTORM)assert(le64(output+0x80)&0x2000);
    }
    for(unsigned p=0;p<USIO_PROFILE_COUNT;p++)
        if(!usio_profile_has_gun((usio_profile_t)p))assert(usio_profile_input_base((usio_profile_t)p)==0x100);
    puts("PASS D-pad aiming on Deadstorm/Razing/Dark Escape/Sailor Zombie: node 1 analog X/Y, fine tap, acceleration, edge clamp, P2 centred, Deadstorm menu bits kept.");
}
// Shooters: action buttons play, top-row keys are system functions.
static void shooter_controls(void) {
    static const struct { unsigned pin; uint64_t bit; } map[] = {
        {PIN_BUTTON3, 0x800000}, {PIN_BUTTON4, 0x400000},   // South / East: fire
        {PIN_BUTTON1, 0x200000}, {PIN_BUTTON2, 0x20000},    // West: Start, North: extra
        {PIN_START, 0x200}, {PIN_SERVICE, 0x4000},          // +: Enter, -: Service
        {PIN_LEFT, 0}, {PIN_RIGHT, 0}, {PIN_BUTTON5, 0}, {PIN_L, 0},
    };
    for (unsigned profile = 0; profile < USIO_PROFILE_COUNT; profile++) {
        if (usio_profile_layout((usio_profile_t)profile) != USIO_LAYOUT_SHOOTER) continue;
        for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            if (profile == USIO_DEADSTORM && map[i].pin == PIN_L) continue; // DX toggle, below.
            setup((usio_profile_t)profile); gpio_sample(1u << map[i].pin, true, 1);
            read_response(0, 0x1000, 384);
            assert(le64(output + 0x80) == map[i].bit);
            assert(le64(output + 0x100) == 0); // Nothing leaks to the idle second node.
        }
    }
    // L toggles the Deadstorm DX cabinet bit (0x100); holding never repeats.
    setup(USIO_DEADSTORM);
    gpio_sample(1u << PIN_L, true, 1); gpio_sample(1u << PIN_L, true, 2);
    read_response(0, 0x1000, 384); assert(le64(output + 0x80) == 0x100 && u.dx_on);
    gpio_sample(0, true, 3); read_response(0, 0x1000, 384); assert(le64(output + 0x80) == 0x100);
    gpio_sample(1u << PIN_BUTTON2, true, 4); read_response(0, 0x1000, 384);
    assert(le64(output + 0x80) == (0x100 | 0x20000)); // North no longer changes DX.
    gpio_sample(1u << PIN_L, true, 5); gpio_sample(0, true, 6);
    read_response(0, 0x1000, 384); assert(le64(output + 0x80) == 0 && !u.dx_on);
    setup(USIO_SHOOTER); gpio_sample(1u << PIN_L, true, 1); assert(!u.dx_on);
    setup(USIO_H050); gpio_sample(1u << PIN_L, true, 1); read_response(0, 0x1000, 384);
    assert(le64(output + 0x100) == 0 && !u.dx_on);
    puts("PASS shooter controls: South/East fire, West Start, North extra, + Enter, - Service, D-pad aim only; L toggles Deadstorm DX and nothing else.");
}
int main(void) { input_tests(); tekken_conformance_tests(); stream_tests(); nfc_tests(); selector_tests(); game_profile_tests(); common_gpio_tests(); playable_gpio_tests(); dpad_aiming(); shooter_controls(); return 0; }
