#pragma once
#include "esp_err.h"

namespace webconfig {

// Starts the setup web server (port 80) and the bookbook.local mDNS name.
// trust_setup_ap: when true (first-time setup, or BOOT held at power-up), clients on the setup
// access point need no password because the AP itself is WPA2-protected. Otherwise every
// request needs HTTP Basic auth (user "admin").
esp_err_t start(bool trust_setup_ap);

// Answers every DNS query with the AP address so phones open the setup page automatically.
void start_captive_dns();

}  // namespace webconfig
