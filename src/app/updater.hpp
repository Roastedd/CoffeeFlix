// Updating CoffeeFlix from its GitHub releases on the console itself.
//
// A newer release's coffeeflix.wuhb downloads next to the running one as
// coffeeflix.wuhb.download (Aroma lists only names ending in .wuhb), is checked against the size
// and SHA-256 GitHub publishes for it, and goes in as CoffeeFlix closes: Aroma keeps the running
// bundle open, and lets go of it only once nothing reads /vol/content any more. The replaced
// version stays as coffeeflix.wuhb.bak, to switch back to.
#pragma once

#include <cstdint>
#include <string>

namespace updater {

enum State { IDLE, CHECKING, UP_TO_DATE, AVAILABLE, DOWNLOADING, READY, FAILED };

struct Release {
    std::string version;  // "2.2.0"
    std::string notes;    // what's new, as plain text
    std::string url;      // its coffeeflix.wuhb
    std::string sha256;   // lower-case hex, as GitHub published it
    int64_t size = 0;
};

const char* version();  // this build's
bool supported();       // running from a bundle that can be replaced
const std::string& unsupported_reason();

void init();  // start-up: tidies up after the last run, says what was installed
void tick();  // every frame: the daily check, automatic downloads
void stop();  // before the worker threads end: cancels a download

State state();
const Release& release();    // AVAILABLE, DOWNLOADING, READY
const std::string& error();  // FAILED
float progress();            // DOWNLOADING, 0 to 1
int64_t last_checked();      // unix time, 0 if never

void check();
void download();
// Stops a download, or drops a READY one. Either way this update is left alone this session.
void cancel();
bool cancelling();

// Automatic: new versions download in the background and install when CoffeeFlix closes.
// Otherwise a new version gets a notice, and waits for Settings.
bool automatic();
void set_automatic(bool on);

// Closes CoffeeFlix to install the READY update.
void install_now();

// The version kept from before the last update, if any, and switching to it (CoffeeFlix closes).
bool has_previous();
std::string previous_version();
void switch_to_previous();

// The very last thing before exiting, once nothing reads bundled files: the swap.
void finish_on_exit();

}  // namespace updater
