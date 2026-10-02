// Crashes and stalls on the Wii U: see platform::install_crash_reporter.
#pragma once

namespace platform::crash {

// Installs the exception handlers and the thread that watches the main loop.
void install();
// The main loop went round once (main thread).
void frame();
// Where the main thread is, for the line logged when it stops. A string literal.
void phase(const char* what);
// HOME took the app out of the foreground, or gave it back.
void background(bool in_background);

}  // namespace platform::crash
