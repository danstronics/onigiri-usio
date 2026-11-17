// SPDX-License-Identifier: GPL-2.0-only
// Real TinyUSB device/control stack and firmware class driver, with a simulated DCD.
#include "usio_usb.h"
#include "controls.h"
#include "board_config.h"
#include "tusb.h"
#include "device/dcd.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { bool pending, stalled; uint8_t *buffer; uint16_t size; } transfer_t;
static transfer_t endpoints[16][2];
static usio_protocol_t protocol;
static uint8_t memory[USIO_PAGE_COUNT * USIO_PAGE_SIZE], received[65535], pattern[65535];
static bool storage_ready = true;
static bool native_exact_reads(usio_profile_t profile) { return profile!=USIO_TAIKO; }
static unsigned open_count;
bool usio_flash_can_accept(void) { return storage_ready; }
uint32_t usio_flash_errors(void) { return 0; }
static transfer_t *endpoint(uint8_t ep) { return &endpoints[ep & 15][ep >> 7]; }
bool dcd_init(uint8_t port, const tusb_rhport_init_t *init) { assert(port == 0 && init->role == TUSB_ROLE_DEVICE); return true; }
bool dcd_deinit(uint8_t port) { (void)port; memset(endpoints, 0, sizeof(endpoints)); return true; }
void dcd_int_enable(uint8_t port) { (void)port; }
void dcd_int_disable(uint8_t port) { (void)port; }
void dcd_int_handler(uint8_t port) { (void)port; }
void dcd_connect(uint8_t port) { (void)port; }
void dcd_disconnect(uint8_t port) { (void)port; }
void dcd_remote_wakeup(uint8_t port) { (void)port; }
void dcd_sof_enable(uint8_t port, bool enable) { (void)port; (void)enable; }
void dcd_edpt0_status_complete(uint8_t port, const tusb_control_request_t *request) { (void)port; (void)request; }
void dcd_edpt_close(uint8_t port, uint8_t ep) { (void)port; memset(endpoint(ep), 0, sizeof(transfer_t)); }
void dcd_edpt_close_all(uint8_t port) { (void)port; for (unsigned i = 1; i < 16; i++) memset(endpoints[i], 0, sizeof(endpoints[i])); }
void dcd_edpt_stall(uint8_t port, uint8_t ep) { (void)port; endpoint(ep)->stalled = true; endpoint(ep)->pending = false; }
void dcd_edpt_clear_stall(uint8_t port, uint8_t ep) { (void)port; endpoint(ep)->stalled = false; }
bool dcd_edpt_open(uint8_t port, const tusb_desc_endpoint_t *ep) {
    assert(port == 0); assert(ep->bEndpointAddress == 1 || ep->bEndpointAddress == 0x82 || ep->bEndpointAddress == 0x83);
    assert(tu_edpt_packet_size(ep) == (ep->bEndpointAddress == 0x83 ? 8 : 64)); open_count++; return true;
}
bool dcd_edpt_xfer(uint8_t port, uint8_t ep, uint8_t *buffer, uint16_t size) {
    assert(port == 0); transfer_t *t = endpoint(ep); assert(!t->pending && !t->stalled);
    t->pending = true; t->buffer = buffer; t->size = size; return true;
}
void dcd_set_address(uint8_t port, uint8_t address) { (void)address; assert(dcd_edpt_xfer(port, 0x80, NULL, 0)); }
static uint32_t usb_time_us;
uint32_t tusb_time_millis_api(void) { return usb_time_us/1000; }
uint32_t usio_usb_test_time_us(void) { return usb_time_us; }
static void advance_usb_us(unsigned elapsed) { usb_time_us+=elapsed;usio_usb_task(); }
static void advance_usb_ms(unsigned elapsed) { advance_usb_us(elapsed*1000); }
static bool mem_read(void *context, uint32_t at, uint8_t *data, size_t size) { (void)context; assert(at + size <= sizeof(memory)); memcpy(data, memory + at, size); return true; }
static bool mem_write(void *context, uint32_t at, const uint8_t *data, size_t size) { (void)context; assert(at + size <= sizeof(memory)); memcpy(memory + at, data, size); return true; }
static bool mem_clear(void *context) { (void)context; memset(memory, 0, sizeof(memory)); return true; }
static void complete(uint8_t ep, uint32_t size) {
    transfer_t *t = endpoint(ep); assert(t->pending && size <= t->size); t->pending = false;
    dcd_event_xfer_complete(0, ep, size, XFER_RESULT_SUCCESS, false); usio_usb_task();
}
static size_t host_in(uint8_t ep, uint8_t *buffer, size_t capacity) {
    transfer_t *t = endpoint(ep);
    for(unsigned wait=0;!t->pending && wait<2;wait++)advance_usb_ms(1);
    assert(t->pending); size_t n = t->size; assert(n <= capacity);
    if (n) memcpy(buffer, t->buffer, n);
    complete(ep, (uint32_t)n); return n;
}
static void host_out(const uint8_t *data, size_t size) {
    transfer_t *t = endpoint(1); assert(t->pending && size <= t->size);
    memcpy(t->buffer, data, size); complete(1, (uint32_t)size);
}
static size_t control_in(uint8_t type, uint8_t request, uint16_t value, uint16_t length, uint8_t *out, size_t capacity) {
    uint16_t language = request == TUSB_REQ_GET_DESCRIPTOR && value >> 8 == TUSB_DESC_STRING && (value & 255) ? 0x0409 : 0;
    uint8_t setup[] = {type, request, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)language, (uint8_t)(language >> 8), (uint8_t)length, (uint8_t)(length >> 8)};
    endpoint(0)->stalled = endpoint(0x80)->stalled = false;
    dcd_event_setup_received(0, setup, false); usio_usb_task(); size_t at = 0;
    for (unsigned i = 0; i < 1000; i++) {
        if (endpoint(0x80)->pending) at += host_in(0x80, out + at, capacity - at);
        else if (endpoint(0)->pending) { assert(endpoint(0)->size == 0); complete(0, 0); }
        else return at;
    }
    assert(!"Control transfer did not terminate"); return 0;
}
static void command(uint8_t op, uint16_t reg, uint16_t size) {
    uint8_t header[] = {op, (uint8_t)((~(reg >> 8)) & 0xf0), (uint8_t)reg, (uint8_t)(reg >> 8), (uint8_t)size, (uint8_t)(size >> 8)};
    host_out(header, 2); host_out(header + 2, 4);
}
static void read_register(uint8_t channel, uint16_t reg, uint16_t size) {
    command((uint8_t)(0x10 | channel), reg, size); size_t at = 0;
    while (at < size) { size_t n = host_in(0x82, received + at, sizeof(received) - at); assert(n > 0); at += n; }
    assert(at == size);
    if (size == 0 || (!native_exact_reads(protocol.profile) && size % 64 == 0)) assert(host_in(0x82, received + at, sizeof(received) - at) == 0);
    assert(!endpoint(0x82)->pending);
}
static void configure(usio_profile_t profile) {
    usio_protocol_init(&protocol, profile, (usio_memory_t){NULL, mem_read, mem_write, mem_clear});
    usio_usb_init(&protocol); memset(endpoints, 0, sizeof(endpoints)); open_count = 0;
    dcd_event_bus_reset(0, TUSB_SPEED_FULL, false); usio_usb_task();
    size_t n = control_in(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 18, received, sizeof(received));
    assert(n == 18 && received[7] == 8 && received[8] == 0x9a && received[9] == 0x0b);
    assert((received[10] | received[11] << 8) == usio_profile_pid(profile));
    assert((received[12] | received[13] << 8) == usio_profile_pid(profile));
    assert(control_in(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0200, 255, received, sizeof(received)) == 39);
    assert(received[4] == 1 && received[13] == 3); // One interface, three endpoints.
    assert(control_in(0x00, TUSB_REQ_SET_ADDRESS, 1, 0, received, sizeof(received)) == 0);
    assert(control_in(0x00, TUSB_REQ_SET_CONFIGURATION, 1, 0, received, sizeof(received)) == 0);
    assert(tud_mounted() && open_count == 3 && endpoint(1)->pending && endpoint(0x83)->pending);
    if (!native_exact_reads(profile)) assert(host_in(0x82, received, sizeof(received)) == 0);
    else assert(!endpoint(0x82)->pending); // Native driver has no initial drain.
}
static unsigned input_base(void) { return usio_profile_input_base(protocol.profile); }
static uint64_t input_word(void) {
    uint64_t result = 0;
    for (unsigned i = 0; i < 8; i++) result |= (uint64_t)received[input_base()+i] << (i * 8);
    return result;
}

