// SPDX-License-Identifier: GPL-2.0-only
#ifndef ONIGIRI_CARD_READER_H
#define ONIGIRI_CARD_READER_H
#include <stddef.h>
#include <stdint.h>
// Card reader (PN5xx-style NFC framing, as in RPCS3) that always reports no card.
typedef struct { uint8_t response[64], output[72]; uint8_t response_size, output_size; } usio_nfc_t;
void usio_nfc_request(usio_nfc_t *, const uint8_t *, size_t);
size_t usio_nfc_take(usio_nfc_t *, uint8_t *, size_t);
#endif
