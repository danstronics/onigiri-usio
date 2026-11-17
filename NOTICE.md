# Sources and licensing

Onigiri USIO is based on:

* [ITAIKO firmware](https://github.com/itaiko-project/ITAIKO-firmware)
  (derived from DonCon2040), MIT. The original license is kept as `LICENSE`.
  The Onigiri pin map and the Taiko USB transport behavior come from it.
* [RPCS3](https://github.com/RPCS3/rpcs3), revision
  `13a3cd4266158911a8c555dda59005bc20fd2a66`, GPL-2.0-only. USIO register
  layouts, identification responses, input masks and the card reader framing
  are adapted from `Emu/Io/usio.cpp`, `usio_types.h` and `nfc_reader.cpp`.
* [pico_ssd1306](https://github.com/daschr/pico-ssd1306) by David Schramm,
  MIT (notice in `libs/pico_ssd1306/LICENSE`), with bounded I2C writes added.
* Aileron font, SIL OFL 1.1 - see `NOTICE-fonts.md`.

The combined firmware and the new code are distributed under GPL-2.0-only
(`COPYING`). Raspberry Pi Pico SDK and TinyUSB keep their own licenses
(`ThirdPartyNotices.txt`).

The game behavior documented in `docs/` was established by interoperability
analysis of legitimately owned game software. No game code is distributed.