static void deadstorm_transaction_timing(void) {
    configure(USIO_DEADSTORM);usb_time_us=100000;
    const uint8_t status[]={0x10,0,0,0,0,1};
    host_out(status,sizeof(status));
    // A whole reply waits one USB frame, not one frame per 64-byte packet.
    assert(!endpoint(0x82)->pending);
    usio_usb_task();assert(!endpoint(0x82)->pending);
    advance_usb_us(999);assert(!endpoint(0x82)->pending);
    advance_usb_us(1);assert(endpoint(0x82)->pending);
    unsigned before=usb_time_us;size_t at=0;
    while(at<256)at+=host_in(0x82,received+at,sizeof(received)-at);
    assert(at==256 && usb_time_us==before && received[0x21]==0x80);
    // WRITE completion keeps its explicit ACK, but does not release it in
    // the same frame as the last payload packet. Test unsigned clock wrap.
    usb_time_us=UINT32_MAX;
    command(0x90,0x1400,2);const uint8_t data[]={0,0};host_out(data,2);
    assert(!endpoint(0x82)->pending && !usio_usb_idle());
    advance_usb_ms(1);assert(usb_time_us==999 && endpoint(0x82)->pending);
    assert(host_in(0x82,received,sizeof(received))==0);
    assert(usio_usb_idle());usio_usb_disconnect();
    // The native-tested Tekken6 path must remain immediate.
    configure(USIO_H050);host_out(status,sizeof(status));assert(endpoint(0x82)->pending);
    at=0;while(at<256)at+=host_in(0x82,received+at,sizeof(received)-at);
    usio_usb_disconnect();
    puts("PASS Deadstorm first-response and post-WRITE ACK frame delay, no per-packet delay, clock wrap and unchanged immediate Tekken6 transport.");
}

