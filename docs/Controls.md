# Controls and display (Onigiri controller)

GPIO numbers are RP2040 GPIOs (ITAIKO-firmware pin map). Buttons connect
the GPIO to GND; internal pull-ups and an 8 ms debounce are used.

## Top row - system functions

| Button | GPIO | Function |
|---|---|---|
| L | 29 | Deadstorm: DX cabinet on/off (remembered across power cycles). Nothing in other games |
| - | 28 | Service |
| Share | 15 | Coin (one credit per press) |
| Home | 4 | Test on/off |
| + | 3 | Enter / Start in menus |
| R | 0 | Button 5 in fighting games |

Hold **+ and -** for 2 seconds for **Game Select**: Up/Down to choose,
+ to confirm, - to cancel. The game choice changes the USB identity; it is
saved in flash. The list is ordered Tekken (by release), Dragon Ball,
Gundam, the shooters, Taiko; titles too long for a row scroll in a loop.
The illustrated user manual is in `manual/`.

## D-pad and action buttons - gameplay

| Input | GPIO | Fighting games | Shooters |
|---|---|---|---|
| D-pad Up / Down | 27 / 7 | stick | aim (and menu Up/Down) |
| D-pad Left / Right | 26 / 14 | stick | aim |
| West | 5 | button 1 | game Start |
| North | 1 | button 2 | extra button |
| South | 2 | button 3 | fire |
| East | 6 | button 4 | fire (second trigger) |

Taiko: West = left Ka, North = left Don, South = right Don, East = right Ka.

## Display (SSD1306 128x64, I2C0 SDA 8 / SCL 9)

* White **bar** at the top with `onigiri-usio`, plus `TEST` while test mode
  is on, and below it the **HOLD + AND - FOR GAME SELECT** hint.
* **Game name** in the large font, wrapped when needed; a white **DX** badge
  appears under Deadstorm Pirates when the DX cabinet switch is on.
* **uptime** at the bottom - time since power on. When the System stops
  issuing USB commands for 2 seconds (crash, hang), the clock stops at the
  last command and blinks. It resumes on its own if the commands come back.
* Game Select shows the list with a scroll bar on the right.

`tests/ui/ui_preview.c` renders every screen to PBM images on a PC.
