#include "app/crash_report.hpp"

#include <cxxabi.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <thread>
#include <typeinfo>

#include "app/dev_log.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace crash_report {

namespace {

char g_path[256];  // the crash file, set by install (a plain array: read while things are going wrong)

constexpr const char* CRASH_FILE = "coffeeflix-crash.log";
constexpr const char* CRASH_PREVIOUS = "coffeeflix-crash-previous.log";
constexpr size_t READ_TAIL = 24 * 1024;
constexpr int TAIL_LINES = 40;

std::string type_of(const std::exception& e) {
    int status = 0;
    char* name = abi::__cxa_demangle(typeid(e).name(), nullptr, nullptr, &status);
    std::string out = status == 0 && name ? name : typeid(e).name();
    std::free(name);
    return out;
}

// An exception nobody caught (in a thread of ours, or in something we call) ends the process
// here. The last words go out from a thread of their own: the log may be what's stuck.
void on_terminate() {
    static std::atomic<int> once{0};
    if (once.exchange(1) == 0) {
        char text[400];
        try {
            if (std::current_exception()) throw;
            std::snprintf(text, sizeof(text), "terminate called with no exception in flight");
        } catch (const std::exception& e) {
            std::snprintf(text, sizeof(text), "uncaught %s: %s", type_of(e).c_str(), e.what());
        } catch (...) {
            std::snprintf(text, sizeof(text), "uncaught exception that is not a std::exception");
        }
        char line[480];
        int n = std::snprintf(line, sizeof(line), "TERMINATE %s\n", text);
        note_raw(line, n > 0 ? (size_t)n : 0);
        std::thread([text = std::string(text)] {
            log_message(LOG_ERROR, "Crash", "The app is ending: %s", text.c_str());
            dev_log::flush_now();
        }).detach();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
    std::abort();
}

// new fails: say how much memory there was, once, then let it throw as it would have.
void on_no_memory() {
    static std::atomic<int> once{0};
    if (once.exchange(1) == 0) {
        char line[160];
        int n = std::snprintf(line, sizeof(line), "OUT OF MEMORY %s\n", platform::memory_summary().c_str());
        note_raw(line, n > 0 ? (size_t)n : 0);
        log_message(LOG_ERROR, "Crash", "Out of memory (%s)", platform::memory_summary().c_str());
        dev_log::flush_now();
    }
    std::set_new_handler(nullptr);
    throw std::bad_alloc();
}

// The lines of text, the last n of them.
std::string last_lines(const std::string& text, int n) {
    size_t pos = text.size();
    while (n-- > 0 && pos > 0) {
        size_t nl = text.rfind('\n', pos - 1);
        if (nl == std::string::npos) return text;
        pos = nl;
    }
    return pos < text.size() ? text.substr(pos + 1) : text;
}

void log_lines(LogLevel level, const char* system, const std::string& text) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (end > at) log_message(level, system, "%.*s", (int)std::min<size_t>(end - at, 700), text.c_str() + at);
        at = end + 1;
    }
}

}  // namespace

void note_raw(const char* text, size_t length) {
    if (!g_path[0] || !length) return;
    int fd = open(g_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    ssize_t ignored = write(fd, text, length);
    (void)ignored;
    fsync(fd);
    close(fd);
}

void install(const std::string& dir) {
    std::snprintf(g_path, sizeof(g_path), "%s/%s", dir.c_str(), CRASH_FILE);
    std::set_terminate(on_terminate);
    std::set_new_handler(on_no_memory);
    platform::install_crash_reporter();
}

void report_last_run(const std::string& dir) {
    const std::string crash = dir + "/" + CRASH_FILE;
    std::string text;
    bool crashed = util::read_file(crash, text) && !text.empty();
    if (crashed) {
        log_message(LOG_ERROR, "Crash", "The last run crashed. What it wrote down as it did:");
        log_lines(LOG_ERROR, "Crash", text.size() > 8192 ? text.substr(text.size() - 8192) : text);
        const std::string kept = dir + "/" + CRASH_PREVIOUS;
        std::remove(kept.c_str());
        std::rename(crash.c_str(), kept.c_str());
    }

    // A log that stops anywhere but at "Shutting down": the last lines it got to the card.
    std::string previous;
    if (!util::read_file(dir + "/coffeeflix-previous.log", previous) || previous.empty()) return;
    const std::string tail = previous.size() > READ_TAIL ? previous.substr(previous.size() - READ_TAIL) : previous;
    if (tail.find("Shutting down") != std::string::npos && !crashed) return;
    log_message(LOG_WARNING, "Crash", "The last run did not end cleanly. The end of its log on the card:");
    log_lines(LOG_WARNING, "LastRun", last_lines(tail, TAIL_LINES));
}

}  // namespace crash_report
