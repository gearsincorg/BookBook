#pragma once
#include <string>
#include "esp_err.h"

// Device settings, split by who owns them:
//  - The member's environment (library login, volume, and one extra Wi-Fi network) is saved on the device and
//    edited on the setup page; the build's values (secrets/sdkconfig.secrets) are only the defaults for a
//    fresh board. The two built-in Wi-Fi networks are baked in and are always tried as a fallback.
//  - Everything the program itself needs (Azure and Anthropic keys, memory storage token, setup-page
//    password, practice mode) is baked into the firmware at build time and cannot be changed on the
//    device.
struct Config {
    std::string wifi_ssid;      // the network saved on the setup page (empty: use only the built-in ones)
    std::string wifi_password;
    std::string wifi_builtin_ssid[2];  // the two networks baked into the firmware (either may be empty)
    std::string wifi_builtin_password[2];
    std::string va_user;
    std::string va_password;
    std::string azure_key;
    std::string azure_region = "eastus";
    std::string anthropic_key;
    std::string memory_url;                   // container SAS URL of memory.json (secret)
    std::string admin_password = "bookbook";  // HTTP Basic password for the LAN config page (default set by CONFIG_BOOKBOOK_ADMIN_PASSWORD)
    int volume = 80;                          // percent
    bool dry_run = true;                      // practice mode: pretend to add/remove, change nothing
};

namespace config {
void load();                       // call once at boot (initialises NVS)
Config get();                      // snapshot copy
esp_err_t save(const Config& cfg); // persist and make current
}  // namespace config
