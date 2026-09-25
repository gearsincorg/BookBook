# BookBook

A stand-alone, single-button version of [Bookworm](../Bookworm) running on a Waveshare ESP32-S3-AUDIO-Board. Press the button, talk to your Vision Australia Library account, hear the answer.

Status: Phase 0 (bring-up) builds. See [docs/decisions.md](docs/decisions.md) for architecture and plan, [docs/hardware.md](docs/hardware.md) for the pin map.

## Build

ESP-IDF 5.5 (C++). From a PowerShell prompt with IDF activated:

    idf.py set-target esp32s3
    idf.py build
    idf.py -p COMx flash monitor

Wi-Fi for development: create `secrets/sdkconfig.secrets` (gitignored) containing

    CONFIG_BOOKBOOK_WIFI_SSID="..."
    CONFIG_BOOKBOOK_WIFI_PASSWORD="..."

This is temporary; the web configuration portal replaces it.
