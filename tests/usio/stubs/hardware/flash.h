#ifndef TEST_FLASH_H
#define TEST_FLASH_H
#include <stddef.h>
#include <stdint.h>
#define FLASH_SECTOR_SIZE 4096
#define FLASH_PAGE_SIZE 256
extern uint8_t test_flash[];
#define XIP_BASE ((uintptr_t)test_flash)
void flash_range_erase(uint32_t, size_t);
void flash_range_program(uint32_t, const uint8_t *, size_t);
#endif