static void original_driver_contracts(void) {
    // Every non-Taiko title ships the same Namco USIO SDK (docs/SDK-callback-race.md).
    const uint8_t status[]={0x10,0,0,0,0,1},inputs[]={0x10,0,0,0x10,0x80,1};
    for(unsigned profile=0;profile<USIO_PROFILE_COUNT;profile++) {
        if(profile==USIO_TAIKO || profile==USIO_H050)continue;
        configure((usio_profile_t)profile);
        // These original SDKs request string index 3 even with iSerial=0.
        assert(control_in(0x80,TUSB_REQ_GET_DESCRIPTOR,0x0303,2,received,sizeof(received))==2);
        assert(received[0]==2 && received[1]==TUSB_DESC_STRING && !endpoint(0x80)->stalled);
        usio_player_t p[2]={0};p[0].coin=p[0].test=true;
        usio_protocol_input(&protocol,p,1);
        for(unsigned cycle=0;cycle<32;cycle++) {
            host_out(status,6);
            size_t at=0;while(at<256)at+=host_in(0x82,received+at,sizeof(received)-at);
            assert(at==256 && received[0x20]==0 && received[0x21]==0x80);
            host_out(inputs,6);
            at=0;while(at<384)at+=host_in(0x82,received+at,sizeof(received)-at);
            assert(at==384 && (received[input_base()]&0x80) && received[input_base()+0x10]==1);
            advance_usb_ms(3);
            assert(!endpoint(0x82)->pending && !protocol.malformed_commands); // No read drain.
            uint8_t outputs[96]={0};command(0x90,0x1400,sizeof(outputs));
            host_out(outputs,64);host_out(outputs+64,32);
            if(!endpoint(0x82)->pending)advance_usb_ms(1);
            assert(host_in(0x82,received,sizeof(received))==0); // Required write ACK retained.
            assert(usio_usb_idle());
        }
        usio_usb_disconnect();
    }
    puts("PASS SDK transport contract on all 9 non-Taiko USIO profiles (incl. Razing/Dark Escape/Sailor Zombie): empty optional serial, no initial/read-drain ZLP, explicit WRITE ACK retained.");
}

