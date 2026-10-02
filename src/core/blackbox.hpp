// For a console that stops and takes the log with it. While developer updates are on, a thread of
// its own writes <dir>/blackbox.txt every few seconds, straight to the card: the main loop's
// counters, how far the logging has got, and for each thread whether it runs or waits, where it
// stopped, and which lock it waits for and who holds it. It allocates nothing and waits for no
// lock of the app's, so it goes on writing when the logging, the downloads or the main loop are
// what's stuck. The last three records stay in the file. A clean end deletes it; if the next start
// finds it, the last run stopped some other way, and its records go into that run's log.
#pragma once

#include <string>

namespace blackbox {

// After the log is open: what the last run's black box says, if it left one, repeated in the log
// and moved to blackbox-previous.txt. Returns the text ("" when there was none).
std::string report_last_run(const std::string& dir);

// On or off (the developer setting, asked again and again: cheap). The first time on, the thread
// starts; it writes a record every `period` seconds.
void keep(bool on, const std::string& dir, double period = 5.0);

// A clean end: the file goes away, so the next start doesn't take it for a stop.
void stop();

}  // namespace blackbox
