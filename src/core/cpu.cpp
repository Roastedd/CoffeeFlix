#include "core/cpu.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "core/util.hpp"
#include "platform/platform.hpp"

namespace cpu {

namespace {

struct Thread {
    const char* name;
    void* handle;
    uint64_t counted;  // CPU time already in a report (or before the tag)
};

std::mutex g_m;
std::map<int, Thread> g_threads;
std::map<std::string, uint64_t> g_ended;  // CPU time of threads untagged since the last report
int g_next_id = 0;
auto g_last_report = std::chrono::steady_clock::now();

}  // namespace

int tag(const char* name) {
    platform::set_thread_name(name);
    void* handle = platform::current_thread();
    uint64_t now = platform::thread_cpu_ns(handle);
    std::lock_guard<std::mutex> lock(g_m);
    g_threads[g_next_id] = Thread{name, handle, now};
    return g_next_id++;
}

void untag(int id) {
    std::lock_guard<std::mutex> lock(g_m);
    auto it = g_threads.find(id);
    if (it == g_threads.end()) return;
    uint64_t now = platform::thread_cpu_ns(it->second.handle);
    if (now > it->second.counted) g_ended[it->second.name] += now - it->second.counted;
    g_threads.erase(it);
}

ThreadTag::ThreadTag(const char* name) : id_(tag(name)) {}
ThreadTag::~ThreadTag() { untag(id_); }

std::string report() {
    struct Sum {
        std::string name;
        uint64_t ns = 0;
        int threads = 0;
    };
    std::vector<Sum> sums;
    auto sum = [&](const std::string& name) -> Sum& {
        for (Sum& s : sums)
            if (s.name == name) return s;
        sums.push_back({name});
        return sums.back();
    };

    std::lock_guard<std::mutex> lock(g_m);
    auto now = std::chrono::steady_clock::now();
    double wall_ns = std::chrono::duration<double, std::nano>(now - g_last_report).count();
    g_last_report = now;
    for (auto& [id, t] : g_threads) {
        uint64_t used = platform::thread_cpu_ns(t.handle);
        Sum& s = sum(t.name);
        s.threads++;
        if (used > t.counted) s.ns += used - t.counted;
        t.counted = std::max(t.counted, used);
    }
    for (auto& [name, ns] : g_ended) sum(name).ns += ns;
    g_ended.clear();
    if (wall_ns <= 0) return "";

    std::sort(sums.begin(), sums.end(), [](const Sum& a, const Sum& b) { return a.ns > b.ns; });
    std::string out;
    uint64_t total = 0;  // everything, the names under 0.5% too
    for (const Sum& s : sums) {
        total += s.ns;
        double pct = s.ns * 100.0 / wall_ns;
        if (pct < 0.5) continue;
        if (!out.empty()) out += ", ";
        out += s.name;
        if (s.threads > 1) out += util::fmt("\xC3\x97%d", s.threads);
        out += util::fmt(" %.0f%%", pct);
    }
    if (out.empty()) return "";  // nothing worth a line
    return out + util::fmt("; %.0f%% in all", total * 100.0 / wall_ns);
}

size_t where(char* out, size_t capacity) {
    if (capacity == 0) return 0;
    size_t used = 0;
    auto add = [&](const char* text) {
        const size_t n = std::min(std::strlen(text), capacity - 1 - used);
        std::memcpy(out + used, text, n);
        used += n;
        out[used] = 0;
    };
    out[0] = 0;
    std::unique_lock<std::mutex> lock(g_m, std::try_to_lock);
    if (!lock.owns_lock()) {
        add("  (the thread list is in use)\n");
        return used;
    }
    for (auto& [id, t] : g_threads) {
        char line[200];
        const size_t n = platform::thread_where(t.handle, line, sizeof(line) - 1);
        if (n == 0) continue;
        line[n] = 0;
        add("  ");
        add(t.name);
        add(" ");
        add(line);
        add("\n");
    }
    return used;
}

std::vector<std::string> clock_debug() {
    std::vector<std::string> out;
    std::string now = platform::clock_debug();
    if (now.empty()) return out;
    out.push_back(now);
    std::vector<std::string> seen;
    std::lock_guard<std::mutex> lock(g_m);
    for (auto& [id, t] : g_threads) {
        if (std::find(seen.begin(), seen.end(), t.name) != seen.end()) continue;
        seen.push_back(t.name);
        out.push_back(util::fmt("%s: %s", t.name, platform::thread_clock_debug(t.handle).c_str()));
    }
    return out;
}

}  // namespace cpu