// One host bulk IN URB: it ends when the requested length is reached or on a
// short packet (including a ZLP), exactly like the PS3 controller.
static size_t host_bulk_in(size_t requested) {
    size_t at=0;
    for(;;) {
        size_t n=host_in(0x82,received+at,sizeof(received)-at);at+=n;
        assert(at<=requested);
        if(at==requested || n<64)return at;
    }
}
static void deadstorm_healthy_cycle(unsigned expected_coins) {
    const uint8_t status[]={0x10,0,0,0,0,1},inputs[]={0x10,0,0,0x10,0x80,1};
    uint8_t outputs[96]={0};
    host_out(status,6);
    assert(host_bulk_in(256)==256 && received[0x21]==0x80 && received[0x22]==2);
    host_out(inputs,6);
    assert(host_bulk_in(384)==384 && (received[0x80]&0x80) && received[0x90]==expected_coins);
    command(0x90,0x1400,sizeof(outputs));host_out(outputs,64);host_out(outputs+64,32);
    assert(host_bulk_in(0)==0);
    advance_usb_ms(3);assert(!endpoint(0x82)->pending && usio_usb_idle());
}
static void deadstorm_callback_race_recovery(void) {
    // Captured on a System 357 with the original Deadstorm executable:
    // WRITE 0x0028/2 + ACK, then status 256 and input 384 headers in the
    // same millisecond. The stale status callback overwrote the context
    // before either header completed: the host now owns TWO 384-byte INs.
    configure(USIO_DEADSTORM);
    usio_player_t players[2]={0};players[0].coin=players[0].test=true;
    usio_protocol_input(&protocol,players,1);
    for(unsigned i=0;i<3;i++)deadstorm_healthy_cycle(1);
    const uint8_t status[]={0x10,0,0,0,0,1},inputs[]={0x10,0,0,0x10,0x80,1},two[2]={0x73,0x81};
    command(0x90,0x0028,2);host_out(two,2);assert(host_bulk_in(0)==0);
    host_out(status,6);host_out(inputs,6);
    // IN#1 (384): only the input response; the abandoned status is dropped.
    assert(host_bulk_in(384)==384 && (received[0x80]&0x80) && received[0x90]==1);
    assert(received[0x21]!=0x80); // Not the 256-byte status block.
    // IN#2 stays pending on the host: the Pico must not feed it data.
    advance_usb_ms(5);assert(!endpoint(0x82)->pending);
    // Input callback 0x5d69f8 continues with WRITE 0x1400 (flags 1).
    uint8_t outputs[96]={0};
    command(0x90,0x1400,sizeof(outputs));host_out(outputs,64);host_out(outputs+64,32);
    assert(host_bulk_in(384)==0); // Stale IN#2: READ-data handler posts the semaphore.
    assert(host_bulk_in(0)==0);   // The write's own ACK IN, handler 0x5d5310.
    advance_usb_ms(3);
    assert(!endpoint(0x82)->pending && usio_usb_idle() && usio_usb_race_recoveries()==1);
    assert(!protocol.malformed_commands);
    // The very next frame is aligned again: status 256, input 384, ACK.
    players[0].coin=false;usio_protocol_input(&protocol,players,2);
    players[0].coin=true;usio_protocol_input(&protocol,players,3);
    for(unsigned i=0;i<3;i++)deadstorm_healthy_cycle(2);
    assert(usio_usb_race_recoveries()==1 && !protocol.malformed_commands);
    // A second race later in the session recovers the same way.
    host_out(status,6);host_out(inputs,6);
    assert(host_bulk_in(384)==384 && received[0x90]==2);
    command(0x90,0x1400,sizeof(outputs));host_out(outputs,64);host_out(outputs+64,32);
    assert(host_bulk_in(384)==0 && host_bulk_in(0)==0);
    deadstorm_healthy_cycle(2);
    assert(usio_usb_race_recoveries()==2);
    usio_usb_disconnect();
    // Bus reset clears a pending compensation instead of leaking a ZLP.
    configure(USIO_DEADSTORM);
    host_out(status,6);host_out(inputs,6);assert(host_bulk_in(384)==384);
    memset(endpoints,0,sizeof(endpoints));dcd_event_bus_reset(0,TUSB_SPEED_FULL,false);usio_usb_task();
    control_in(0x00,TUSB_REQ_SET_CONFIGURATION,1,0,received,sizeof(received));
    command(0x90,0x1400,sizeof(outputs));host_out(outputs,64);host_out(outputs+64,32);
    assert(host_bulk_in(0)==0);advance_usb_ms(5);assert(!endpoint(0x82)->pending);
    usio_usb_disconnect();
    puts("PASS Deadstorm stock callback race: abandoned status dropped, input answered once, stale IN and write ACK IN each retired by one ZLP, next frames aligned, repeatable, reset-safe.");
}
static void sdk_healthy_cycle(unsigned coins) {
    const uint8_t status[]={0x10,0,0,0,0,1},inputs[]={0x10,0,0,0x10,0x80,1};
    uint8_t outputs[96]={0};
    host_out(status,6);assert(host_bulk_in(256)==256 && received[0x21]==0x80 && received[0x22]==2);
    host_out(inputs,6);
    assert(host_bulk_in(384)==384 && (received[input_base()]&0x80) && received[input_base()+0x10]==coins);
    command(0x90,0x1400,sizeof(outputs));host_out(outputs,64);host_out(outputs+64,32);
    assert(host_bulk_in(0)==0);
    advance_usb_ms(3);assert(!endpoint(0x82)->pending);
}
static void all_sdk_titles_race_recovery(void) {
    const uint8_t status[]={0x10,0,0,0,0,1},inputs[]={0x10,0,0,0x10,0x80,1};
    for(unsigned profile=0;profile<USIO_PROFILE_COUNT;profile++) {
        if(profile==USIO_TAIKO)continue;
        configure((usio_profile_t)profile);
        usio_player_t p[2]={0};p[0].coin=p[0].test=true;usio_protocol_input(&protocol,p,1);
        for(unsigned i=0;i<2;i++)sdk_healthy_cycle(1);
        for(unsigned round=0;round<3;round++) {
            host_out(status,6);host_out(inputs,6); // Stale status callback race.
            // Tekken 6 answers immediately: at most one status packet precedes
            // the input reply inside the host's first 384-byte IN.
            assert(host_bulk_in(384)==384 && (received[input_base()]&0x80) && received[input_base()+0x10]==1);
            advance_usb_ms(5);assert(!endpoint(0x82)->pending);
            uint8_t outputs[96]={0};
            command(0x90,0x1400,sizeof(outputs));host_out(outputs,64);host_out(outputs+64,32);
            assert(host_bulk_in(384)==0 && host_bulk_in(0)==0);
            for(unsigned i=0;i<2;i++)sdk_healthy_cycle(1);
            assert(usio_usb_race_recoveries()==round+1 && !protocol.malformed_commands);
        }
        usio_usb_disconnect();
    }
    // Taiko keeps its old transport: no race handling.
    configure(USIO_TAIKO);host_out(status,6);host_out(inputs,6);
    assert(usio_usb_race_recoveries()==0);usio_usb_disconnect();
    puts("PASS stock callback race recovered on every non-Taiko title (Tekken 6 immediate path included), three times per session, with aligned frames afterwards; Taiko unchanged.");
}

