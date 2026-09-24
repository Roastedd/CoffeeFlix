#include "logger/logger.hpp"

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

std::mutex g_mutex;  // keeps lines from different threads apart

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
}
