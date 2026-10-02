// Closing the app must never leave the console hanging: once the app is on its way out, this ends the
// process by force if the clean-up hasn't finished in time. (A decoder or a download that never
// returns would otherwise hold the whole shutdown, and the Wii U with it.)
#pragma once

namespace exit_watchdog {

// The process ends `seconds` from now unless it has already ended by itself. A later call can
// only bring the deadline closer. Safe from any thread. Idle threads are cheap, so the first call
// starts one and it stays until the process ends.
void arm(double seconds);

// Whether arm() has been called (the app is on its way out).
bool armed();

// Lets tests that arm it go on afterwards.
void disarm();

}  // namespace exit_watchdog
