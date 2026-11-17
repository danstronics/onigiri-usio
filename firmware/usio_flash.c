// SPDX-License-Identifier: GPL-2.0-only
#include "usio_flash.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include <string.h>

// Full 1 MiB address space, with a 128 KiB write-back cache. Never truncate a host write.
// Leave the last 8 KiB (stock iTaiko settings/auth) untouched to allow restoring stock firmware.
#define SRAM_BYTES (USIO_PAGE_SIZE * USIO_PAGE_COUNT)
#define TAIL_BYTES (2u * FLASH_SECTOR_SIZE)
#define PROFILE_SLOTS (PICO_FLASH_SIZE_BYTES >= 8u * 1024u * 1024u ? USIO_PROFILE_COUNT : 1)
#define BANK_BYTES (SRAM_BYTES + 2u * FLASH_SECTOR_SIZE)
#define DATA_BASE (PICO_FLASH_SIZE_BYTES - TAIL_BYTES - PROFILE_SLOTS * BANK_BYTES - 2u * FLASH_SECTOR_SIZE)
#define SELECT_BASE (PICO_FLASH_SIZE_BYTES - TAIL_BYTES - 2u * FLASH_SECTOR_SIZE)
#define MAGIC 0x554f4e47u
enum { CACHE_COUNT = 32, SECTOR_COUNT = SRAM_BYTES / FLASH_SECTOR_SIZE };
typedef struct { uint32_t magic, generation, profile, crc; uint8_t valid[SECTOR_COUNT / 8]; } bank_header_t;
typedef struct { bool used; uint16_t sector; uint8_t data[FLASH_SECTOR_SIZE]; } cache_t;
static cache_t cache[CACHE_COUNT];
static bank_header_t header;
static uint32_t base, header_slot, last_write_ms, errors;
static bool need_header;
static uint8_t flash_page[FLASH_PAGE_SIZE];
extern const char __flash_binary_end;
_Static_assert(DATA_BASE > 256u * 1024u, "Flash is too small for firmware and SRAM storage");
static uint32_t crc32(const uint8_t *p, size_t size) {
    uint32_t crc = 0xffffffffu;
    while (size--) { crc ^= *p++; for (unsigned i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0); }
    return ~crc;
}
static bool valid_header(const bank_header_t *h) {
    bank_header_t copy = *h; uint32_t crc = copy.crc; copy.crc = 0;
    return copy.magic == MAGIC && copy.profile < USIO_PROFILE_COUNT && crc == crc32((const uint8_t *)&copy, sizeof(copy));
}
typedef struct { uint32_t offset, erase_size, size; const uint8_t *data; } flash_job_t;
static void run_job(void *arg) {
    flash_job_t *j = arg;
    if (j->erase_size) flash_range_erase(j->offset, j->erase_size);
    if (j->size) flash_range_program(j->offset, j->data, j->size);
}
static bool program(uint32_t offset, const uint8_t *data, uint32_t size, uint32_t erase_size) {
    flash_job_t job = {offset, erase_size, size, data};
    bool success = flash_safe_execute(run_job, &job, 1000) == PICO_OK &&
        !memcmp((const void *)(XIP_BASE + offset), data, size);
    if (!success) { errors++; return false; }
    return true;
}
static bool save_header(void) {
    bank_header_t next = header; next.magic = MAGIC; next.generation++; next.crc = 0;
    next.crc = crc32((const uint8_t *)&next, sizeof(next));
    memset(flash_page, 0xff, sizeof(flash_page)); memcpy(flash_page, &next, sizeof(next));
    uint32_t slot = header_slot ^ 1;
    if (!program(base + slot * FLASH_SECTOR_SIZE, flash_page, sizeof(flash_page), FLASH_SECTOR_SIZE)) return false;
    header = next; header_slot = slot; need_header = false; return true;
}
static const bank_header_t *best_header(uint32_t at) {
    const bank_header_t *a = (const bank_header_t *)(XIP_BASE + at), *b = (const bank_header_t *)(XIP_BASE + at + FLASH_SECTOR_SIZE);
    bool va = valid_header(a), vb = valid_header(b);
    if (!va) return vb ? b : NULL;
    return vb && (int32_t)(b->generation - a->generation) > 0 ? b : a;
}
static cache_t *find_sector(unsigned sector) {
    for (unsigned i = 0; i < CACHE_COUNT; i++) if (cache[i].used && cache[i].sector == sector) return &cache[i];
    return NULL;
}
bool usio_flash_can_accept(void) {
    unsigned count = 0; for (unsigned i = 0; i < CACHE_COUNT; i++) count += !cache[i].used;
    return count >= 16; // Enough space for any single valid 65,535-byte USIO transaction.
}
static bool read_memory(void *context, uint32_t address, uint8_t *data, size_t size) {
    (void)context; if (address > SRAM_BYTES || size > SRAM_BYTES - address) return false;
    while (size) {
        unsigned sector = address / FLASH_SECTOR_SIZE, inside = address % FLASH_SECTOR_SIZE;
        size_t n = FLASH_SECTOR_SIZE - inside; if (n > size) n = size;
        cache_t *c = find_sector(sector);
        if (c) memcpy(data, c->data + inside, n);
        else if (header.valid[sector / 8] & (1u << (sector % 8))) memcpy(data, (const void *)(XIP_BASE + base + 2 * FLASH_SECTOR_SIZE + address), n);
        else memset(data, 0, n);
        address += (uint32_t)n; data += n; size -= n;
    }
    return true;
}
static bool write_memory(void *context, uint32_t address, const uint8_t *data, size_t size) {
    (void)context; if (address > SRAM_BYTES || size > SRAM_BYTES - address) return false;
    // Preflight capacity so failure cannot leave half a write in the cache.
    unsigned needed = 0, available = 0;
    for (unsigned i = 0; i < CACHE_COUNT; i++) available += !cache[i].used;
    if (size) for (unsigned sector = address / FLASH_SECTOR_SIZE; sector <= (address + size - 1) / FLASH_SECTOR_SIZE; sector++) needed += find_sector(sector) == NULL;
    if (needed > available) { errors++; return false; }
    bool changed = false;
    while (size) {
        unsigned sector = address / FLASH_SECTOR_SIZE, inside = address % FLASH_SECTOR_SIZE;
        size_t n = FLASH_SECTOR_SIZE - inside; if (n > size) n = size;
        cache_t *c = find_sector(sector);
        if (!c) for (unsigned i = 0; i < CACHE_COUNT; i++) if (!cache[i].used) {
            c = &cache[i]; read_memory(NULL, sector * FLASH_SECTOR_SIZE, c->data, FLASH_SECTOR_SIZE);
            c->sector = (uint16_t)sector; break;
        }
        if (memcmp(c->data + inside, data, n)) { memcpy(c->data + inside, data, n); c->used = true; changed = true; }
        address += (uint32_t)n; data += n; size -= n;
    }
    if (changed) last_write_ms = to_ms_since_boot(get_absolute_time());
    return true;
}
static bool clear_memory(void *context) {
    (void)context; memset(cache, 0, sizeof(cache)); memset(header.valid, 0, sizeof(header.valid));
    need_header = true; return true;
}
void usio_flash_tick(bool force) {
    if (!force && usio_flash_can_accept() && (uint32_t)(to_ms_since_boot(get_absolute_time()) - last_write_ms) < 750) return;
    // Persist invalidation before accepting a new sector into a cleared bank.
    if (need_header && !save_header()) return;
    for (unsigned i = 0; i < CACHE_COUNT; i++) if (cache[i].used) {
        cache_t *c = &cache[i];
        if (!program(base + 2 * FLASH_SECTOR_SIZE + c->sector * FLASH_SECTOR_SIZE, c->data, FLASH_SECTOR_SIZE, FLASH_SECTOR_SIZE)) return;
        if (!(header.valid[c->sector / 8] & (1u << (c->sector % 8)))) {
            header.valid[c->sector / 8] |= (uint8_t)(1u << (c->sector % 8)); need_header = true;
            if (!save_header()) return;
        }
        c->used = false; return; // Service USB between individual sector commits.
    }
}
bool usio_flash_flush(void) {
    uint32_t before = errors;
    for (unsigned i = 0; i <= CACHE_COUNT; i++) { usio_flash_tick(true); if (errors != before) return false; }
    return !need_header;
}
usio_memory_t usio_flash_open(usio_profile_t profile) {
    memset(cache, 0, sizeof(cache));
    // Original five banks occupied SELECT_BASE - 5*BANK_BYTES .. SELECT_BASE.
    // Keep their exact offsets when appending profiles, so an update cannot
    // reinterpret or discard existing Taiko/Tekken/shooter bookkeeping.
    unsigned bank_from_top = (unsigned)profile < 5 ? 5 - (unsigned)profile : (unsigned)profile + 1;
    base = PROFILE_SLOTS == 1 ? DATA_BASE : SELECT_BASE - bank_from_top * BANK_BYTES;
    const bank_header_t *saved = best_header(base);
    memset(&header, 0, sizeof(header)); header_slot = 1;
    if (saved) { header = *saved; header_slot = ((uintptr_t)saved - XIP_BASE - base) / FLASH_SECTOR_SIZE; }
    need_header = !saved || header.profile != (uint32_t)profile;
    if (need_header) { header.profile = (uint32_t)profile; memset(header.valid, 0, sizeof(header.valid)); }
#ifndef ONIGIRI_HOST_TEST
    if ((uintptr_t)&__flash_binary_end - XIP_BASE > DATA_BASE) panic("Firmware overlaps SRAM storage");
#endif
    return (usio_memory_t){NULL, read_memory, write_memory, clear_memory};
}
usio_profile_t usio_flash_saved_profile(void) {
    const bank_header_t *saved = best_header(SELECT_BASE);
    return saved ? (usio_profile_t)saved->profile : USIO_TAIKO;
}
// The selection record reuses the bank header; its sector bitmap is unused
// there, so byte 0 carries persistent options (bit 0: Deadstorm DX cabinet).
uint8_t usio_flash_saved_options(void) {
    const bank_header_t *saved = best_header(SELECT_BASE);
    return saved ? saved->valid[0] : 0;
}
bool usio_flash_save_profile(usio_profile_t profile) { return usio_flash_save_selection(profile, usio_flash_saved_options()); }
bool usio_flash_save_selection(usio_profile_t profile, uint8_t options) {
    const bank_header_t *saved = best_header(SELECT_BASE);
    if (saved && saved->profile == (uint32_t)profile && saved->valid[0] == options) return true;
    bank_header_t next = {0}; next.magic = MAGIC; next.profile = (uint32_t)profile; next.generation = saved ? saved->generation + 1 : 1;
    next.valid[0] = options;
    next.crc = crc32((const uint8_t *)&next, sizeof(next));
    uint32_t slot = saved ? (((uintptr_t)saved - XIP_BASE - SELECT_BASE) / FLASH_SECTOR_SIZE) ^ 1 : 0;
    memset(flash_page, 0xff, sizeof(flash_page)); memcpy(flash_page, &next, sizeof(next));
    return program(SELECT_BASE + slot * FLASH_SECTOR_SIZE, flash_page, sizeof(flash_page), FLASH_SECTOR_SIZE);
}
uint32_t usio_flash_errors(void) { return errors; }
