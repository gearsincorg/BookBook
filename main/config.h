#pragma once
#include <string>
#include "esp_err.h"

// Device settings. Defaults come from the developer's git-ignored Kconfig secrets; anything saved
// through the web page (stored in NVS) overrides them.
struct Config {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string va_user;
    std::string va_password;
    std::string azure_key;
    std::string azure_region = "eastus";
    std::string anthropic_key;
    std::string admin_password = "bookbook";  // HTTP Basic password for the LAN config page
    int volume = 80;                          // percent
};

namespace config {
void load();                       // call once at boot (initialises NVS)
Config get();                      // snapshot copy
esp_err_t save(const Config& cfg); // persist and make current
}  // namespace config
