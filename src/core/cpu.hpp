// Where the CPU time goes, for the log: threads say what they are with a ThreadTag, and report()
// tells how much of a core each kind used since it was last asked.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace cpu {

// Counts the calling thread's CPU time under `name` (a string literal: it's kept, not copied)
// until it goes away, which has to happen on the same thread, before the thread ends.
class ThreadTag {
public:
    explicit ThreadTag(const char* name);
    ~ThreadTag();
    ThreadTag(const ThreadTag&) = delete;
    ThreadTag& operator=(const ThreadTag&) = delete;

private:
    int id_;
};

// For a thread someone else starts and stops (SDL's sound thread): tag() on the thread itself,
// untag() from whoever stops it, before it stops. untag(-1) does nothing.
int tag(const char* name);
void untag(int id);

// "video 41%, main 35%, download×4 18%, sound 3%; 97% in all": the CPU time each name used
// since the last report, as a share of one core (the Wii U has three). Empty when the system
// doesn't tell.
std::string report();

// Wii U, developer log: the clocks, then the raw scheduler counters of the first thread of each
// name (platform::thread_clock_debug), a line each, to check what report() reads against.
// Empty elsewhere.
std::vector<std::string> clock_debug();

// For the black box: a line for each tagged thread (name, state, where it stopped, what it waits
// for: platform::thread_where) in `out`, as many as fit, without allocating or waiting for the
// list (a note says so when someone else has it). Returns the length.
size_t where(char* out, size_t capacity);

}  // namespace cpu
