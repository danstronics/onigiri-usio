# Emulating a USIO

A practical checklist for building a USIO replacement on a microcontroller
(this project uses an RP2040 with TinyUSB). Read `USIO-protocol.md` first.

## 1. Enumerate like the board

* VID `0x0B9A`, PID `0x0910` (`0x0900` for Tekken 6 / BR), bcdDevice = PID,
  USB 1.1 full speed, EP0 8 bytes.
* One interface with exactly EP `0x01` bulk OUT 64, EP `0x82` bulk IN 64,
  EP `0x83` interrupt IN 8 / 16 ms.
* Strings: 1 `NAMCO`, 2 the board name; **answer index 3 with an empty
  string descriptor** instead of stalling.
* Report the firmware version the game expects at status offset `0x14`
  (`Games.md`).

## 2. Pick the transport for the game

| Driver | Reads | Writes | Extras |
|---|---|---|---|
| Namco SDK (every non-Taiko game) | exact length, **no** trailing ZLP | ZLP ACK after the payload | race recovery, OUT queue |
| Taiko | ZLP after any reply that is a multiple of 64 bytes | ZLP ACK | one ZLP when the interface opens |

A drain ZLP that the SDK does not expect is consumed by its *next* IN
transfer and shifts every later reply by one: the classic "works for a
while, then NOT CONNECT".

## 3. Keep the OUT side flowing

The host may send the next header while an IN reply is still in progress
(race case). Accept OUT packets into a small FIFO (16 x 64 bytes in this
firmware), process them strictly in order, and only NAK when the FIFO is
full. Never drop a packet and never treat a 6-byte payload tail as a header:
track whether you are inside a write payload.

## 4. Handle the SDK callback race

Implement the recovery in `SDK-callback-race.md`. Without it every SDK game
eventually loses sync. Hold the status reply (READ `0x0000`) for about 1 ms
so the race always shows up before any status byte has left.

## 5. Inputs

* Sample buttons at least every millisecond and debounce (8 ms here).
* Coin is a **counter**: increment once per press.
* Test is **latched** by the board in RPCS3 and in this firmware: one press
  on, next press off.
* Latch short events (Taiko hits) until the next poll.
* Put the player in the node the game reads (`Games.md`) and leave the
  other node at zero: stray bits there can register as a second player.
* Guns: 16-bit analog, center `0x8000`. Without a real gun, a digital
  direction can drive it. This firmware moves 192 units per 8 ms at first and
  accelerates to 1536 after about 0.4 s held.

## 6. Backup memory

Implement channels 2..17 as 1 MiB of non-volatile memory. On a 2 MiB RP2040
this firmware keeps a 128 KiB write-back cache in RAM and commits one 4 KiB
sector at a time to flash between USB transfers (about 0.75 s after the last
write). That keeps flash erases from stalling the bus during a frame and
still saves settings within seconds. Clear the memory on WRITE `0x000A` =
`0x6666` or command `0xA0` on register `0x000A`.

## 7. Card readers

Registers `0x0080/0x0090` (status), `0x7400/0x7C00` (request) and
`0x7000/0x7800` (reply) carry PN5xx-style NFC frames. Answering every poll
with "no card" is enough for the games to run (`firmware/card_reader.c`).

## 8. Detecting a hung game

A running game polls the board every frame (about 60 Hz). If no command
arrives for 2 seconds, the game has crashed or stopped its I/O thread. This
firmware freezes and blinks its uptime clock at that moment, which tells a
real crash apart from a slow loading screen.

## Testing without a cabinet

`tests/usio/usb_transport_test.c` runs the real TinyUSB device stack against
a simulated controller and a model of the PS3 host. A bulk IN transfer ends
at full length or on a short packet. The model replays the race exactly as
it was captured on hardware, and it is the fastest way to check transport
changes.
