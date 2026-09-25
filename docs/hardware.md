# Board pin map (Waveshare ESP32-S3-AUDIO-Board v1.1)

Source: schematic v1.1 GPIO table + Waveshare demo package (`mp3_play_03`, `factory_01`). Both agree. Files are in `reference/` (gitignored; re-download from the Waveshare resources page).

| Function | Pin |
|---|---|
| I2C SDA / SCL (shared: ES8311, ES7210, TCA9555, PCF85063 RTC) | GPIO11 / GPIO10 |
| I2S MCLK / SCLK (BCLK) / LRCK / DIN (mic) / DOUT (speaker) | GPIO12 / 13 / 14 / 15 / 16 (one I2S port shared by ES7210 and ES8311) |
| RGB LEDs: 7x WS2812B, data | GPIO38 |
| microSD (SDMMC 1-bit) CLK / CMD / D0 | GPIO40 / 42 / 41 |
| TCA9555 I2C address | 0x20 (ADDRESS_000) |
| ES8311 / ES7210 | esp_codec_dev default addresses |

## TCA9555 expander (pins numbered 0-15)

| Pin | Use |
|---|---|
| 8 | **PA_CTRL** speaker amp enable (output, high = on; NS4150B) |
| 9 / 10 / 11 | **Key1 / Key2 / Key3** user buttons (inputs, active low, 10k pull-ups) |
| 2 | RTC interrupt (input) |
| 0, 1, 5, 6 | LCD reset, touch reset, camera select, USB/UART mux (outputs; unused by BookBook) |

BOOT (GPIO0) and RESET are separate buttons.

## Notes

- Buttons are only reachable over I2C via the expander, so they are **polled** (10 ms). Plenty fast, and the board is mains powered.
- ES7210 is a 4-channel ADC; the demo opens it as 2 channels x 32 bit at 16 kHz. Two digital mics are populated (MIC1/MIC2).
- Demo defaults: playback volume 60, record gain 30 dB.
- Expander pin 6 muxes USB vs. camera UART pins; leave it low so USB serial/JTAG stays usable.
- LED colour order (GRB vs RGB) is unverified: the demo config and comment disagree. Phase 0 flashes red, green, blue to check.

## XIAO ESP32-S3 stand-in wiring (verified)

Used until the Waveshare board is in hand. Selected with `CONFIG_BOOKBOOK_BOARD_XIAO_ESP32S3` in the git-ignored `secrets/sdkconfig.secrets`. The Waveshare board stays the committed default.

| Function | XIAO pin | GPIO |
|---|---|---|
| WS2812 LED string (12) | D1 | 2 |
| Key1 (BOOT button, active low) | - | 0 |
| MAX98357A BCLK / LRC / DIN / SD_MODE | D8 / D7 / D9 / D10 | 7 / 44 / 8 / 9 |
| PDM microphone CLK | D0 | 1 |
| PDM microphone DAT | D2 | 3 |
| PDM microphone SEL | GND (left slot) | - |
| PDM microphone power | 3V3 / GND | - |

Notes:
- **PDM receive only works on I2S0** on the ESP32-S3 (the driver rejects I2S1), so the microphone is created on I2S0 by number and the speaker is pinned to I2S1.
- The PDM clock runs at 2.048 MHz (16 kHz x 128), inside a typical PDM mic's 1-3.25 MHz range.
- Microphone software gain is `CONFIG_BOOKBOOK_MIC_GAIN` (default 4x); SEL tied to 3V3 instead needs `CONFIG_BOOKBOOK_MIC_RIGHT_SLOT=y`.
- BOOT is GPIO0, a strapping pin: never hold it while powering up or resetting (chip enters the ROM downloader). Setup mode is a 5 s hold while running.
- GPIO3 (mic DAT) is also a strapping pin (JTAG source select); no effect in normal use.
- Test from the setup page: **Test microphone** records 4 s, plays it back, transcribes it with Azure and says what it heard.
