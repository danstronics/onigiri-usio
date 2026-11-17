// SPDX-License-Identifier: GPL-2.0-only
#include "usio_flash.h"
#include "hardware/flash.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
uint8_t test_flash[PICO_FLASH_SIZE_BYTES];
static unsigned erases, programs, safe_calls;
static bool fail_next;
static uint8_t data[65535], result[65535];
void flash_range_erase(uint32_t at, size_t size) {
    assert(at % FLASH_SECTOR_SIZE == 0 && size % FLASH_SECTOR_SIZE == 0);
    assert(at >= 256 * 1024 && at + size <= sizeof(test_flash) - 8192);
    memset(test_flash + at, 0xff, size); erases++;
}
void flash_range_program(uint32_t at, const uint8_t *p, size_t size) {
    assert(at % FLASH_PAGE_SIZE == 0 && size % FLASH_PAGE_SIZE == 0);
    assert(at >= 256 * 1024 && at + size <= sizeof(test_flash) - 8192);
    for (size_t i = 0; i < size; i++) { assert((test_flash[at + i] & p[i]) == p[i]); test_flash[at + i] &= p[i]; }
    programs++;
}
int flash_safe_execute(void (*fn)(void *), void *arg, unsigned timeout) {
    assert(timeout > 0); safe_calls++;
    if (fail_next) { fail_next = false; return -1; }
    fn(arg); return 0;
}
static uint32_t test_time=10000;
uint32_t get_absolute_time(void) { return test_time; }
uint32_t to_ms_since_boot(uint32_t time) { return time; }
static uint32_t legacy_crc(const uint8_t *p,size_t size) {
    uint32_t crc=0xffffffffu;
    while(size--) { crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^((crc&1)?0xedb88320u:0); }
    return ~crc;
}
static void legacy_bank_tests(void) {
    // Fixture from the previous five-profile layout, not the new offset formula.
    typedef struct { uint32_t magic,generation,profile,crc;uint8_t valid[32]; } old_header_t;
    const uint32_t select=PICO_FLASH_SIZE_BYTES-16384u,bank=1048576u+8192u;
    memset(test_flash,0xff,sizeof(test_flash));
    unsigned count=PICO_FLASH_SIZE_BYTES>=8u*1024u*1024u?5:1;
    for(unsigned i=0;i<count;i++) {
        old_header_t h={.magic=0x554f4e47u,.generation=3,.profile=i};h.valid[0]=1;
        h.crc=legacy_crc((const uint8_t *)&h,sizeof(h));
        uint32_t at=select-count*bank+i*bank;
        memcpy(test_flash+at,&h,sizeof(h));memset(test_flash+at+8192,0x30+(int)i,4096);
    }
    for(unsigned i=0;i<count;i++) {
        usio_memory_t m=usio_flash_open((usio_profile_t)i);
        assert(m.read(NULL,0,result,8));
        for(unsigned j=0;j<8;j++)assert(result[j]==0x30+i);
    }
    memset(test_flash,0xff,sizeof(test_flash));
    puts("PASS firmware upgrade reads existing five-profile bank geometry without moving data.");
}
static void background_persistence_tests(void) {
    memset(test_flash,255,sizeof(test_flash));
    usio_memory_t m=usio_flash_open(USIO_DEADSTORM);
    uint8_t settings[256],readback[256];
    for(unsigned i=0;i<sizeof(settings);i++)settings[i]=(uint8_t)(i*37u+9u);
    assert(m.write(NULL,0x40,settings,sizeof(settings)));
    assert(m.write(NULL,USIO_PAGE_SIZE+0x40,settings,sizeof(settings)));
    // Settings written by the game reach flash in the background, without a
    // forced flush, so a power cut after a few seconds keeps them.
    unsigned before=programs;
    test_time+=5000;
    for(unsigned i=0;i<1000;i++)usio_flash_tick(false);
    assert(programs>before && usio_flash_can_accept());
    assert(m.read(NULL,0x40,readback,sizeof(readback)) && !memcmp(settings,readback,sizeof(settings)));
    m=usio_flash_open(USIO_DEADSTORM);
    assert(m.read(NULL,0x40,readback,sizeof(readback)) && !memcmp(settings,readback,sizeof(settings)));
    assert(m.read(NULL,USIO_PAGE_SIZE+0x40,readback,sizeof(readback)) && !memcmp(settings,readback,sizeof(settings)));
    // Existing normal background persistence must remain for Tekken6.
    m=usio_flash_open(USIO_H050);
    assert(m.write(NULL,0x40,settings,sizeof(settings)));
    before=programs;test_time+=5000;usio_flash_tick(false);
    assert(programs>before && usio_flash_flush());
    // Pressure relief when the write-back cache is full.
    m=usio_flash_open(USIO_DEADSTORM);
    for(unsigned i=0;i<sizeof(data);i++)data[i]=(uint8_t)(i^(i>>8));
    assert(m.write(NULL,1,data,sizeof(data)));
    assert(m.write(NULL,USIO_PAGE_SIZE+1,data,sizeof(data)));
    assert(!usio_flash_can_accept());
    before=programs;usio_flash_tick(false);assert(programs>before);
    assert(m.read(NULL,1,result,sizeof(result)) && !memcmp(data,result,sizeof(data)));
    assert(usio_flash_flush() && usio_flash_can_accept());
    m=usio_flash_open(USIO_DEADSTORM);
    assert(m.read(NULL,USIO_PAGE_SIZE+1,result,sizeof(result)) && !memcmp(data,result,sizeof(data)));
    puts("PASS background persistence on Deadstorm and Tekken6, live readback, reopen, pressure relief.");
}
static void selection_option_tests(void) {
    memset(test_flash,255,sizeof(test_flash));
    assert(usio_flash_saved_profile()==USIO_TAIKO && usio_flash_saved_options()==0);
    assert(usio_flash_save_selection(USIO_DEADSTORM,USIO_OPTION_DX));
    assert(usio_flash_saved_profile()==USIO_DEADSTORM && usio_flash_saved_options()==USIO_OPTION_DX);
    assert(usio_flash_save_profile(USIO_H050)); // Changing game keeps the option.
    assert(usio_flash_saved_profile()==USIO_H050 && usio_flash_saved_options()==USIO_OPTION_DX);
    assert(usio_flash_save_selection(USIO_H050,0) && usio_flash_saved_options()==0);
    puts("PASS saved game selection with the persistent DX option.");
}

