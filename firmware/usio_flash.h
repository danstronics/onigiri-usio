// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONIGIRI_USIO_FLASH_H
#define ONIGIRI_USIO_FLASH_H
#include "usio_protocol.h"
usio_memory_t usio_flash_open(usio_profile_t);
bool usio_flash_can_accept(void);
void usio_flash_tick(bool force);
bool usio_flash_flush(void);
usio_profile_t usio_flash_saved_profile(void);
bool usio_flash_save_profile(usio_profile_t);
enum { USIO_OPTION_DX = 1 };
uint8_t usio_flash_saved_options(void);
bool usio_flash_save_selection(usio_profile_t, uint8_t options);
uint32_t usio_flash_errors(void);
#endif
