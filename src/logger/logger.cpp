#include "logger/logger.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "core/cpu.hpp"
#include "platform/platform.hpp"

namespace {

struct Style {
    const char* label;
    const char* color;  // ANSI
};

const Style STYLES[] = {
    {"OK", "\x1b[32m"},
    {"WARNING", "\x1b[33m"},
    {"ERROR", "\x1b[31m"},
    {"DEBUG", "\x1b[34m"},
};

constexpr long FILE_LIMIT = 1 << 20;  // per run
constexpr size_t RECENT_LINES = 4000;

struct Line {
    std::string console, file;
};

std::mutex g_mutex;  // the lists below
std::condition_variable g_cv;
std::deque<Line> g_pending;       // for the writer thread
std::deque<std::string> g_recent;  // the last lines, for log_lines_since
uint64_t g_count = 0;              // lines logged so far
std::thread* g_writer = nullptr;   // never deleted: log_shutdown joins it
bool g_stop = false;

std::mutex g_io;  // the console and the file
FILE* g_file = nullptr;
long g_file_bytes = 0;
const auto g_start = std::chrono::steady_clock::now();

void write_out(const Line& l) {
    std::lock_guard<std::mutex> lock(g_io);
    std::fputs(l.console.c_str(), stdout);
    std::fflush(stdout);
    if (!g_file || g_file_bytes >= FILE_LIMIT) return;
    if (std::fputs(l.file.c_str(), g_file) >= 0) g_file_bytes += (long)l.file.size();
    if (g_file_bytes >= FILE_LIMIT) std::fputs("(log limit reached)\n", g_file);
    // Written through to the card: the last lines matter most when the console froze.
    std::fflush(g_file);
    fsync(fileno(g_file));
}

void writer_loop() {
    cpu::ThreadTag tag("log");
    platform::lower_thread_priority();
    std::unique_lock<std::mutex> lock(g_mutex);
    for (;;) {
        g_cv.wait(lock, [] { return g_stop || !g_pending.empty(); });
        if (g_pending.empty()) return;  // stopping, and all written
        Line l = std::move(g_pending.front());
        g_pending.pop_front();
        lock.unlock();
        write_out(l);
        lock.lock();
    }
}

}  // namespace

void log_message(LogLevel level, const char* system, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    const Style& s = STYLES[level >= LOG_OK && level <= LOG_DEBUG ? level : LOG_DEBUG];
    if (!system) system = "App";
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    char buf[1200];
    Line l;
    std::snprintf(buf, sizeof(buf), "%s[%s] [%s] %s\x1b[0m\n", s.color, system, s.label, text);
    l.console = buf;
    std::snprintf(buf, sizeof(buf), "%9.3f [%s] [%s] %s\n", t, system, s.label, text);
    l.file = buf;

    std::unique_lock<std::mutex> lock(g_mutex);
    g_recent.push_back(l.file);
    if (g_recent.size() > RECENT_LINES) g_recent.pop_front();
    g_count++;
    if (g_writer && !g_stop) {
        g_pending.push_back(std::move(l));
        lock.unlock();
        g_cv.notify_one();
    } else {
        lock.unlock();
        write_out(l);
    }
}

void log_to_file(const std::string& dir) {
    std::string path = dir + "/coffeeflix.log", previous = dir + "/coffeeflix-previous.log";
    {
        std::lock_guard<std::mutex> lock(g_io);
        if (g_file) std::fclose(g_file);
        std::remove(previous.c_str());
        std::rename(path.c_str(), previous.c_str());
        g_file = std::fopen(path.c_str(), "w");
        g_file_bytes = 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_writer || g_stop) return;
    g_writer = new std::thread(writer_loop);
    std::atexit(log_shutdown);  // when the app ends some other way than through app::run
}

void log_shutdown() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_writer || g_stop) return;
        g_stop = true;
    }
    g_cv.notify_one();
    g_writer->join();
}

uint64_t log_lines_since(uint64_t from, std::string& out, size_t max_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    uint64_t first = g_count - g_recent.size();  // the oldest line still kept
    if (from < first) {
        out += "(" + std::to_string(first - from) + " earlier lines weren't kept)\n";
        from = first;
    }
    for (uint64_t i = from; i < g_count; i++) {
        const std::string& line = g_recent[(size_t)(i - first)];
        if (!out.empty() && out.size() + line.size() > max_bytes) return i;
        out += line;
    }
    return g_count;
}