int main(void) {
    legacy_bank_tests();
    background_persistence_tests();
    selection_option_tests();
    memset(test_flash, 0xff, sizeof(test_flash));
    assert(usio_flash_saved_profile() == USIO_TAIKO);
    usio_memory_t memory = usio_flash_open(USIO_TAIKO);
    assert(memory.read(NULL, 65500, result, 100));
    for (unsigned i = 0; i < 100; i++) assert(result[i] == 0);
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i ^ (i >> 8));
    // Two full pages fill the cache. Further OUT commands must receive backpressure.
    assert(memory.write(NULL, 1, data, sizeof(data)));
    assert(memory.write(NULL, USIO_PAGE_SIZE + 1, data, sizeof(data)));
    assert(!usio_flash_can_accept());
    assert(memory.read(NULL, 1, result, sizeof(result)) && !memcmp(data, result, sizeof(data)));
    memset(result, 0, sizeof(result));
    assert(!memory.write(NULL, 2 * USIO_PAGE_SIZE, data, sizeof(data))); // Refusal is atomic, not a partial write.
    assert(memory.read(NULL, 2 * USIO_PAGE_SIZE, result, 8)); for (unsigned i = 0; i < 8; i++) assert(!result[i]);
    assert(usio_flash_flush() && usio_flash_can_accept());
    memory = usio_flash_open(USIO_TAIKO); assert(memory.read(NULL, 1, result, sizeof(result)) && !memcmp(data, result, sizeof(data)));
    unsigned saved_programs = programs;
    assert(memory.write(NULL, 1, data, sizeof(data)) && usio_flash_flush()); assert(programs == saved_programs);
    // Exercise the entire mapped 1 MiB instead of only Taiko's first 8 KiB.
    for (unsigned page = 2; page < USIO_PAGE_COUNT; page++) {
        assert(memory.write(NULL, page * USIO_PAGE_SIZE + 1, data, sizeof(data)));
        assert(usio_flash_flush());
        memory = usio_flash_open(USIO_TAIKO);
        assert(memory.read(NULL, page * USIO_PAGE_SIZE + 1, result, sizeof(result)) && !memcmp(data, result, sizeof(data)));
    }
    assert(!memory.write(NULL, USIO_PAGE_COUNT * USIO_PAGE_SIZE - 4, data, 8));
    assert(usio_flash_save_profile(USIO_TEKKEN)); assert(usio_flash_saved_profile() == USIO_TEKKEN);
    memory = usio_flash_open(USIO_TEKKEN); assert(memory.read(NULL, 1, result, 8));
    for (unsigned i = 0; i < 8; i++) assert(!result[i]); // No other game's bookkeeping leaks into a new profile.
    assert(usio_flash_flush());
    memory = usio_flash_open(USIO_TAIKO); assert(memory.read(NULL, 1, result, 8));
    if (PICO_FLASH_SIZE_BYTES >= 8u * 1024u * 1024u) assert(!memcmp(data, result, 8));
    else for (unsigned i = 0; i < 8; i++) assert(!result[i]); // 2 MiB board has one active bank.
    assert(memory.write(NULL, 100, data, 50)); assert(usio_flash_flush());
    assert(memory.clear(NULL)); assert(usio_flash_flush());
    memory = usio_flash_open(USIO_TAIKO); assert(memory.read(NULL, 100, result, 50));
    for (unsigned i = 0; i < 50; i++) assert(!result[i]);
    assert(memory.write(NULL, 100, data, 50)); fail_next = true;
    unsigned before = usio_flash_errors(); assert(!usio_flash_flush()); assert(usio_flash_errors() == before + 1);
    assert(memory.read(NULL, 100, result, 50) && !memcmp(data, result, 50)); // Failed flush keeps dirty cache for retry.
    assert(usio_flash_flush());
    for(unsigned p=0;p<USIO_PROFILE_COUNT;p++) {
        memory=usio_flash_open((usio_profile_t)p);
        uint8_t marker=(uint8_t)(p+1);
        assert(memory.write(NULL,128,&marker,1)&&usio_flash_flush());
        assert(usio_flash_save_profile((usio_profile_t)p)&&usio_flash_saved_profile()==(usio_profile_t)p);
    }
    if(PICO_FLASH_SIZE_BYTES>=8u*1024u*1024u) {
        for(unsigned p=0;p<USIO_PROFILE_COUNT;p++) {
            memory=usio_flash_open((usio_profile_t)p);
            assert(memory.read(NULL,128,result,1)&&result[0]==p+1);
        }
    } else {
        memory=usio_flash_open((usio_profile_t)(USIO_PROFILE_COUNT-1));
        assert(memory.read(NULL,128,result,1)&&result[0]==USIO_PROFILE_COUNT);
        memory=usio_flash_open(USIO_TAIKO);assert(memory.read(NULL,128,result,1)&&result[0]==0);
    }
    assert(erases == programs && erases == safe_calls - 1);
    printf("PASS %u MiB flash: full address space, write-back/readback, backpressure, clear, profile banks, failed flush retry; %u verified commits.\n", PICO_FLASH_SIZE_BYTES / (1024 * 1024), programs);
    return 0;
}
