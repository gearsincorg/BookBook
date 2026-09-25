#pragma once
#include "esp_err.h"

namespace webconfig {

// Starts the setup web server (port 80) and the bookbook.local mDNS name.
// trust_setup_ap: when true (first-time setup; a 5 s Key1 hold enables it later), clients on the setup
// access point need no password because the AP itself is WPA2-protected. Otherwise every
// request needs HTTP Basic auth (user "admin").
esp_err_t start(bool trust_setup_ap);

// Grants or revokes password-free access for clients on the setup access point.
void set_trust_setup_ap(bool trust);

// Answers every DNS query with the AP address so phones open the setup page automatically.
void start_captive_dns();

}  // namespace webconfig
