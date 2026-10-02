// When the app dies (a bad memory access, an exception nobody caught), the Wii U shows the last
// picture and nothing else. What is known at that moment goes to <data dir>/coffeeflix-crash.log,
// written straight to the card, and into the log; the next start reads it back into its own log.
#pragma once

#include <cstddef>
#include <string>

namespace crash_report {

// Once, at the start: the handlers for uncaught exceptions and running out of memory, and the
// platform's for crashes. dir is where the crash file goes.
void install(const std::string& dir);

// After the log is open: what the last run left behind (a crash file, or a log that did not end
// with "Shutting down") is repeated in this run's log, where the developer log picks it up.
void report_last_run(const std::string& dir);

// A line in the crash file, at once and without allocating or locking: for code that is running
// because something just went wrong. Nothing if install hasn't been called.
void note_raw(const char* text, size_t length);

}  // namespace crash_report
