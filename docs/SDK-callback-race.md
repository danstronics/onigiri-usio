# The SDK callback race ("NOT CONNECT IO-PCB")

## Symptom

With an emulated USIO, a game boots and plays normally, then after 30 seconds
to a few minutes stops reading inputs and shows **NOT CONNECT IO-PCB** (error
`8001002B` in the game's poll). The USB link stays up and the device keeps
answering. The bug is in the game's driver, not in the board (see below).
It has not been reported with original boards; why their timing avoids it
was not measured.

## Which games

Every title we examined except Taiko links the same Namco USIO driver, and every
one of them has the bug. The driver was found with a structural scan (semaphore post, then reload of
`ctx+0x24`, then indirect call) of the original executables:

| Game | Executable | Race sites (post call) |
|---|---|---|
| Deadstorm Pirates SE | `EBOOT.BIN` | `0x5D53C0`, `0x5D5530` |
| Tekken 6 | `GAME_109.SELF` | `0x5FD1FC`, `0x5FD3D4` |
| Tekken 6 Bloodline Rebellion | `GAME_208.SELF` | `0x4BE298`, `0x4BE408` |
| Tekken Tag Tournament 2 | `GAME0180.SELF` | `0x4B5664` and 9 more (-O0 build) |
| Gundam Extreme VS Maxi Boost | `GAME.SELF` 24.07 | `0x503098` and 9 more |
| Dragon Ball Zenkai Battle | `game.self` 26.01 | `0xC33CA0` and 9 more |
| Razing Storm | `EBOOT.BIN` | `0x507728`, `0x507898` |
| Dark Escape 4D | `EBOOT.BIN` | `0x5608CC` and 9 more (SNC build, `bdnzctrl`) |
| Sailor Zombie | `EBOOT.BIN` | `0x4C305C` and 9 more |

All of them share the same command chain:

1. status thread: acquire the context semaphore, READ `0x0000` (256 bytes),
   asynchronous (flags 2), callback = *status callback*;
2. status callback: if the node type is `0x8000`, READ `0x1000`
   (`0x80 + 0x80 * count` bytes), flags 3 (no acquire), callback = *input callback*;
3. input callback: WRITE `0x1400` (`0x20 + 0x20 * count` bytes), flags 1;
4. that write's ACK completion releases the semaphore.

## The bug

The driver keeps **one shared context** (buffer, length, flags, callback at
`+0x24`). A synchronous completion handler does:

```
store result
if (!(flags & 2)) semaphore_post()     // release the context
callback = ctx->callback               // reload AFTER the release
if (callback) callback(ctx)
```

Between the post and the reload another thread can take the semaphore and
start a new command. In practice the status thread wins it right after a
write finishes, sets `ctx->callback = status callback` and sends its READ
`0x0000` header. The old completion then reloads the *new* callback and
calls the status callback **before the status READ was even answered**. The
status callback uses the previous (still valid) status buffer and sends READ
`0x1000`.

Now two READ headers are on the bus: status (256) and input (384). Both
header completions run with the context already overwritten, so the host
queues **two IN transfers of 384 bytes** for what the device sees as two
different replies. The next write leaves one more zero-length ACK IN pending.
From then on every reply lands in the wrong transfer, the status buffer fills
with garbage, the SDK falls back to the 128-byte input read and the game
reports NOT CONNECT.

The semaphore is created with a maximum count of 1, so an extra post is
rejected by the kernel and does no harm. The harm comes only from the
unmatched IN transfers.

## Device-side recovery (no game patch)

The signature is unambiguous. In a healthy flow the next request is only
issued from the previous READ's completion, so a header never arrives
while a READ is still unanswered.

1. A READ `0x1000` header arrives while the status READ (`0x0000`) has not
   sent its reply yet: **drop the status reply** and answer only the input
   READ. If a 64-byte packet of the status reply was already handed to the
   controller, continue the input reply from that offset so the host's first
   IN still receives exactly 384 bytes.
2. The input callback then sends WRITE `0x1400`. Answer its ACK with **two
   zero-length packets**. The first closes the host's extra 384-byte IN (its
   handler posts the semaphore, exactly where the write would). The second
   closes the write's own ACK IN.

The stream is aligned again from the next frame. The firmware also holds the
first reply packet of the status READ for 1 ms, so the second header always
arrives before any status byte leaves. Deadstorm holds every reply and ACK
for 1 ms, which was verified on hardware.

With this recovery the unmodified Deadstorm Pirates executable ran without
the error on a System 357, where it previously failed within about a minute.
For the other titles the recovery is identical but has not yet been tested
on hardware. See `firmware/usio_usb.c`; the host-side model is exercised by
`tests/usio/usb_transport_test.c`.
