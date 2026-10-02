#include "app/exit_watchdog.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "app/dev_log.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace exit_watchdog {

namespace {

using Clock = std::chrono::steady_clock;

// Never destroyed: the watching thread may still be waiting on these while the process exits.
struct State {
    std::mutex m;
    std::condition_variable cv;
    Clock::time_point deadline;
    bool armed = false, started = false;
};

State& state() {
    static State* s = new State;
    return *s;
}

void watch() {
    platform::set_thread_name("exit watchdog");
    platform::raise_thread_priority();  // it has to run while something else spins or waits
    State& s = state();
    std::unique_lock<std::mutex> lock(s.m);
    for (;;) {
        if (!s.armed) {
            s.cv.wait(lock);
        } else if (s.cv.wait_until(lock, s.deadline) == std::cv_status::timeout && s.armed && Clock::now() >= s.deadline) {
            break;
        }
    }
    lock.unlock();
    // The log may be what's stuck (a thread that hangs holding its lock), so the last words go out
    // from a thread of their own, and the process ends whether they got out or not.
    std::thread([] {
        log_message(LOG_ERROR, "App", "Closing took too long: ending the app");
        dev_log::flush_now();
    }).detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    platform::terminate_now(0);
}

}  // namespace

void arm(double seconds) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    Clock::time_point at = Clock::now() + std::chrono::milliseconds((long long)(seconds * 1000));
    if (!s.armed || at < s.deadline) s.deadline = at;
    s.armed = true;
    if (!s.started) {
        s.started = true;
        std::thread(watch).detach();
    }
    s.cv.notify_all();
}

bool armed() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    return s.armed;
}

void disarm() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.m);
    s.armed = false;
    s.cv.notify_all();
}

}  // namespace exit_watchdog
