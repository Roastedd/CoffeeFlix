// Settings > Send logs: this run's log and the one before go to dpaste.com without private data
// (logger/log_scrub), and a new GitHub issue links to them.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace bug_report {

constexpr int EXPIRY_DAYS = 30;  // dpaste.com keeps items 1 to 365 days

// The logs that go, in the data folder: this run's and the one before (logger.hpp).
extern const char* const LOG_FILES[2];

// What the report says about this console, and what to take out of the logs. Main thread.
struct Info {
    std::string version, platform, language, system_language;
    std::string log_dir;
    std::vector<std::string> secrets;  // from the settings: sign-in tokens, device IDs...
};
Info gather();

// The paste: a line about the build, then both logs without private data, each cut to fit by
// leaving out its middle. Reads the log files, so from a worker thread.
std::string compose(const Info& info);

struct Result {
    std::string url;    // the paste
    std::string error;  // for the user, when it didn't work
};
// Uploads the logs; `wanted` is asked as it goes, and false gives up (the screen was left).
// Blocks: call from a worker thread.
Result send(const Info& info, std::function<bool()> wanted = nullptr);

// A new issue on GitHub with the paste's link, the version and the console filled in, short
// enough for a QR code: less of it when all of it doesn't fit.
std::string issue_url(const Info& info, const std::string& paste_url);

}  // namespace bug_report
