# BookBook

A stand-alone, single-button version of [Bookworm](../Bookworm) running on a Waveshare ESP32-S3-AUDIO-Board. Press the button, talk to your Vision Australia Library account, hear the answer.

Status: Phase 0 (bring-up) builds. See [docs/decisions.md](docs/decisions.md) for architecture and plan, [docs/hardware.md](docs/hardware.md) for the pin map.

## Build

ESP-IDF 5.5 (C++). `tools\idf.ps1` runs `idf.py` with this project's IDF 5.5 whatever the shell has activated (edit the two paths at its top if the install moves), for example `.\tools\idf.ps1 build` or `.\tools\idf.ps1 -p COM5 flash monitor`. Or, from a PowerShell prompt with IDF 5.5 activated:

    idf.py set-target esp32s3
    idf.py build
    idf.py -p COMx flash monitor

Secrets and per-board defaults live in `secrets/sdkconfig.secrets` (gitignored; see [docs/setup.md](docs/setup.md#what-is-editable-and-what-is-baked-in)). For example, Wi-Fi

    CONFIG_BOOKBOOK_WIFI_SSID="..."
    CONFIG_BOOKBOOK_WIFI_PASSWORD="..."

The setup page then edits only the Wi-Fi, library login and volume; the keys are baked in at build time.