static void queued_payload_capacity_and_reset(void) {
    const uint8_t status[]={0x10,0,0,0,0,1};
    const uint8_t inputs[]={0x10,0,0,0x10,0x80,1};
    const uint8_t write[]={0x92,0xf0,200,0,70,0};
    uint8_t payload[70];memset(payload,0x5a,sizeof(payload));
    // A six-byte payload tail deliberately looks like a READ command.
    memcpy(payload+64,status,sizeof(status));
    configure(USIO_DEADSTORM);
    host_out(status,6);host_out(write,6);host_out(payload,64);host_out(payload+64,6);host_out(inputs,6);
    size_t at=0;while(at<256)at+=host_in(0x82,received+at,sizeof(received)-at);
    assert(!memcmp(memory+200,payload,sizeof(payload)));
    assert(host_in(0x82,received,sizeof(received))==0); // WRITE ACK before the queued READ response.
    at=0;while(at<384)at+=host_in(0x82,received+at,sizeof(received)-at);
    assert(at==384 && protocol.commands==3 && !protocol.malformed_commands);
    assert(!endpoint(0x82)->pending && endpoint(1)->pending);
    usio_usb_disconnect();

    // FIFO capacity. A status READ followed by READ 0x1000 is the callback
    // race (dedicated tests), so fill the queue behind an input READ.
    configure(USIO_TEKKEN);host_out(inputs,6);
    unsigned accepted=0;
    while(endpoint(1)->pending && accepted<64) {host_out(inputs,6);accepted++;}
    assert(accepted>=2 && accepted<64 && !endpoint(1)->pending);
    assert(!protocol.malformed_commands && !usio_usb_idle() && !usio_usb_race_recoveries());
    at=0;while(at<384)at+=host_in(0x82,received+at,sizeof(received)-at);
    assert(endpoint(1)->pending); // Space is returned as requests execute.
    for(unsigned i=0;i<accepted;i++) {
        at=0;while(at<384)at+=host_in(0x82,received+at,sizeof(received)-at);
        assert(at==384);
    }
    assert(protocol.commands==accepted+1 && !protocol.malformed_commands && usio_usb_idle());
    usio_usb_disconnect();

    configure(USIO_DEADSTORM);
    usio_player_t players[2]={0};players[0].coin=true;
    usio_protocol_input(&protocol,players,1);
    host_out(status,6);host_out(inputs,6);
    memset(endpoints,0,sizeof(endpoints));dcd_event_bus_reset(0,TUSB_SPEED_FULL,false);usio_usb_task();
    control_in(0x00,TUSB_REQ_SET_CONFIGURATION,1,0,received,sizeof(received));
    assert(!endpoint(0x82)->pending && protocol.coins[0]==1);
    read_register(0,0x1000,384);assert(received[0x90]==1);
    assert(!protocol.malformed_commands);
    usio_usb_disconnect();
    puts("PASS queued WRITE payload/ACK order, command-like payload tail, full-FIFO backpressure/drain, and reset discarding stale requests while preserving Coin.");
}

