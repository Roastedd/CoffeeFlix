// Levelled console logging: "[System] [LEVEL] message", colored for terminals. On the Wii U,
// platform/wiiu/log_stdout.cpp forwards the console to the system log.
#pragma once

#include <string>

enum LogLevel { LOG_OK, LOG_WARNING, LOG_ERROR, LOG_DEBUG };

// printf-style; safe to call from any thread.
void log_message(LogLevel level, const char* system, const char* format, ...);

// From now on, also writes the log to <dir>/coffeeflix.log, flushed line by line so it survives
// a freeze. The previous run's log is kept as coffeeflix-previous.log.
void log_to_file(const std::string& dir);
