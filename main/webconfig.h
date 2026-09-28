#pragma once
#include "esp_err.h"

namespace webconfig {

// Starts the setup web server (port 80) and the librarian.local mDNS name.
// trust_setup_ap: when true (first-time setup, before any Wi-Fi is saved), clients on the setup access point need
// no password because the AP itself is WPA2-protected. Otherwise every request needs HTTP Basic auth (user "admin").
esp_err_t start(bool trust_setup_ap);

// Answers every DNS query with the AP address so phones open the setup page automatically.
void start_captive_dns();
void stop_captive_dns();

// When the setup server last handled an authorised request (ms since boot), 0 if never. Used to close a setup
// network that was opened on request once it has been idle for a while.
long long last_admin_activity_ms();

}  // namespace webconfig
