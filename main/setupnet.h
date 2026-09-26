#pragma once
#include <string>
#include "esp_err.h"

// The setup network (Wi-Fi access point "Librarian-XXXX") on the member's request. The device also starts it by
// itself when it cannot join the saved Wi-Fi (main.cpp); that one is never timed out, because it is the only way
// to fix the Wi-Fi. One opened on request closes by itself once nobody has used the setup page for
// kIdleCloseMinutes, counted from the last authorised request to the setup web server (or from the moment it was
// opened, if nobody ever connects).
namespace setupnet {

constexpr int kIdleCloseMinutes = 10;

// The web address of the setup page (mDNS on the home network; on the setup network the device answers every name).
constexpr const char* kAddress = "librarian.local";
constexpr const char* kSpokenAddress = "librarian dot local";

struct Opened {
    std::string name;       // "Librarian-A1B2"
    std::string spoken;     // for speech: "Librarian, A, 1, B, 2"
    bool automatic = false; // it was already up because Wi-Fi is down: it stays up until Wi-Fi works
};

// Opens the setup network (or, if it is already open, keeps it open another kIdleCloseMinutes).
esp_err_t open_on_request(Opened& out);

}  // namespace setupnet
