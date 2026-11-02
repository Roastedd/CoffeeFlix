#include "core/speedtest.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

#include "core/http.hpp"
#include "core/util.hpp"

namespace speedtest {

namespace {

using Clock = std::chrono::steady_clock;

// 25 MB of nothing, from a server meant for this (Cloudflare's speed test, which refuses 100 MB and up with a 403).
// The test stops long before the end.
const char* INTERNET_URL = "https://speed.cloudflare.com/__down?bytes=25000000";
const char* INTERNET_PROBE_URL = "https://speed.cloudflare.com/__down?bytes=0";  // what Cloudflare's own test asks for latency

struct Pull {
    double dns_ms = 0, tcp_ms = 0, tls_ms = 0, wait_ms = 0;  // where the time to the first byte went
    double first_byte_ms = 0;
    double bytes = 0;    // after the first second of the connection
    double seconds = 0;  // the time those bytes took
    std::string error;
};

// One new connection, read for `seconds` after its first byte. The first second (TCP starting slowly, the
// server's own ramp) is not counted.
Pull pull(const std::string& url, double seconds, const std::atomic<bool>* cancel) {
    const double WARM_UP = 1.0;
    Pull out;
    http::Request req;
    req.url = url;
    req.timeout = 40;
    req.stall_seconds = 10;
    req.max_bytes = 200u << 20;
    req.cancel = cancel;
    req.big_buffers = true;
    req.fresh_connection = true;
    Clock::time_point first, counted_from, last, last_any;
    double all_bytes = 0;  // everything, for a transfer that ended before the warm-up did (a very fast link)
    bool got = false, counting = false;
    long refused = 0;  // the status of an answer that was not a success
    req.on_data = [&](const http::Response& r, const char* /*data*/, size_t size) {
        const auto now = Clock::now();
        if (r.status < 200 || r.status >= 300) {
            refused = r.status;
            return false;
        }
        if (!got) {
            got = true;
            first = now;
        }
        all_bytes += (double)size;
        last_any = now;
        const double age = std::chrono::duration<double>(now - first).count();
        if (!counting && age >= WARM_UP) {
            counting = true;
            counted_from = now;
        } else if (counting) {
            out.bytes += (double)size;
            last = now;
        }
        return age < seconds + WARM_UP && !(cancel && cancel->load());
    };
    http::Response resp = http::perform(req);
    out.first_byte_ms = resp.first_byte_seconds * 1000;
    {
        const double dns = resp.dns_seconds, conn = std::max(resp.connect_seconds, dns), tls = std::max(resp.tls_seconds, conn);
        out.dns_ms = dns * 1000;
        out.tcp_ms = (conn - dns) * 1000;
        out.tls_ms = (tls - conn) * 1000;
        out.wait_ms = std::max(0.0, resp.first_byte_seconds - tls) * 1000;
    }
    if (!got) {
        out.error = refused ? util::fmt("HTTP %ld", refused) : !resp.error.empty() ? resp.error : util::fmt("HTTP %ld", resp.status);
        return out;
    }
    if (counting) {
        out.seconds = std::chrono::duration<double>(last - counted_from).count();
    } else {
        out.bytes = all_bytes;
        out.seconds = std::chrono::duration<double>(last_any - first).count();
    }
    return out;
}

// Small requests on one open connection until told to stop: how long each took (the first one, which
// opens the connection, is not counted). On a link that queues, these wait behind the downloads.
std::vector<double> probe(const std::string& url, const std::atomic<bool>& stop, const std::atomic<bool>* cancel, size_t want = 0) {
    std::vector<double> ms;
    if (url.empty()) return ms;
    http::Connection conn;
    bool first = true;
    while (!stop && !(cancel && cancel->load())) {
        http::Request req;
        req.url = url;
        req.timeout = 8;
        req.connection = &conn;
        req.cancel = cancel;
        req.max_bytes = 4096;
        http::Response r = http::perform(req);
        if (!r.ok()) break;
        if (!first) ms.push_back(r.first_byte_seconds * 1000);
        first = false;
        if (want && ms.size() >= want) break;
        for (int i = 0; i < 4 && !stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return ms;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

}  // namespace

Leg measure(const std::string& name, const std::string& url, double seconds, const std::atomic<bool>* cancel,
            const std::vector<int>& counts, const std::string& probe_url) {
    Leg leg;
    leg.name = name;
    std::vector<double> first_bytes, later_tls;
    if (!probe_url.empty()) {  // idle: four small requests after the one that opens the connection, nothing else going on
        std::atomic<bool> stop{false};
        std::thread ender([&] {
            for (int i = 0; i < 150 && !(cancel && cancel->load()) && !stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));  // a slow first connection takes seconds
            stop = true;
        });
        leg.idle_ms = median(probe(probe_url, stop, cancel, 4));
        stop = true;
        ender.join();
    }
    for (int n : counts) {
        if (cancel && cancel->load()) break;
        if (!leg.steps.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(500));  // let the network core catch its breath
        std::vector<Pull> pulls(n);
        std::vector<std::thread> threads;
        // Started 200 ms apart, so that a link doesn't get a burst of handshakes at once.
        for (int i = 1; i < n; i++)
            threads.emplace_back([&, i] {
                std::this_thread::sleep_for(std::chrono::milliseconds(200 * i));
                pulls[i] = pull(url, seconds, cancel);
            });
        std::atomic<bool> probe_stop{false};
        std::vector<double> loaded;
        std::thread prober([&] { loaded = probe(probe_url, probe_stop, cancel); });
        pulls[0] = pull(url, seconds, cancel);  // this thread is one of them
        for (auto& t : threads) t.join();
        probe_stop = true;
        prober.join();
        if (leg.steps.empty()) {  // the first connection of all: nothing cached yet
            leg.dns_ms = pulls[0].dns_ms;
            leg.tcp_ms = pulls[0].tcp_ms;
            leg.tls_ms = pulls[0].tls_ms;
            leg.wait_ms = pulls[0].wait_ms;
        }
        for (int i = (leg.steps.empty() ? 1 : 0); i < n; i++)
            if (pulls[i].error.empty() && pulls[i].tls_ms > 0) later_tls.push_back(pulls[i].tls_ms);
        Step step;
        step.connections = n;
        step.loaded_ms = median(loaded);
        for (const Pull& p : pulls) {
            if (!p.error.empty()) {
                if (leg.error.empty()) leg.error = p.error;
                continue;
            }
            first_bytes.push_back(p.first_byte_ms);
            if (p.seconds > 0.02) step.kb += p.bytes / 1024 / p.seconds;
        }
        if (step.kb <= 0) {  // nothing came: the rest would not either
            if (leg.error.empty()) leg.error = "No data";
            break;
        }
        leg.steps.push_back(step);
    }
    if (!first_bytes.empty()) {
        leg.first_byte_ms = median(first_bytes);
    }
    leg.later_tls_ms = median(later_tls);
    for (const Step& s : leg.steps) leg.best_kb = std::max(leg.best_kb, s.kb);
    for (const Step& s : leg.steps)
        if (s.kb >= leg.best_kb * 0.9) {
            leg.best_connections = s.connections;
            leg.best_loaded_ms = s.loaded_ms;
            break;
        }
    leg.ok = !leg.steps.empty();
    if (leg.ok) leg.error.clear();
    return leg;
}

Result run(const std::string& dev_server, const std::atomic<bool>* cancel) {
    Result r;
    if (!dev_server.empty()) r.legs.push_back(measure("Home network", "http://" + dev_server + "/coffeeflix.wuhb", 3, cancel, {1, 2, 3, 4, 6}, "http://" + dev_server + "/dev.json"));
    if (!(cancel && cancel->load())) r.legs.push_back(measure("Internet", INTERNET_URL, 3, cancel, {1, 2, 3, 4, 6}, INTERNET_PROBE_URL));
    return r;
}

}  // namespace speedtest
