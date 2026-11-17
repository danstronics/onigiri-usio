// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONIGIRI_USIO_USB_H
#define ONIGIRI_USIO_USB_H
#include "usio_protocol.h"
void usio_usb_init(usio_protocol_t *);
void usio_usb_task(void);
bool usio_usb_idle(void);
uint8_t usio_usb_state(void);
void usio_usb_disconnect(void);
uint32_t usio_usb_race_recoveries(void);
// True while the System has the USIO interface configured.
bool usio_usb_opened(void);
#endif
