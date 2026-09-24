#include "logger/logger.hpp"

#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <mutex>

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

std::mutex g_mutex;  // keeps lines from different threads apart
FILE* g_file = nullptr;
long g_file_bytes = 0;
const auto g_start = std::chrono::steady_clock::now();

}  // namespace

void log_message(LogLevel level, const char* system, const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    const Style& s = STYLES[level >= LOG_OK && level <= LOG_DEBUG ? level : LOG_DEBUG];
    std::lock_guard<std::mutex> lock(g_mutex);
    std::printf("%s[%s] [%s] %s\x1b[0m\n", s.color, system ? system : "App", s.label, text);
    std::fflush(stdout);

    if (!g_file || g_file_bytes >= FILE_LIMIT) return;
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    int n = std::fprintf(g_file, "%9.3f [%s] [%s] %s\n", t, system ? system : "App", s.label, text);
    if (n > 0) g_file_bytes += n;
    if (g_file_bytes >= FILE_LIMIT) std::fputs("(log limit reached)\n", g_file);
    // Written through to the card: the last lines matter most when the console froze.
    std::fflush(g_file);
    fsync(fileno(g_file));
}

void log_to_file(const std::string& dir) {
    std::string path = dir + "/coffeeflix.log", previous = dir + "/coffeeflix-previous.log";
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fclose(g_file);
    std::remove(previous.c_str());
    std::rename(path.c_str(), previous.c_str());
    g_file = std::fopen(path.c_str(), "w");
    g_file_bytes = 0;
}
