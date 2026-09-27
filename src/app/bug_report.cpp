#include "app/bug_report.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "app/updater.hpp"
#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/qr.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/log_scrub.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "services/smb.hpp"

namespace bug_report {

const char* const LOG_FILES[2] = {"coffeeflix.log", "coffeeflix-previous.log"};

namespace {

// https://dpaste.com/api/v2/: no account needed, one request a second, up to 1 MB an item.
const char* const PASTE_API = "https://dpaste.com/api/v2/";
constexpr size_t MAX_PASTE = 900 * 1000;
const char* const NEW_ISSUE = "https://github.com/Roastedd/CoffeeFlix/issues/new?";

// Settings whose values are private wherever they show up in the log.
const char* const PRIVATE_SETTINGS[] = {"yt_account_token", "yt_visitor", "twitch_device_id", "jf_token",
                                        "jf_device_id",     "jf_user_id", "jf_user_name"};

void add_secret(std::vector<std::string>& out, const std::string& v) {
    if (v.empty()) return;
    // As stored, and as links have it (visitor data is stored percent-encoded).
    for (const std::string& s : {v, util::url_decode(v), util::url_encode(v)})
        if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
}

// "https://jf.example.org:8096/" -> "jf.example.org"
std::string host_of(const std::string& url) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : start + 3;
    size_t end = url.find_first_of(":/?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Cuts the middle out of a log that's too long: the start says which version runs on what, the
// end what happened last.
std::string fit(const std::string& log, size_t max) {
    if (log.size() <= max) return log;
    max = std::max<size_t>(max, 1024);
    size_t head = log.rfind('\n', max / 5);
    head = head == std::string::npos ? 0 : head + 1;
    size_t tail = log.find('\n', log.size() - (max - head) + 64);
    tail = tail == std::string::npos ? log.size() : tail + 1;
    size_t lines = std::count(log.begin() + head, log.begin() + tail, '\n');
    return log.substr(0, head) + util::fmt("[... %zu lines left out ...]\n", lines) + log.substr(tail);
}

// For the issue link: spaces as +, and what links carry as it is, so it (and its QR code) stays
// short.
std::string query_encode(std::string_view s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || (c && std::strchr("-._~:/,()", c))) {
            out += (char)c;
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

}  // namespace

Info gather() {
    Info in;
    in.version = updater::version();
    // The Aroma build runs from the .wuhb, whose files show at /vol/content; the Tiramisu one is a
    // bare .rpx with them on the SD card.
    in.platform = !platform::is_wiiu()                                    ? platform::name()
                  : util::starts_with(platform::content_dir(), "/vol/content") ? "Wii U (Aroma)"
                                                                          : "Wii U (Tiramisu)";
    in.language = i18n::current().code;
    in.system_language = platform::system_language();
    in.log_dir = platform::data_dir();
    for (const char* key : PRIVATE_SETTINGS) add_secret(in.secrets, store::get_str(key));
    add_secret(in.secrets, host_of(store::get_str("jf_server")));
    for (const smb::Share& s : smb::saved_shares()) add_secret(in.secrets, s.password);
    if (const char* home = getenv("HOME")) add_secret(in.secrets, home);  // desktop paths name the user
    return in;
}

std::string compose(const Info& info) {
    struct Part {
        const char* what;
        bool found = false;
        size_t size = 0;
        std::string text;
    } parts[2] = {{"this run"}, {"the run before"}};
    for (int i = 0; i < 2; i++) {
        std::string raw;
        parts[i].found = util::read_file(info.log_dir + "/" + LOG_FILES[i], raw);
        parts[i].size = raw.size();
        parts[i].text = scrub_log(raw, info.secrets);
    }
    std::string out = util::fmt("CoffeeFlix %s on %s, language %s (the console's: %s)\n", info.version.c_str(),
                                info.platform.c_str(), info.language.c_str(), info.system_language.c_str());
    out += "Sent from Settings > Send logs. Sign-in tokens, device IDs, signed links, email addresses and public IP "
           "addresses were taken out.\n";

    // Half the room each, and what one doesn't need to the other.
    size_t room = MAX_PASTE - out.size() - 512, half = room / 2;
    size_t a = parts[0].text.size(), b = parts[1].text.size();
    size_t limit[2] = {b < half ? room - b : half, a < half ? room - a : half};
    for (int i = 0; i < 2; i++) {
        const Part& p = parts[i];
        out += util::fmt("\n======== %s: %s ", LOG_FILES[i], p.what);
        if (!p.found) {
            out += "(none) ========\n";
            continue;
        }
        std::string text = fit(p.text, limit[i]);
        out += util::fmt("(%s%s) ========\n", util::format_bytes(p.size).c_str(),
                         text.size() < p.text.size() ? ", the middle left out" : "");
        out += text;
    }
    return out;
}

Result send(const Info& info, std::function<bool()> wanted) {
    Result res;
    std::string content = compose(info);
    if (wanted && !wanted()) return res;

    // Multipart, so the log goes as it is rather than percent-encoded (much longer).
    std::string boundary = "CoffeeFlix-" + util::random_hex(12);
    auto field = [&](const char* name, const std::string& value) {
        return "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
    };
    http::Request req;
    req.method = "POST";
    req.url = util::env_or("COFFEEFLIX_PASTE_API", PASTE_API);
    req.body = field("content", content) + field("title", util::fmt("CoffeeFlix %s logs", info.version.c_str())) +
               field("expiry_days", std::to_string(EXPIRY_DAYS)) + "--" + boundary + "--\r\n";
    // No "Expect: 100-continue" (curl would wait for an answer before sending the body).
    req.headers = {{"Content-Type", "multipart/form-data; boundary=" + boundary}, {"Expect", ""}};
    req.timeout = 120;
    req.stall_seconds = 20;
    req.max_bytes = 64 * 1024;
    if (wanted) req.keep_going = [wanted](double) { return wanted(); };
    http::Response r = http::perform(req);
    if (wanted && !wanted()) return res;
    if (!r.ok()) {
        res.error = r.error.empty() ? util::fmt(tr("HTTP error %ld"), r.status) : r.error;
        log_message(LOG_WARNING, "Report", "Couldn't upload the logs: %s", res.error.c_str());
        return res;
    }

    // The item's link, in the Location header and as the body.
    auto loc = r.headers.find("location");
    std::string url = util::trim(loc != r.headers.end() ? loc->second : r.body);
    if (!(util::starts_with(url, "https://") || util::starts_with(url, "http://")) || url.size() > 200 ||
        url.find_first_of(" \t\r\n\"<>") != std::string::npos) {
        res.error = tr("dpaste.com didn't send back a link");
        log_message(LOG_WARNING, "Report", "No link in the answer to the upload (HTTP %ld)", r.status);
        return res;
    }
    res.url = url;
    log_message(LOG_OK, "Report", "Uploaded %s of logs to %s, kept %d days", util::format_bytes(content.size()).c_str(),
                url.c_str(), EXPIRY_DAYS);
    return res;
}

std::string issue_url(const Info& info, const std::string& paste_url) {
    // In English, like the rest of the issues (and fewer bytes for the QR code than most languages).
    std::string logs = "Logs: " + paste_url;
    std::string about =
        util::fmt("CoffeeFlix %s on %s, language %s", info.version.c_str(), info.platform.c_str(), info.language.c_str());
    std::string ask = "\n\nWhat happened?\n";
    std::string body = std::string(NEW_ISSUE) + "body=";
    // The whole of it, then less while it's too long for a QR code (qr.hpp).
    const std::string tries[] = {
        std::string(NEW_ISSUE) + "title=" + query_encode("Bug: ") + "&body=" + query_encode(logs + "\n" + about + ask),
        body + query_encode(logs + "\n" + about + ask),
        body + query_encode(logs + "\n" + about),
        body + query_encode(logs),
    };
    for (const std::string& t : tries)
        if (qr::encode(t).size) return t;
    return paste_url;
}

}  // namespace bug_report