static void deadstorm_native_sequence(void) {
    // Driver in the supplied IMG: read wrapper 0x5d58d8 -> command-complete
    // 0x5d5628 -> exact-size BulkTransfer -> callback 0x5d5480.
    // The read path has no trailing drain; the WRITE payload callback
    // 0x5d5598 submits a zero-length IN, then 0x5d5310 completes the write.
    configure(USIO_DEADSTORM);
    // Actual RPCS3 sys_usbd trace: game queries 0300, then header/full
    // descriptor for indices 1, 2 AND 3, despite iSerialNumber == 0.
    for (unsigned index = 1; index <= 3; index++) {
        assert(control_in(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0300, 4, received, sizeof(received)) == 4);
        assert(control_in(0x80, TUSB_REQ_GET_DESCRIPTOR, (uint16_t)(0x0300 | index), 2, received, sizeof(received)) == 2);
        unsigned length = received[0]; assert(length >= 2 && received[1] == TUSB_DESC_STRING);
        assert(control_in(0x80, TUSB_REQ_GET_DESCRIPTOR, (uint16_t)(0x0300 | index), (uint16_t)length, received, sizeof(received)) == length);
        assert(!endpoint(0)->stalled && !endpoint(0x80)->stalled);
        if (index == 3) assert(length == 2); // Empty optional serial, no fabricated identifier.
    }
    puts("PASS Deadstorm actual descriptor trace: string indices 0/1/2/3, English langid, header/full queries, optional serial returns empty descriptor without STALL.");
    usio_player_t players[2] = {0};
    for (unsigned tick = 0; tick < 32; tick++) {
        // Communication thread reads 256 bytes at reg 0, not the 64-byte
        // poll used in the older host test. Firmware data determines 2 nodes.
        read_register(0, 0, 256);
        assert(received[0x20] == 0 && received[0x21] == 0x80 && received[0x22] == 2);
        if (tick == 0) read_register(0, 0x1800, 384);
        uint8_t outputs[96] = {0};
        command(0x90, 0x1400, sizeof(outputs));
        host_out(outputs, 64); host_out(outputs + 64, 32);
        advance_usb_ms(tick==0?200:1);assert((usio_usb_state() & 32) != 0);
        assert(host_in(0x82, received, sizeof(received)) == 0); // Write-payload callback 0x5d5598 explicitly drains IN.
        assert(!endpoint(0x82)->pending);
        assert((usio_usb_state() & 32) == 0);
        if (tick == 0) {
            command(0x90, 0x18d4, 0);
            assert(host_in(0x82, received, sizeof(received)) == 0);
            assert(!endpoint(0x82)->pending);
        }
        usio_controls_from_gpio((tick % 2 == 0 ? 1u << PIN_COIN : 0) |
                                (tick == 0 ? 1u << PIN_TEST : 0) |
                                (tick == 1 ? (1u << PIN_SERVICE) | (1u << PIN_START) : 0), true, players);
        usio_protocol_input(&protocol, players, tick + 1);
        read_register(0, 0x1000, 384);
        assert(received[input_base()+0x10] == tick / 2 + 1 && received[input_base()+0x11] == 0);
        assert((input_word() & 0x80) != 0);
        if (tick == 1) assert((input_word() & 0x4200) == 0x4200); // Service + menu Enter.
        assert(!endpoint(0x82)->pending);
    }
    usio_usb_disconnect();
    puts("PASS Deadstorm IMG native exact-size sequence: 32 repeated keepalive/output/input cycles, independent Coin edges, Test, Service and Start; exact-size reads and explicit post-write IN acknowledgements.");
}

