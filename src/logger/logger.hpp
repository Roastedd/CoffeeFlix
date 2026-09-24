// Levelled console logging: "[System] [LEVEL] message", colored for terminals. On the Wii U,
// platform/wiiu/log_stdout.cpp forwards the console to the system log.
#pragma once

enum LogLevel { LOG_OK, LOG_WARNING, LOG_ERROR, LOG_DEBUG };

// printf-style; safe to call from any thread.
void log_message(LogLevel level, const char* system, const char* format, ...);
