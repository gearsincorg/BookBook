# Phil's VA board: wiring and hardware notes

A Seeed XIAO ESP32-S3 module (8 MB flash, PSRAM) on the PhilbotSays PCB, with a PDM microphone, a MAX98357A speaker amp, a ring of WS2812 LEDs and a capacitive touch pad as the one button. Wiring (verified):

| Function | XIAO pin | GPIO |
|---|---|---|
| WS2812 LED string (12) | D1 | 2 |
| Button: capacitive touch pad (same as PhilbotSays) | D5 | 6 |
| BOOT button, optional bench fallback (`CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON`) | - | 0 |
| MAX98357A BCLK / LRC / DIN / SD_MODE | D8 / D7 / D9 / D10 | 7 / 44 / 8 / 9 |
| PDM microphone CLK | D0 | 1 |
| PDM microphone DAT | D2 | 3 |
| PDM microphone SEL | GND (left slot) | - |
| PDM microphone power | 3V3 / GND | - |

The pins and LED count are build settings (`main/Kconfig.projbuild`), defaulting to the values above.

Notes:
- **PDM receive only works on I2S0** on the ESP32-S3 (the driver rejects I2S1), so the microphone is created on I2S0 by number and the speaker is pinned to I2S1.
- The PDM clock runs at 2.048 MHz (16 kHz x 128), inside a typical PDM mic's 1-3.25 MHz range.
- Microphone gain is **adaptive** (`CONFIG_BOOKBOOK_MIC_AUTO_GAIN`, on by default): after each recording its loud parts (99.5th percentile, so a click does not count) are scaled to `CONFIG_BOOKBOOK_MIC_TARGET_PEAK` (12000 of 32767), with a multiplier between 1 and `CONFIG_BOOKBOOK_MIC_MAX_GAIN` (20); the gain used is logged with each recording. With it off, the fixed `CONFIG_BOOKBOOK_MIC_GAIN` is used (default 4x; the enclosure grill over the microphone lowers the level, and 3x was used before it because 4x clipped close-range speech on the bare board); SEL tied to 3V3 instead needs `CONFIG_BOOKBOOK_MIC_RIGHT_SLOT=y`.
- **Touch button** (`touch.cpp`, ported from PhilbotSays' `touch_sense.c`): calibrates on the untouched pad at power-up (10 samples x 10 ms), a touch is a raw reading more than 20% above baseline (released below 15%; PhilbotSays uses 30%/25%, lowered here for a lighter touch, which is close to the ~15% idle noise PhilbotSays measured, so raise it if the pad free-runs or sticks on), polled every 20 ms with a two-poll debounce, and the baseline resets to the average of any 10 consecutive readings below it (recovers from a pad held or wet at power-up). Keep your hand off the pad while the board powers up. `CONFIG_BOOKBOOK_TOUCH_ENABLED` turns the sensor off entirely (the BOOT button then becomes the button).
- BOOT is GPIO0, a strapping pin: never hold it while powering up or resetting (chip enters the ROM downloader). It is not used as a button unless `CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON` is on.
- GPIO3 (mic DAT) is also a strapping pin (JTAG source select); no effect in normal use.
- The board is mains powered (USB), so Wi-Fi power save is off and buttons and sensors are simply polled.
- Test from the setup page: **Test microphone** records 4 s, plays it back, transcribes it with Azure and says what it heard.

## Flash layout

`partitions.csv`: NVS, OTA data, PHY init, and two 3.94 MB (0x3F0000) app slots (`ota_0`, `ota_1`) for over-the-air updates, which together use all the rest of the 8 MB flash. There is no other data partition. Changing the partition table needs a USB flash: an over-the-air update only replaces the app.