static void tekken_native_sequence(void) {
    for(unsigned which=0;which<2;which++) {
        configure(which?USIO_TEKKEN6BR:USIO_H050);
        usio_player_t players[2]={0};
        for(unsigned cycle=0;cycle<1000;cycle++) {
            players[0].coin=cycle%2==0;
            players[0].up=cycle%2==0;
            players[0].button[0]=cycle%2!=0;
            players[0].test=cycle==0;
            usio_protocol_input(&protocol,players,cycle+1);
            // Native read callback finishes here: no unsolicited/drain ZLP.
            read_register(0,0,256);assert(received[0x20]==0 && received[0x21]==0x80);
            read_register(0,0x1000,384);
            uint64_t expected=0x80 | (cycle%2==0?0x2000:0x200);
            assert(input_word()==expected);
            unsigned credits=cycle/2+1;
            assert((unsigned)(received[input_base()+0x10] | received[input_base()+0x11]<<8)==credits);
            uint8_t outputs[96]={0};command(0x90,0x1400,sizeof(outputs));
            host_out(outputs,64);host_out(outputs+64,32);
            // Write payload callback explicitly requests a zero-length IN ACK.
            assert(host_in(0x82,received,sizeof(received))==0);
            assert(!endpoint(0x82)->pending && protocol.malformed_commands==0);
        }
        usio_usb_disconnect();
    }
    puts("PASS Tekken6/BR native USB: 2,000 status/input/output cycles without unsolicited/read-drain ZLPs; explicit write ACK retained; latest button/coin state in the next read.");
}

