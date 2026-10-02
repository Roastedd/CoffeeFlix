#include "core/blackbox.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "core/cpu.hpp"
#include "core/heartbeat.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace blackbox {

namespace {

constexpr const char* FILE_NAME = "blackbox.txt";
constexpr const char* PREVIOUS_NAME = "blackbox-previous.txt";
constexpr int SLOTS = 3;         // the records the file keeps
constexpr size_t RECORD = 2800;  // each, with a line for each thread

// Never destroyed (the thread may be in the middle of a record as the process ends), and plain
// arrays: nothing here allocates.
struct State {
    std::atomic<bool> on{false}, stopping{false}, busy{false};
    std::atomic<uint32_t> period_ms{5000};
    bool started = false;
    char path[256] = "";
    char records[SLOTS][RECORD];
    size_t lengths[SLOTS] = {};
    uint32_t count = 0;  // records made so far
};

State& state() {
    static State* s = new State;
    return *s;
}

// printf into the record after what it holds; no allocating (no floating point either).
void put(char* record, size_t& used, const char* format, ...) {
    if (used >= RECORD - 1) return;
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(record + used, RECORD - used, format, args);
    va_end(args);
    if (n > 0) used = std::min(used + (size_t)n, RECORD - 1);
}

void write_file(State& s, uint32_t newest) {
    const int fd = open(s.path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    const uint32_t first = newest >= SLOTS - 1 ? newest - (SLOTS - 1) : 0;
    for (uint32_t r = first; r <= newest; r++) {
        ssize_t ignored = write(fd, s.records[r % SLOTS], s.lengths[r % SLOTS]);
        (void)ignored;
    }
    fsync(fd);
    close(fd);
}

void tick(State& s) {
    const uint32_t n = s.count++;
    char* record = s.records[n % SLOTS];
    size_t used = 0;
    const unsigned long ms = (unsigned long)(log_seconds() * 1000);
    const heartbeat::Snapshot beat = heartbeat::snapshot();
    const LogCounts log = log_counts();
    put(record, used, "== %lu.%03lu s: frame %u, last phase %s ended %u ms ago; log: called %u, queued %u, written %u\n", ms / 1000,
        ms % 1000, (unsigned)beat.frames, beat.phase ? beat.phase : "none", (unsigned)beat.phase_ms_ago, (unsigned)log.called,
        (unsigned)log.queued, (unsigned)log.written);
    if (used < RECORD - 1) used += cpu::where(record + used, RECORD - used);
    s.lengths[n % SLOTS] = used;
    write_file(s, n);
}

void run() {
    platform::set_thread_name("black box");
    platform::raise_thread_priority();  // it has to run while something else spins or waits
    State& s = state();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(s.period_ms.load()));
        if (!s.on.load() || s.stopping.load()) continue;
        s.busy.store(true);
        if (!s.stopping.load()) tick(s);
        s.busy.store(false);
    }
}

}  // namespace

std::string report_last_run(const std::string& dir) {
    const std::string path = dir + "/" + FILE_NAME, previous = dir + "/" + PREVIOUS_NAME;
    std::string text;
    if (!util::read_file(path, text)) return "";
    std::remove(previous.c_str());
    std::rename(path.c_str(), previous.c_str());
    if (text.empty()) return "";
    log_message(LOG_WARNING, "Blackbox", "The last run's black box, a record every few seconds (R running, r ready, W waiting, S suspended), the last three:");
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (end > at) log_message(LOG_WARNING, "Blackbox", "%.*s", (int)std::min<size_t>(end - at, 700), text.c_str() + at);
        at = end + 1;
    }
    return text;
}

void keep(bool on, const std::string& dir, double period) {
    State& s = state();
    s.period_ms.store((uint32_t)std::max(50.0, period * 1000));
    if (on && !s.started) {
        s.started = true;  // from the main loop only, so no one else is starting it
        std::snprintf(s.path, sizeof(s.path), "%s/%s", dir.c_str(), FILE_NAME);
        std::thread(run).detach();
    }
    s.on.store(on && s.started);
}

void stop() {
    State& s = state();
    s.stopping.store(true);
    // A record being written (the card can take a while) is waited for a moment; if it isn't done,
    // the file stays, and the next start reads it as a stop it wasn't.
    for (int i = 0; i < 30 && s.busy.load(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!s.busy.load() && s.path[0]) std::remove(s.path);
}

}  // namespace blackbox
