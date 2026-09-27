#include "app/dev_log.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "app/updater.hpp"
#include "core/cpu.hpp"
#include "core/http.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "core/version.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace dev_log {

namespace {

constexpr size_t BATCH = 64 * 1024;

std::mutex g_m;
std::condition_variable g_cv;
std::string g_server;  // "ip:port", empty while there's nowhere to send to
bool g_changed = false, g_stop = false, g_done = false;
std::thread* g_thread = nullptr;  // never deleted: shutdown joins it
std::atomic<bool> g_cancel{false};
double g_next_look = 0;
std::string g_run;  // tells this run's lines from the last one's

void sender() {
    cpu::ThreadTag tag("dev log");
    platform::lower_thread_priority();
    uint64_t sent = 0;  // lines the computer has
    double wait = 0;
    std::string failing;  // the server it couldn't reach, so that's logged once
    std::unique_lock<std::mutex> lock(g_m);
    for (;;) {
        g_cv.wait_for(lock, std::chrono::duration<double>(wait), [] { return g_stop || g_changed; });
        g_changed = false;
        std::string server = g_server;
        bool stopping = g_stop;
        // Closing: a last batch, unless the computer wasn't answering anyway.
        if (server.empty() || (stopping && !failing.empty())) {
            if (stopping) break;
            wait = 3600;  // until tick() finds one
            continue;
        }
        lock.unlock();
        std::string body;
        uint64_t upto = log_lines_since(sent, body, BATCH);
        bool ok = true, full = body.size() >= BATCH / 2;
        if (!body.empty()) {
            http::Request r;
            r.method = "POST";
            r.url = "http://" + server + "/log";
            r.headers = {{"Content-Type", "text/plain; charset=utf-8"},
                         {"X-CoffeeFlix-Run", g_run},
                         {"X-CoffeeFlix-Version", APP_VERSION},
                         {"X-CoffeeFlix-Platform", platform::name()}};
            r.body = std::move(body);
            r.timeout = stopping ? 2 : 4;
            r.max_bytes = 4096;
            r.cancel = &g_cancel;
            http::Response res = http::perform(r);
            ok = res.ok();
            if (ok) {
                if (sent == 0 || !failing.empty()) log_message(LOG_OK, "DevLog", "Sending the log to %s", server.c_str());
                sent = upto;
                failing.clear();
            } else if (failing != server) {
                failing = server;
                log_message(LOG_WARNING, "DevLog", "Couldn't send the log to %s (%s), trying again later", server.c_str(),
                            res.error.c_str());
            }
        }
        lock.lock();
        if (stopping) break;
        // Once a second (right away when there's more), less and less often while it fails.
        wait = !ok ? std::clamp(wait * 2, 5.0, 60.0) : full ? 0 : 1;
    }
    g_done = true;
    g_cv.notify_all();
}

}  // namespace

void tick() {
    double now = util::now_seconds();
    if (now < g_next_look) return;
    g_next_look = now + 1;
    std::string server;
    if (updater::developer()) {
        // Found by the update check, which waits a bit after start-up: until then, last time's.
        server = updater::dev_server();
        if (server.empty()) server = util::env_or("COFFEEFLIX_DEV_SERVER", "");  // desktop tests
        if (server.empty()) server = store::get_str("update_dev_server");
    }
    std::lock_guard<std::mutex> lock(g_m);
    if (g_stop || server == g_server) return;
    g_server = server;
    g_changed = true;
    g_cv.notify_all();
    if (!g_thread) {
        g_run = util::random_hex(4);
        g_thread = new std::thread(sender);
    }
}

void shutdown() {
    std::unique_lock<std::mutex> lock(g_m);
    g_stop = true;
    if (!g_thread) return;
    g_cv.notify_all();
    // The last lines are worth waiting for, but not for long.
    if (!g_cv.wait_for(lock, std::chrono::seconds(2), [] { return g_done; })) g_cancel = true;
    lock.unlock();
    g_thread->join();
}

}  // namespace dev_log