int main(void) {
    original_driver_contracts();
    deadstorm_transaction_timing();
    deadstorm_callback_race_recovery();
    all_sdk_titles_race_recovery();
    queued_payload_capacity_and_reset();
    tekken_native_sequence();
    deadstorm_native_sequence();
    configure(USIO_TEKKEN);
    read_register(0, 0x1800, 384); assert(!memcmp(received, "NBGI.;USIO01;Ver1.00;", 20));
    read_register(0, 0x1080, 96); read_register(0, 0x1000, 384); assert(received[2] == 0x10);
    read_register(0, 0x4321, 0); read_register(0, 0x1800, 65);
    for (unsigned i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i ^ i / 256);
    command(0x92, 1, 65535);
    for (unsigned at = 0; at < sizeof(pattern);) {
        size_t n = sizeof(pattern) - at; if (n > 64) n = 64;
        host_out(pattern + at, n); at += (unsigned)n;
    }
    assert(host_in(0x82, received, sizeof(received)) == 0); // Post-write PS3 drain.
    read_register(2, 1, 65535); assert(!memcmp(received, pattern, sizeof(pattern)));
    // Backpressure must NAK subsequent OUTs while leaving USB control responsive.
    command(0x92, 100, 2); storage_ready = false; host_out(pattern, 2);
    assert(!endpoint(1)->pending); assert(host_in(0x82, received, sizeof(received)) == 0);
    assert(control_in(0x80, TUSB_REQ_GET_DESCRIPTOR, 0x0100, 18, received, sizeof(received)) == 18);
    storage_ready = true; usio_usb_task(); assert(endpoint(1)->pending);
    usio_player_t players[2] = {0}; players[0].coin = true; usio_protocol_input(&protocol, players, 1);
    // A bus reset halfway through a write discards the partial write, not bookkeeping/credits.
    uint8_t before_reset[100]; memcpy(before_reset, memory + 200, sizeof(before_reset));
    command(0x92, 200, 100); host_out(pattern, 64);
    memset(endpoints, 0, sizeof(endpoints)); dcd_event_bus_reset(0, TUSB_SPEED_FULL, false); usio_usb_task();
    assert(!protocol.writing && protocol.coins[0] == 1 && !memcmp(memory + 200, before_reset, sizeof(before_reset)));
    control_in(0x00, TUSB_REQ_SET_CONFIGURATION, 1, 0, received, sizeof(received));
    assert(!endpoint(0x82)->pending); read_register(0, 0x1000, 384);
    assert(received[input_base()+0x10] == 1);
    usio_usb_disconnect();
    for(unsigned p=0;p<USIO_PROFILE_COUNT;p++) {
        configure((usio_profile_t)p);
        read_register(0,0,64);
        assert((received[0x14] | received[0x15]<<8)==usio_profile_version((usio_profile_t)p));
        // Exercise the same physical binding used by main.c, through the real
        // TinyUSB endpoint code, not just by inspecting an encoder's buffer.
        uint32_t pins = (1u << PIN_COIN) | (1u << PIN_TEST) | (1u << PIN_SERVICE) |
                        (1u << PIN_START) | (1u << PIN_UP);
        usio_controls_from_gpio(pins, true, players);
        usio_protocol_input(&protocol, players, 1); usio_protocol_input(&protocol, players, 2);
        read_register(0, 0x1080, 96);
        assert((received[0] | received[1] << 8) == 0x6280 && received[16] == 1 && received[17] == 0);
        read_register(0, 0x1000, 384);
        usio_layout_t layout = usio_profile_layout((usio_profile_t)p);
        uint64_t expected = layout == USIO_LAYOUT_USJ ? 0xe080 : layout == USIO_LAYOUT_SHOOTER ? 0x6280 : 0xa04080;
        assert(input_word() == expected && received[input_base()+0x10] == 1 && received[input_base()+0x11] == 0);
        usio_controls_from_gpio(pins, false, players); usio_protocol_input(&protocol, players, 3);
        read_register(0, 0x1000, 384);
        assert(input_word() == 0x80 && received[input_base()+0x10] == 1); // Latched Test survives releasing inputs.
        usio_usb_disconnect();
    }
    puts("PASS real TinyUSB stack: enumeration, descriptors, configuration, fragmented commands, 65,535-byte bulk transfers, drain ZLPs, backpressure/control access, bus-reset recovery, profile reconnect.");
    puts("PASS real USB bulk packets from shared Coin/Test/Service/Enter/Up GPIOs on all 11 profiles, including shooter Start and selector suppression.");
    return 0;
}
