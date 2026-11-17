// SPDX-License-Identifier: GPL-2.0-only
// TinyUSB transport adapted from iTaiko usio_driver.c.
#include "usio_usb.h"
#include "usio_flash.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include <string.h>
#ifdef USIO_HOST_TEST_CLOCK
extern uint32_t usio_usb_test_time_us(void);
#define reply_time_us usio_usb_test_time_us
#else
#include "pico/time.h"
#define reply_time_us time_us_32
#endif

enum { EP_OUT = 0x01, EP_IN = 0x82, EP_STATUS = 0x83, PACKET = 64, REPLY_HOLD_US = 1000 };
static usio_protocol_t *protocol;
static bool opened, out_armed, response_pending, need_terminator, idle_zlp;
static uint16_t response_length;
static bool reply_waiting;
static uint32_t reply_started_us;
static CFG_TUSB_MEM_ALIGN uint8_t out_packet[PACKET], in_packet[PACKET], status_packet[8];
enum { OUT_QUEUE_CAPACITY = 16 };
static struct { uint16_t size; uint8_t data[PACKET]; } out_queue[OUT_QUEUE_CAPACITY];
static uint8_t out_queue_head, out_queue_tail, out_queue_count;

// Every title except Taiko ships Namco's USIO SDK (Deadstorm, Tekken 6/BR,
// Tag 2, Gundam, Dragon Ball, Razing Storm, Dark Escape, Sailor Zombie). Its
// read callback completes on the exact byte count with no drain transfer; the
// write-payload callback submits a zero-length IN that the device must ACK.
// Tekken 6 keeps the immediate transport verified on hardware.
static bool uses_async_stock_driver(void) {
    return protocol->profile!=USIO_TAIKO && protocol->profile!=USIO_H050;
}
static bool uses_drain_zlps(void) {
    return protocol->profile!=USIO_H050 && !uses_async_stock_driver();
}
// Host SDK callback race (e.g. Deadstorm 0x5d5310/0x5d5480): a synchronous
// completion posts the context semaphore (max count 1) and only then reloads
// ctx+0x24. If the status thread wins the semaphore in between, the stale
// status callback submits READ 0x1000 before the status READ was answered.
// Both header completions then use the new context: the host queues TWO INs
// of the input length for ONE reply, and the next write leaves one extra
// zero-length ACK IN. A healthy flow never sends a header while the status
// READ is unanswered, so the signature is unambiguous. Recovery: answer only
// the input READ, then retire the stale IN with one extra ZLP after the next
// write ACK. Same race in every non-Taiko title (docs/SDK-callback-race.md).
static uint8_t stale_in_debt, extra_zlps;
static uint32_t race_recoveries;
static bool recovers_callback_race(void) { return protocol->profile!=USIO_TAIKO; }
static uint16_t header16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0110, .bDeviceClass = 0xff, .bDeviceSubClass = 0,
    .bDeviceProtocol = 0xff, .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x0b9a, .idProduct = 0x0910, .bcdDevice = 0x0910,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 0, .bNumConfigurations = 1,
};
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, 39, 0xc0, 100),
    9, TUSB_DESC_INTERFACE, 0, 0, 3, 0, 0, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_OUT, TUSB_XFER_BULK, 64, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_IN, TUSB_XFER_BULK, 64, 0, 0,
    7, TUSB_DESC_ENDPOINT, EP_STATUS, TUSB_XFER_INTERRUPT, 8, 0, 16,
};
const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return configuration; }
const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid; static uint16_t result[64]; const char *text;
    if (index == 0) { result[0] = (TUSB_DESC_STRING << 8) | 4; result[1] = 0x0409; return result; }
    if (index == 1) text = "NAMCO";
    else if (index == 2) text = usio_profile_board(protocol->profile);
    // The SDK queries index 3 even with iSerialNumber == 0; a STALL makes its
    // string reader fail, so answer with an empty descriptor like RPCS3.
    else if (index == 3 && uses_async_stock_driver()) text = "";
    else return NULL;
    size_t n = strlen(text); if (n > 63) n = 63;
    for (size_t i = 0; i < n; i++) result[i + 1] = (uint8_t)text[i];
    result[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2)); return result;
}
static void arm_out(void) {
    if (!opened || out_armed || (!protocol->writing && !protocol->header_used && !usio_flash_can_accept())) return;
    // Apply OUT backpressure only when the bounded raw-packet FIFO is full.
    if (uses_async_stock_driver() && out_queue_count == OUT_QUEUE_CAPACITY) return;
    out_armed = usbd_edpt_xfer(0, EP_OUT, out_packet, sizeof(out_packet));
}
static void hold_reply(void) { reply_started_us = reply_time_us(); reply_waiting = true; }
static void pump(void) {
    if (!opened || usbd_edpt_busy(0, EP_IN)) return;
    if (reply_waiting) {
        if ((uint32_t)(reply_time_us() - reply_started_us) < REPLY_HOLD_US) return;
        reply_waiting = false;
    }
    if (response_pending) {
        if (protocol->reading) {
            size_t n = usio_protocol_read(protocol, in_packet, sizeof(in_packet));
            if (n) { (void)usbd_edpt_xfer(0, EP_IN, in_packet, (uint16_t)n); return; }
        }
        response_pending = false;
        if (response_length == 0 || need_terminator) { need_terminator = false; (void)usbd_edpt_xfer(0, EP_IN, NULL, 0); }
    } else if (idle_zlp) {
        idle_zlp = false;
        (void)usbd_edpt_xfer(0, EP_IN, NULL, 0);
    }
}
static void consume_out(const uint8_t *data, size_t size) {
    unsigned events = usio_protocol_feed(protocol, data, size);
    // Deadstorm (verified on hardware): every reply and ACK waits one frame.
    // Other SDK titles hold only the status READ, so the stale callback's
    // READ 0x1000 arrives before a single status byte has left.
    if (protocol->profile == USIO_DEADSTORM && (events & (USIO_EVENT_READ | USIO_EVENT_WRITE))) hold_reply();
    else if (uses_async_stock_driver() && (events & USIO_EVENT_READ) &&
             protocol->channel == 0 && protocol->reg == 0x0000) hold_reply();
    if (stale_in_debt && (events & (USIO_EVENT_READ | USIO_EVENT_WRITE))) {
        // The input callback always continues with WRITE 0x1400. Any other
        // order means the host differs from the analysed driver: stop
        // compensating rather than guessing at another pending transfer.
        if (events & USIO_EVENT_WRITE) extra_zlps = stale_in_debt;
        stale_in_debt = 0;
    }
    if (events & USIO_EVENT_READ) {
        response_pending = true; response_length = protocol->length;
        need_terminator = uses_drain_zlps() && protocol->length % PACKET == 0;
    }
    if (events & USIO_EVENT_WRITE) idle_zlp = true;
}
// A lone READ 0x1000 header arrives while the status READ is still being
// answered. Normally no byte has left yet; with Tekken 6's immediate transport
// one full packet may be on its way. The host's first IN then holds those
// bytes and takes the rest from the input reply.
static bool callback_race_detected(const uint8_t *next, uint16_t size) {
    if (!recovers_callback_race() || stale_in_debt || extra_zlps) return false;
    if (protocol->writing || protocol->header_used || !protocol->reading || !response_pending) return false;
    if (idle_zlp || need_terminator) return false;
    if (protocol->channel != 0 || protocol->reg != 0x0000) return false;
    if (size != 6 || next[0] != 0x10 || header16(next + 2) != 0x1000) return false;
    uint32_t sent = protocol->read_position;
    return sent % PACKET == 0 && sent < header16(next + 4);
}
static void recover_callback_race(const uint8_t *next) {
    uint32_t sent = protocol->read_position;
    protocol->reading = false;
    response_pending = need_terminator = reply_waiting = false;
    race_recoveries++;
    consume_out(next, 6);
    protocol->read_position = sent;   // Continue the host's first IN, not restart it.
    stale_in_debt = 1;                // Set after the replacement READ is consumed.
}
static void drain_out_queue(void) {
    if (!uses_async_stock_driver() || !opened) return;
    pump();
    while (out_queue_count) {
        // Keep each command's read context until all its data and optional
        // ACK have completed. A WRITE or fragmented header may consume its
        // remaining OUT bytes immediately, preserving packet order.
        const uint8_t *next = out_queue[out_queue_tail].data;
        uint16_t size = out_queue[out_queue_tail].size;
        bool race = false;
        if (!protocol->writing && !protocol->header_used &&
            (protocol->reading || response_pending || idle_zlp || extra_zlps || usbd_edpt_busy(0, EP_IN))) {
            if (!callback_race_detected(next, size)) return;
            race = true;
        }
        if (race) recover_callback_race(next); else consume_out(next, size);
        out_queue_tail = (uint8_t)((out_queue_tail + 1) % OUT_QUEUE_CAPACITY);
        out_queue_count--;
        pump();
    }
}
static void clear_transport(void) {
    opened = out_armed = response_pending = idle_zlp = need_terminator = false;
    out_queue_head = out_queue_tail = out_queue_count = 0;
    reply_waiting = false; reply_started_us = 0; stale_in_debt = extra_zlps = 0;
}
static void reset(uint8_t rhport) {
    (void)rhport; clear_transport();
    if (protocol) usio_protocol_reset_transport(protocol);
    // Keep credits/test state and pending storage across USB bus resets.
}
static void init(void) { reset(0); }
static uint16_t open_interface(uint8_t rhport, const tusb_desc_interface_t *itf, uint16_t max_length) {
    TU_VERIFY(itf->bInterfaceClass == 0 && itf->bInterfaceSubClass == 0 && itf->bInterfaceProtocol == 0 && itf->bNumEndpoints == 3, 0);
    TU_VERIFY(max_length >= 30, 0); reset(rhport);
    const uint8_t *p = (const uint8_t *)itf + sizeof(*itf);
    for (unsigned i = 0; i < 3; i++, p = tu_desc_next(p)) {
        const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p;
        TU_VERIFY(ep->bDescriptorType == TUSB_DESC_ENDPOINT &&
            (ep->bEndpointAddress == EP_OUT || ep->bEndpointAddress == EP_IN || ep->bEndpointAddress == EP_STATUS), 0);
        TU_ASSERT(usbd_edpt_open(rhport, ep), 0);
    }
    opened = true; arm_out(); idle_zlp = uses_drain_zlps(); pump();
    memset(status_packet, 0, sizeof(status_packet));
    (void)usbd_edpt_xfer(rhport, EP_STATUS, status_packet, sizeof(status_packet));
    return 30;
}
static bool control(uint8_t rhport, uint8_t stage, const tusb_control_request_t *request) {
    (void)rhport; (void)stage; (void)request; return false;
}
static bool transfer(uint8_t rhport, uint8_t endpoint, xfer_result_t result, uint32_t size) {
    if (result != XFER_RESULT_SUCCESS) { reset(rhport); return false; }
    if (endpoint == EP_OUT) {
        out_armed = false;
        if (uses_async_stock_driver()) {
            // Endpoint arming reserves one free slot, so the last OUT
            // accepted by hardware always has room and cannot be dropped.
            if (out_queue_count >= OUT_QUEUE_CAPACITY || size > PACKET) { reset(rhport); return false; }
            out_queue[out_queue_head].size = (uint16_t)size;
            memcpy(out_queue[out_queue_head].data, out_packet, size);
            out_queue_head = (uint8_t)((out_queue_head + 1) % OUT_QUEUE_CAPACITY);
            out_queue_count++;
            drain_out_queue();
        } else if (callback_race_detected(out_packet, (uint16_t)size)) {
            // Tekken 6: replaces the old malformed-header drop that left the host stuck.
            recover_callback_race(out_packet);
        } else consume_out(out_packet, size);
        arm_out(); pump();
    } else if (endpoint == EP_IN) {
        // The ordinary write ACK closed the stale input IN; the next ZLP
        // closes the write's own zero-length ACK IN.
        if (extra_zlps && !size && !response_pending && !idle_zlp && !protocol->reading) {
            extra_zlps--; idle_zlp = true;
        }
        pump();
        if (uses_async_stock_driver()) { drain_out_queue(); arm_out(); }
    }
    else if (endpoint == EP_STATUS) (void)usbd_edpt_xfer(rhport, EP_STATUS, status_packet, sizeof(status_packet));
    return true;
}
static const usbd_class_driver_t driver = {
    .init = init, .reset = reset, .open = open_interface,
    .control_xfer_cb = control, .xfer_cb = transfer, .sof = NULL,
};
const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *count) { *count = 1; return &driver; }
void usio_usb_init(usio_protocol_t *u) {
    protocol = u; race_recoveries = 0;
    device.idProduct = usio_profile_pid(u->profile);
    device.bcdDevice = device.idProduct;
    tud_init(0);
}
void usio_usb_task(void) { if (!tud_inited()) return; tud_task(); drain_out_queue(); arm_out(); pump(); }
bool usio_usb_idle(void) {
    return !opened || (!protocol->writing && !protocol->reading && !response_pending && !reply_waiting &&
                       !extra_zlps && !out_queue_count && !usbd_edpt_busy(0, EP_IN));
}
bool usio_usb_opened(void) { return opened; }
uint32_t usio_usb_race_recoveries(void) { return race_recoveries; }
uint8_t usio_usb_state(void) {
    return (opened ? 1 : 0) | (out_armed ? 2 : 0) | (protocol->reading ? 4 : 0) |
           (protocol->writing ? 8 : 0) | (response_pending ? 16 : 0) |
           (usbd_edpt_busy(0, EP_IN) ? 32 : 0) | (idle_zlp ? 64 : 0) | (need_terminator ? 128 : 0);
}
void usio_usb_disconnect(void) { tud_disconnect(); tud_deinit(0); clear_transport(); }
