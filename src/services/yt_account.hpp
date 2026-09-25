// Optional sign-in to a YouTube account, the way YouTube's TV and VR apps do it: the Wii U shows a code,
// you enter it at google.com/device on your phone or computer. CoffeeFlix never sees the password;
// it keeps the token Google gives it in coffeeflix.json until you sign out.
#pragma once

#include <string>

namespace yt_account {

struct Code {
    bool ok = false;
    std::string error;
    std::string device_code, user_code, url;
    int interval = 5;  // seconds between polls
    int expires_in = 1800;
};

// --- worker threads ---------------------------------------------------------------------------
Code request_code();

enum Poll { WAITING, SIGNED_IN, FAILED };
// Whether the code was approved yet. SIGNED_IN: the account is saved. `interval` grows when Google
// asks for fewer polls; FAILED explains itself in `error` (expired, declined...).
Poll poll(const std::string& device_code, int& interval, std::string& error);

// An access token for the account, renewed when it's about to run out (or with `renew`, after
// YouTube turned it down). "" when signed out or Google can't be reached (see `error`).
std::string access_token(bool renew, std::string& error);

// --- any thread -------------------------------------------------------------------------------
bool signed_in();
std::string name();
std::string photo();
int version();    // changes on sign-in and sign-out, for screens to reload
void sign_out();  // forgets the account and withdraws the token at Google (in the background)

}  // namespace yt_account
