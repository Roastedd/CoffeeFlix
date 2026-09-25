#include "services/yt_account.hpp"

#include <atomic>
#include <ctime>
#include <mutex>

#include "core/http.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "services/youtube.hpp"

namespace yt_account {

namespace {

// The OAuth client of YouTube's VR app, which FourthTube and Kodi's YouTube add-on use as well:
// the ANDROID_VR InnerTube client accepts its tokens (the TV clients want the TV app's own).
// It is public, not a secret of ours.
const char* const CLIENT = "client_id=652469312169-4lvs9bnhr9lpns9v451j5oivd81vjvu1.apps.googleusercontent.com"
                           "&client_secret=3fTWrBJI5Uojm1TK7_iJCW5Z";
const char* const SCOPE = "https://www.googleapis.com/auth/youtube";
const char* const OAUTH = "https://www.youtube.com/o/oauth2/";

std::mutex g_renew_m;  // one renewal at a time
std::mutex g_m;        // the access token
std::string g_access;
double g_expires = 0;
std::atomic<int> g_version{0};

http::Response post_form(const char* path, const std::string& form) {
    http::Request req;
    req.method = "POST";
    req.url = std::string(OAUTH) + path;
    req.body = form;
    req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
    req.timeout = 15;
    return http::perform(req);
}

std::string describe(const http::Response& r, json_t* root) {
    if (!r.error.empty()) return r.error;
    std::string e = json::str(root, {"error"});
    return e.empty() ? util::fmt("Google answered %ld", r.status) : e;
}

// Keeps the tokens of a token response; false when it has none.
bool take_tokens(json_t* root) {
    std::string access = json::str(root, {"access_token"});
    if (access.empty()) return false;
    std::string refresh = json::str(root, {"refresh_token"});
    int64_t expires = json::num(root, {"expires_in"}, 3600);
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_access = access;
        g_expires = util::now_seconds() + (double)expires;
    }
    if (!refresh.empty()) store::set_str("yt_account_token", refresh);
    return true;
}

void forget() {
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_access.clear();
        g_expires = 0;
    }
    store::set_str("yt_account_token", "");
    store::set_str("yt_account_name", "");
    store::set_str("yt_account_photo", "");
    store::fav_clear("yt_account_channel");  // the account's channels (screens/youtube_pages.cpp)
    g_version++;
}

}  // namespace

Code request_code() {
    Code c;
    std::string form = std::string(CLIENT) + "&scope=" + util::url_encode(SCOPE) +
                       "&device_id=wiiu-" + std::to_string((long long)time(nullptr)) + "&device_model=Wii%20U";
    http::Response r = post_form("device/code", form);
    json::Doc doc = json::Doc::parse(r.body);
    c.device_code = json::str(doc.get(), {"device_code"});
    c.user_code = json::str(doc.get(), {"user_code"});
    c.url = json::str(doc.get(), {"verification_url"}, "https://www.google.com/device");
    c.interval = (int)json::num(doc.get(), {"interval"}, 5);
    c.expires_in = (int)json::num(doc.get(), {"expires_in"}, 1800);
    c.ok = r.ok() && !c.device_code.empty() && !c.user_code.empty();
    if (!c.ok) {
        c.error = "Couldn't get a code from Google (" + describe(r, doc.get()) + ")";
        log_message(LOG_WARNING, "YouTube", "%s", c.error.c_str());
    }
    return c;
}

Poll poll(const std::string& device_code, int& interval, std::string& error) {
    http::Response r = post_form("token", std::string(CLIENT) + "&code=" + util::url_encode(device_code) +
                                              "&grant_type=" + util::url_encode("http://oauth.net/grant_type/device/1.0"));
    if (!r.error.empty()) return WAITING;  // a network hiccup: ask again next time
    json::Doc doc = json::Doc::parse(r.body);
    std::string e = json::str(doc.get(), {"error"});
    if (r.status == 428 || e == "authorization_pending") return WAITING;
    if (e == "slow_down") {
        interval += 5;
        return WAITING;
    }
    if (!take_tokens(doc.get())) {
        error = e == "expired_token"   ? "The code ran out of time"
                : e == "access_denied" ? "Sign-in was cancelled on the other device"
                                       : "Couldn't sign in (" + describe(r, doc.get()) + ")";
        log_message(LOG_WARNING, "YouTube", "Sign-in failed: %s", describe(r, doc.get()).c_str());
        return FAILED;
    }
    youtube::AccountInfo info;
    std::string err;
    if (youtube::account_info(info, err)) {
        store::set_str("yt_account_name", info.name);
        store::set_str("yt_account_photo", info.photo);
    } else {
        log_message(LOG_WARNING, "YouTube", "Signed in, but the account's name didn't load: %s", err.c_str());
    }
    log_message(LOG_OK, "YouTube", "Signed in to a YouTube account");
    g_version++;
    return SIGNED_IN;
}

std::string access_token(bool renew, std::string& error) {
    std::lock_guard<std::mutex> renewing(g_renew_m);
    {
        std::lock_guard<std::mutex> lk(g_m);
        if (!renew && !g_access.empty() && util::now_seconds() < g_expires - 60) return g_access;
    }
    std::string refresh = store::get_str("yt_account_token", "");
    if (refresh.empty()) {
        error = "Not signed in to YouTube";
        return "";
    }
    http::Response r = post_form("token", std::string(CLIENT) + "&refresh_token=" + util::url_encode(refresh) +
                                              "&grant_type=refresh_token");
    json::Doc doc = json::Doc::parse(r.body);
    if (r.ok() && take_tokens(doc.get())) {
        std::lock_guard<std::mutex> lk(g_m);
        return g_access;
    }
    if (json::str(doc.get(), {"error"}) == "invalid_grant") {
        // Withdrawn from the Google account's settings, or unused for months.
        log_message(LOG_WARNING, "YouTube", "Google no longer accepts the sign-in; signed out");
        forget();
        error = "Signed out of YouTube: sign in again in Settings";
        return "";
    }
    error = "Couldn't renew the YouTube sign-in (" + describe(r, doc.get()) + ")";
    log_message(LOG_WARNING, "YouTube", "%s", error.c_str());
    return "";
}

bool signed_in() { return !store::get_str("yt_account_token", "").empty(); }

std::string name() {
    std::string n = store::get_str("yt_account_name", "");
    return n.empty() ? "YouTube account" : n;
}

std::string photo() { return store::get_str("yt_account_photo", ""); }

int version() { return g_version.load(); }

void sign_out() {
    std::string refresh = store::get_str("yt_account_token", "");
    forget();
    if (refresh.empty()) return;
    log_message(LOG_OK, "YouTube", "Signed out of the YouTube account");
    // Withdrawing the refresh token ends the access tokens made from it too.
    tasks::submit(tasks::API, [refresh]() -> std::function<void()> {
        post_form("revoke", "token=" + util::url_encode(refresh));
        return nullptr;
    });
}

}  // namespace yt_account
