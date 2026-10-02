#include "core/heartbeat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace heartbeat {

namespace {

using Clock = std::chrono::steady_clock;

// 32-bit: the Wii U's CPU has no 64-bit atomics. Milliseconds wrap after 49 days, which a
// difference of two of them doesn't mind.
std::atomic<uint32_t> g_frames{0}, g_phase_ms{0};
std::atomic<const char*> g_phase{nullptr};

uint32_t milliseconds() { return (uint32_t)(util::now_seconds() * 1000.0); }

// Never destroyed: the thread may still be waiting on these while the process exits.
struct State {
    std::mutex m;
    std::condition_variable cv;
    double until = 0;   // a line a second until then
    double period = 0;  // and a line every `period` seconds otherwise (0: none)
    std::string (*source)() = nullptr;
    bool started = false;
};

State& state() {
    static State* s = new State;
    return *s;
}

void beat() {
    platform::set_thread_name("heartbeat");
    platform::raise_thread_priority();  // it has to run while something else spins or waits
    State& s = state();
    uint32_t frames_before = 0;
    double last = 0;  // when the last line went out (0: none to count the frames since)
    std::unique_lock<std::mutex> lock(s.m);
    for (;;) {
        if (util::now_seconds() >= s.until && s.period <= 0) {
            last = 0;
            s.cv.wait(lock);
            continue;
        }
        auto source = s.source;
        lock.unlock();
        const double now = util::now_seconds();
        const uint32_t frames = g_frames.load(), now_ms = milliseconds();
        const char* phase = g_phase.load();
        std::string text = util::fmt("main: frame %u", frames);
        if (last > 0) {
            const double gap = now - last;
            text += gap < 1.5 ? util::fmt(" (%u in the last second)", frames - frames_before)
                            : util::fmt(" (%u in the last %.0f s)", frames - frames_before, gap);
        }
        if (phase) text += util::fmt(", its %s phase ended %.2f s ago", phase, (uint32_t)(now_ms - g_phase_ms.load()) / 1000.0);
        if (source) text += "; " + source();
        text += util::fmt("; %s; %u log lines waiting", platform::memory_summary().c_str(), (unsigned)log_pending());
        log_message(LOG_DEBUG, "Beat", "%s", text.c_str());
        frames_before = frames;
        last = now;
        lock.lock();
        const double wait = util::now_seconds() < s.until ? 1.0 : s.period;
        s.cv.wait_until(lock, Clock::now() + std::chrono::milliseconds((long)(wait * 1000)));
    }
}

}  // namespace

void phase_done(const char* name) {
    g_phase.store(name);
    g_phase_ms.store(milliseconds());
}

void frame_done() { g_frames.fetch_add(1); }

Snapshot snapshot() {
    const char* phase = g_phase.load();
    return {g_frames.load(), phase, phase ? (uint32_t)(milliseconds() - g_phase_ms.load()) : 0};
}

void keep(double period, std::string (*source)()) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    const bool changed = (period > 0) != (s.period > 0);
    s.period = std::max(period, 0.0);
    if (source) s.source = source;
    if (s.period > 0 && !s.started) {
        s.started = true;
        std::thread(beat).detach();
    }
    if (changed) s.cv.notify_all();
}

void arm(double seconds, std::string (*source)()) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    const double now = util::now_seconds();
    const bool asleep = now >= s.until;
    s.until = std::max(s.until, now + seconds);
    if (source) s.source = source;
    if (!s.started) {
        s.started = true;
        std::thread(beat).detach();
    }
    if (asleep) s.cv.notify_all();
}

}  // namespace heartbeat
