# The Namco USIO protocol

The USIO ("Universal Serial I/O") is the USB I/O board used by Bandai Namco
games on the PlayStation 3 based **System 357/369** arcade boards. It carries
coin, test/service, player switches, analog channels (guns, wheels),
card readers, lamp/output data and 1 MiB of battery-backed memory
(bookkeeping and settings).

Everything below was established from RPCS3's emulation, from static analysis
of the original game executables and from USB captures on a real System 357.
Where something is inferred rather than observed, it is marked as such.

## USB identity

| Field | Value |
|---|---|
| Vendor ID | `0x0B9A` (Namco) |
| Product ID | `0x0910` (USIO) - `0x0900` for the H050 "USJ" board used by Tekken 6 / Tekken 6 BR |
| bcdDevice | same as the product ID |
| USB version | 1.1, full speed, EP0 size 8 |
| Class | vendor specific (`0xFF/0x00/0xFF`) |
| Strings | 1 = `NAMCO`, 2 = board name (`USIO PCB rev00`, `H050 USJ(C) PCB rev00`), no serial |

One configuration, one interface, three endpoints:

| Endpoint | Type | Size | Use |
|---|---|---|---|
| `0x01` OUT | bulk | 64 | command headers and write payloads |
| `0x82` IN | bulk | 64 | read data and write acknowledgements |
| `0x83` IN | interrupt | 8 | status, interval 16 ms (the games do not depend on it) |

**String index 3.** The Namco SDK reads string descriptor 3 even though
`iSerialNumber` is 0. Answer with an empty string descriptor (`02 03`). A
STALL makes the SDK's descriptor reader fail and the game never opens the board.

## Commands

Every command starts with a 6-byte header on EP `0x01`:

```
byte 0     : operation (high nibble) | channel (low nibble)
byte 1     : check byte (writes only)
bytes 2..3 : register, little endian
bytes 4..5 : length in bytes, little endian
```

| Operation | Meaning |
|---|---|
| `0x10` | READ `length` bytes from `register` on `channel`; the data comes back on EP `0x82` |
| `0x90` | WRITE: `length` payload bytes follow on EP `0x01`. Check byte = `(~(register >> 8)) & 0xF0` |
| `0xA0` | reset/init; on channel 0 register `0x000A` it clears the backup memory |

The SDK sends the header as one 6-byte OUT transfer. Payloads and replies
are split into 64-byte packets.

### Completion rules (important)

* **READ**: send exactly `length` bytes. Namco's SDK games expect **no**
  zero-length packet after a reply whose length is a multiple of 64. Taiko's
  driver is different and expects one (see `Emulation-guide.md`).
* **WRITE**: after the last payload byte the host submits a zero-length IN
  transfer on EP `0x82`. The device must answer it with a **zero-length
  packet**. The write only completes on the host when that ACK arrives.
* Commands are strictly serialized by the host: one header, its data/ACK,
  then the next header. If two headers arrive before a READ was answered,
  the host's driver has hit its callback race (see `SDK-callback-race.md`).

## Channel 0 registers

| Register | Dir | Size | Content |
|---|---|---|---|
| `0x0000` | R | 0x100 | status block (below) |
| `0x0002`, `0x0028`, `0x0048`..`0x007A` | W | small | configuration writes; acknowledge and ignore |
| `0x0080` / `0x0090` | R | 16 | card reader 0 / 1 status; bytes 2..3 = pending reply length |
| `0x1000` | R | 0x80 + 0x80 * nodes | switch/analog input block (below) |
| `0x1080` | R | 0x60 | Taiko input block |
| `0x1100` | R | 0x60 | Taiko: copy of the last `0x1080` block |
| `0x1400` | W | 0x20 + 0x20 * nodes | outputs (lamps, motors); acknowledge |
| `0x1800` / `0x1880` | R | <= 0x180 | firmware information strings (`NBGI.;USIO01;Ver1.00;JPN,Multipurpose with PPG.`) |
| `0x7000` / `0x7800` | R | var | card reader 0 / 1: read reply bytes |
| `0x7400` / `0x7C00` | W | var | card reader 0 / 1: request (PN5xx-style NFC frames) |

### Status block, register `0x0000`

| Offset | Content |
|---|---|
| `0x00` | 16-bit running counter (milliseconds is fine) |
| `0x04` | `74 01 01` |
| `0x12` | `0x007E` |
| `0x14` | **board firmware version** - some games check it (see `Games.md`) |
| `0x20` | node type, 16-bit **`0x8000`** = "switch/analog I/O node present" |
| `0x22` | **number of I/O nodes** (the SDK uses at most 7) |
| `0x28`, `0x34` | `0x8000` |

The SDK reads this block periodically. If `0x20` is `0x8000` it reads
`0x80 + 0x80 * count` bytes of register `0x1000`, otherwise it falls back to
`0x80` bytes. Report 2 nodes: the SDK then reads 0x180 bytes.

### Input block, register `0x1000`

```
0x00   u16  "LM" summary word (Test 0x1000, Enter 0x0800, Up 0x0200, Down 0x0400,
            Left 0x2000, Right 0x4000, button 1 0x0100) - not read by the games we analysed
0x02   u8   DIP switches, 0x10
0x80   node 1
0x100  node 2
...    node N at 0x80 * N
```

Each node (all values little endian):

| Node offset | Content |
|---|---|
| `+0x00` | 64-bit switch word. Bit 7 (`0x80`) = Test, `0x4000` = Service. Player 2 bits are player 1 bits shifted left by 24 |
| `+0x10` | 16-bit coin counter (increments once per coin, never decreases) |
| `+0x20` | eight 16-bit analog channels (gun X/Y, wheel...) - center `0x8000` |
| `+0x30` | 8-bit rotary encoder / wheel position |

The SDK copies the switch bytes into its own state, most significant first:
SDK word = `raw[1]<<24 | raw[2]<<16 | raw[3]<<8 | raw[4]`. When you read
masks from a game's code, convert them back with that formula.

Which node a game reads is a property of the game, not of the board: the
Tekken titles read node 2 (`0x100`), the gun games node 1 (`0x80`). The
switch bit layout of each game family is in `Games.md`.

### Taiko input block, register `0x1080`

```
0x00  u16  Test 0x80, Enter 0x200, Down 0x1000, Up 0x2000, Service 0x4000
0x10  u16  coin counter
0x20  4 x u16  player 1 drum: left Ka, left Don, right Don, right Ka (0xFFFF = hit)
0x28  4 x u16  player 2 drum
```

Drum hits are pulses: latch them until the next poll so short hits are never lost.

## Backup memory (channels 2..17)

Channels 2 to 17 address sixteen 64 KiB pages = 1 MiB of non-volatile
memory. READ and WRITE work like on channel 0, with the register as the
offset inside the page. The games keep bookkeeping, settings and rankings
there; it must survive power cycles.
