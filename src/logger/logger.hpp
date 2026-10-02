// Levelled console logging: "[System] [LEVEL] message", colored for terminals. On the Wii U,
// platform/wiiu/log_stdout.cpp forwards the console to the system log.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

enum LogLevel { LOG_OK, LOG_WARNING, LOG_ERROR, LOG_DEBUG };

// printf-style; safe to call from any thread.
void log_message(LogLevel level, const char* system, const char* format, ...);

// From now on, also writes the log to <dir>/coffeeflix.log, line by line so it survives a
// freeze. The previous run's log is kept as coffeeflix-previous.log. The writing (the SD card can
// take a while) happens on a thread of its own from here on, so no one waits for it.
void log_to_file(const std::string& dir);
// Writes out what's still waiting and ends that thread; lines after this are written directly.
void log_shutdown();
// How many lines wait for that thread (the card is slow, or stuck).
size_t log_pending();

// How far the logging has got, readable without waiting for anything (the black box reads it
// while the logging may be what's stuck): calls to log_message, those that got a line queued, and
// lines written to the card. Calls ahead of the queued ones are threads waiting inside log_message;
// queued ahead of written means the writer is waiting on the card.
struct LogCounts {
    uint32_t called, queued, written;
};
LogCounts log_counts();
// Seconds since the log's own zero: the time stamps of its lines.
double log_seconds();

// The log so far, for sending elsewhere (developer builds): appends the lines after the first
// `from` ones, as the file has them, up to about `max_bytes`, and returns how many lines that
// makes. Only the last few thousand are kept for this.
uint64_t log_lines_since(uint64_t from, std::string& out, size_t max_bytes);
