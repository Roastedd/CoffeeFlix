// A line a second for a while (or every few seconds all the time), written by a thread of its
// own: where the main loop was and what the downloads were doing, for the log of a console that
// froze. The last line says when it stopped, and whether the whole app did (no more lines) or the
// main loop alone (lines go on while its frame count doesn't).
#pragma once

#include <cstdint>
#include <string>

namespace heartbeat {

// The main loop: phase `name` (a string literal) just ended / the frame just ended.
void phase_done(const char* name);
void frame_done();

// The same counters, read without taking any lock: frames done, the phase that ended last (null
// before the first) and how long ago, in milliseconds.
struct Snapshot {
    uint32_t frames;
    const char* phase;
    uint32_t phase_ms_ago;
};
Snapshot snapshot();

// A line every `period` seconds for as long as it's on (0: off), whatever the downloads are doing:
// a main loop or a whole app that stops shows in the log even when nothing else was wrong yet. The
// lines of arm() take its place, a second apart, while they last. `source` as in arm().
void keep(double period, std::string (*source)() = nullptr);

// From now on for `seconds` (longer if it is on already): a line a second, with `source`'s text
// in it (called from the heartbeat's thread; one for the whole process, null for none).
void arm(double seconds, std::string (*source)() = nullptr);

}  // namespace heartbeat
