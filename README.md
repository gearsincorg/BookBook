# BookBook

A stand-alone, single-touch version of [Bookworm](../Bookworm): touch and hold the pad, talk to your Vision Australia Library account, hear the answer. It is a spoken librarian ("Marian") for a vision-impaired member: search the catalogue, manage the bookshelf, request list, subscriptions and reading lists, all by voice.

Status: working, on Phil's VA board (a Seeed XIAO ESP32-S3 with a PDM microphone, MAX98357A amp, WS2812 ring and a touch pad).

## Documentation

- [docs/setup.md](docs/setup.md): setting the device up, what the lights mean, over-the-air updates.
- [docs/hardware.md](docs/hardware.md): the board's wiring, microphone and touch-pad notes.
- [docs/conversation-rules.md](docs/conversation-rules.md): how she talks and behaves (generated from the prompt in `main/brain.cpp` by `tools/rules_doc.py`).
- [docs/memory.md](docs/memory.md): the authors list, books list and On Hold list.
- [docs/va-endpoints.md](docs/va-endpoints.md): the Vision Australia portal calls, extending Bookworm's notes.
- [docs/decisions.md](docs/decisions.md): the original architecture and plan (a historical document: where it disagrees with the code, the code and the other docs win).

## Build

ESP-IDF 5.5 (C++). `tools\idf.ps1` runs `idf.py` with this project's IDF 5.5 whatever the shell has activated (edit the two paths at its top if the install moves), for example `.\tools\idf.ps1 build` or `.\tools\idf.ps1 -p COM5 flash monitor`. Or, from a PowerShell prompt with IDF 5.5 activated:

    idf.py set-target esp32s3
    idf.py build
    idf.py -p COMx flash monitor

Secrets and per-board defaults live in `secrets/sdkconfig.secrets` (gitignored; see [docs/setup.md](docs/setup.md#what-is-editable-and-what-is-baked-in)). For example, Wi-Fi

    CONFIG_BOOKBOOK_WIFI_SSID="..."
    CONFIG_BOOKBOOK_WIFI_PASSWORD="..."

The setup page then edits only the Wi-Fi, library login and volume; the keys are baked in at build time.

## Releasing

`tools\release.ps1` cuts a numbered release (tag, build, push, GitHub release, publish the firmware); `python tools/publish_firmware.py` alone uploads the current build so a unit can pick it up. See [docs/setup.md](docs/setup.md#updating-the-firmware-over-the-air).
