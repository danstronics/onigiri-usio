# Per-game USIO notes

| Game | PID | Board string | Version (`0x0000`+0x14) | Node | Switch layout | Transport | Status |
|---|---|---|---|---|---|---|---|
| Taiko no Tatsujin | `0910` | USIO PCB rev00 | `007E` | Taiko block `0x1080` | Taiko | drain ZLPs | transport unchanged from iTaiko |
| Tekken 6 (GAME 109/113) | `0900` | H050 USJ(C) PCB rev00 | `2D84` | 2 (`0x100`) | USJ | immediate | **verified on hardware** |
| Tekken 6 BR (GAME 208) | `0900` | H050 USJ(C) PCB rev00 | `2E85` | 2 (`0x100`) | USJ | SDK | untested |
| Tekken Tag Tournament 2 | `0910` | USIO PCB rev00 | `007E` | 2 (`0x100`) | Fighting | SDK | untested |
| Gundam Extreme VS Maxi Boost | `0910` | USIO PCB rev00 | `0203` | 2 (`0x100`) | Fighting | SDK | untested |
| Dragon Ball Zenkai Battle | `0910` | USIO PCB rev00 | `007E` | 2 (`0x100`) | Fighting | SDK | untested |
| Deadstorm Pirates SE | `0910` | USIO PCB rev00 | `007E` | 1 (`0x80`) | Shooter | SDK + 1 ms hold | **verified on hardware** |
| Razing Storm | `0910` | USIO PCB rev00 | `007E` | 1 (`0x80`) | Shooter | SDK | untested |
| Dark Escape 4D | `0910` | USIO PCB rev00 | `007E` | 1 (`0x80`) | Shooter | SDK | untested |
| Sailor Zombie AKB48 | `0910` | USIO PCB rev00 | `007E` | 1 (`0x80`) | Shooter | SDK | untested |

*Version*: `2D84`, `2E85` and `0203` come from the version checks in those
executables; `007E` is the value RPCS3 returns where no check was found.
*Node*: Deadstorm's node was found in its executable and confirmed on
hardware; Dark Escape's game loop reads node 1 directly; Razing Storm and
Sailor Zombie are assumed to match (same SDK and cabinet I/O).
*Transport*: see `Emulation-guide.md`. Every "SDK" title needs the race
recovery from `SDK-callback-race.md`.

## Switch layouts (player 1, node switch word)

Player 2 uses the same bits shifted left by 24 (fighting layouts).

### Fighting (RPCS3 "Tekken" mapping) - Tag 2, Gundam, Dragon Ball

| Input | Bit |
|---|---|
| Start | `0x800000` |
| Up / Down / Left / Right | `0x200000` / `0x100000` / `0x80000` / `0x40000` |
| Button 1 / 2 / 3 / 4 / 5 | `0x20000` / `0x10000` / `0x40000000` / `0x20000000` / `0x80000000` |

### USJ legacy - Tekken 6, Tekken 6 BR (H050 board)

| Input | Bit |
|---|---|
| Start | `0x8000` |
| Up / Down / Left / Right | `0x2000` / `0x1000` / `0x800` / `0x400` |
| LP / RP / LK / RK | `0x200` / `0x100` / `0x800000` / `0x400000` |

### Shooter - Deadstorm, Razing Storm, Dark Escape, Sailor Zombie

From Deadstorm's own button accessors (`0x324EAC` held, `0x324EC0`
pressed, `0x324ED4` released, `0x324FFC` DX):

| Input | Bit (player 1) | Player 2 |
|---|---|---|
| Gun trigger (either handle) | `0x800000` / `0x400000` | `0x100000` / `0x80000` |
| Start | `0x200000` | `0x40000` |
| Extra button | `0x20000` | `0x10000` (inferred) |
| Menu Enter | `0x200` | |
| Menu Up / Down | `0x2000` / `0x1000` | |
| **DX cabinet** (switch, not a button) | `0x100` | |

The gun position is analog: X on channel 0 (`node + 0x20`), Y on channel 1
(`node + 0x22`), player 2 on channels 2/3, 16-bit, center `0x8000`.
Deadstorm reads channels 0..7 at `0x325394` and the wheel encoder at
`node + 0x30`.

**Deadstorm DX:** the game reads bit `0x100` in ten places to decide the
cabinet type. Treat it as a **switch** that stays on, not as a button.

### Common bits

`0x80` Test (latched: one press on, next press off), `0x4000` Service,
coin counter at `node + 0x10`.

## Game-specific transport notes

* **Deadstorm**: the confirmed configuration holds every first reply packet
  and every write ACK for 1 ms. The other SDK titles hold only the status
  reply, which is enough for the race detection.
* **Tekken 6**: answers immediately, without the OUT queue. Hardware tests
  showed it works that way; the race recovery only engages when the second
  header arrives.
* **All SDK titles** query string descriptor 3 and need an empty reply.
