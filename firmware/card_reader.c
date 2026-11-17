// SPDX-License-Identifier: GPL-2.0-only
// Adapted from RPCS3 nfc_reader.cpp. No card identity or authentication data.
#include "card_reader.h"
#include <stdbool.h>
#include <string.h>
void usio_nfc_request(usio_nfc_t *n, const uint8_t *data, size_t size) {
    while (size >= 2 && !(data[0] == 0 && data[1] == 0xff)) { data++; size--; }
    if (size < 4) return;
    if (data[2] == 0xff && data[3] == 0) { // NACK: resend last response, without ACK.
        memcpy(n->output, n->response, n->response_size); n->output_size = n->response_size; return;
    }
    if (data[2] == 0 && data[3] == 0xff) { n->output_size = n->response_size = 0; return; }
    bool extended = data[2] == 0xff && data[3] == 0xff;
    unsigned tfi = extended ? 7 : 4;
    if (size <= tfi) return;
    unsigned length = extended ? ((unsigned)data[4] << 8 | data[5]) : data[2];
    unsigned check = extended ? data[4] + data[5] + data[6] : data[2] + data[3];
    if (length < 2 || (check & 255) || data[tfi] != 0xd4 || tfi + length >= size) return;
    uint8_t sum = 0; for (unsigned i = tfi; i <= tfi + length; i++) sum = (uint8_t)(sum + data[i]);
    if (sum) return;
    const uint8_t *payload = data + tfi + 1; unsigned payload_size = length - 1;
    uint8_t body[16] = {(uint8_t)(payload[0] + 1)}; unsigned body_size = 1;
    switch (payload[0]) {
    case 0x06:
        if (payload_size < 3) return;
        if (payload[2] == 0x1c) { const uint8_t values[] = {0xff, 0x3f, 0x0e, 0xf1, 0xff, 0x3f, 0x0e, 0xf1}; memcpy(body + 1, values, sizeof(values)); body_size += sizeof(values); }
        else { const uint8_t values[] = {0xdc, 0xf4, 0x3f, 0x11, 0x4d, 0x85, 0x61, 0xf1, 0x26, 0x6a, 0x87}; memcpy(body + 1, values, sizeof(values)); body_size += sizeof(values); }
        break;
    case 0x08: body[body_size++] = 0; break;
    case 0x0c: body[body_size++] = 0; body[body_size++] = 6; body[body_size++] = 0; break;
    case 0x0e: case 0x12: case 0x18: case 0x32: break;
    case 0x40: body[body_size++] = 1; break; // No card present.
    case 0x4a: body[body_size++] = 0; break;
    case 0x52: body[body_size++] = 1; body[body_size++] = 0; break;
    default: body_size = 0; break;
    }
    static const uint8_t ack[] = {0, 0, 0xff, 0, 0xff, 0};
    memcpy(n->output, ack, sizeof(ack)); unsigned at = 0;
    if (!body_size) {
        const uint8_t error[] = {0, 0, 0xff, 1, 0xff, 0x7f, 0x81, 0};
        memcpy(n->response, error, sizeof(error)); at = sizeof(error);
    } else {
        unsigned len = body_size + 1;
        n->response[at++] = 0; n->response[at++] = 0; n->response[at++] = 0xff;
        n->response[at++] = (uint8_t)len; n->response[at++] = (uint8_t)(0 - len); n->response[at++] = 0xd5;
        uint8_t checksum = 0xd5;
        for (unsigned i = 0; i < body_size; i++) { n->response[at++] = body[i]; checksum = (uint8_t)(checksum + body[i]); }
        n->response[at++] = (uint8_t)(0 - checksum); n->response[at++] = 0;
    }
    n->response_size = (uint8_t)at; memcpy(n->output + sizeof(ack), n->response, at);
    n->output_size = (uint8_t)(sizeof(ack) + at);
}
size_t usio_nfc_take(usio_nfc_t *n, uint8_t *data, size_t capacity) {
    size_t size = n->output_size < capacity ? n->output_size : capacity;
    memcpy(data, n->output, size);
    memmove(n->output, n->output + size, n->output_size - size); n->output_size -= (uint8_t)size;
    return size;
}
