// A measurement of what the connection gives CoffeeFlix: how long a server takes to answer, and
// how fast data arrives over one connection and over three at once. Run against the internet
// and, when a developer server is on the network, against that computer too: the difference
// says whether the Wi-Fi or the internet is the slow part.
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace speedtest {

struct Step {
    int connections = 0;
    double kb = 0;               // KB/s summed over the connections, once each had been going for a second
    double loaded_ms = 0;        // how long a small request took while they ran (the median); 0 if none got through
};

struct Leg {
    std::string name;            // "Internet", "Home network"
    bool ok = false;
    std::string error;
    double first_byte_ms = 0;    // time to the first byte of a new connection (TLS included), the median
    // The very first connection, where nothing was known yet, in its parts: finding the server's address, opening
    // the connection, the secure handshake (0 for plain http) and waiting for the first byte.
    double dns_ms = 0, tcp_ms = 0, tls_ms = 0, wait_ms = 0;
    double later_tls_ms = 0;     // the median secure handshake of the connections after the very first (a resumed session is faster)
    double idle_ms = 0;          // how long a small request takes on an open connection with nothing else going on
    std::vector<Step> steps;     // 1, 2, 3, 4 and 6 connections, one after the other
    // The fewest connections that get within a tenth of the most: more than that only loads the console's
    // network core, which has no room to spare.
    int best_connections = 0;
    double best_kb = 0;          // the most any step got
    double best_loaded_ms = 0;   // the small request's time during that step: far above idle_ms means the link is queueing
};

struct Result {
    std::vector<Leg> legs;
};

// Blocking, a few seconds per step: run it on a thread of its own. `dev_server` is
// "host:port" of a computer on the network that serves /coffeeflix.wuhb and /dev.json, or empty. `cancel` ends it early.
Result run(const std::string& dev_server, const std::atomic<bool>* cancel);

// The same measurement against any URL (tests): seconds is how long each step lasts, the first second of
// each connection left out (TCP is still speeding up then).
// `probe_url` is a small answer to ask for now and then (an empty string: no latency figures).
Leg measure(const std::string& name, const std::string& url, double seconds, const std::atomic<bool>* cancel,
            const std::vector<int>& counts = {1, 2, 3, 4, 6}, const std::string& probe_url = "");

}  // namespace speedtest
