#pragma once
#include <string>
#include "config.h"
#include "esp_err.h"

// Over-the-air updates, only ever on the member's spoken request (see brain.cpp). The firmware is published
// with tools/publish_firmware.py to the same storage container as memory.json, as two blobs:
//   bookbook.bin   the app image
//   bookbook.json  {"sha256": <ELF sha256 of the image>, "version", "built", "size"}, uploaded last
// A check reads only the small manifest and compares its sha256 with the running image. An install is
// queued by the brain (request_install) and run by the main loop once the reply has been spoken, so the
// download does not run inside the turn's time limit. A new image boots "pending verify" and is rolled
// back by the bootloader unless mark_valid() is called (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
namespace ota {

struct Info {
    bool available = false;  // the published image differs from the running one
    std::string running_version, running_built;
    std::string new_version, new_built;
    std::string new_sha256;
};

// Reads the manifest. ESP_ERR_INVALID_STATE when no storage container is configured or Wi-Fi is down.
esp_err_t check(const Config& cfg, Info& out);

// Marks that the member asked for an install; consumed by the main loop.
void request_install();
void cancel_install();
bool install_requested();

// Downloads and installs the published image, then restarts (does not return on success). On failure
// returns the error, with a short reason in `why`, and the running image is untouched.
esp_err_t install(const Config& cfg, std::string* why);

// What happened to the last install, known only after the restart: the installer notes the hash it expects
// before restarting, and the next boot compares it with what is actually running. Reported once (the note is
// erased): Updated when the new image is running, RolledBack when the bootloader went back to the old one.
enum class BootReport { None, Updated, RolledBack };
BootReport take_boot_report();

// Tells the bootloader the running image is good, so it will not roll back. Safe to call repeatedly.
void mark_valid();

}  // namespace ota
