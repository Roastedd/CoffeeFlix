// The black box: a record every so often in a file of its own (the last three kept), with the
// main loop's counters, the log's progress and a line for each thread; a clean end removes the file,
// any other end leaves it for the next start to put in the log.
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "core/blackbox.hpp"
#include "core/cpu.hpp"
#include "core/heartbeat.hpp"
#include "logger/logger.hpp"

static std::string read(const std::string& path) {
    std::ifstream in(path);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

static bool exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

static int count(const std::string& text, const std::string& what) {
    int n = 0;
    for (size_t at = 0; (at = text.find(what, at)) != std::string::npos; at += what.size()) n++;
    return n;
}

int main() {
    char dir[] = "/tmp/blackbox-test-XXXXXX";
    assert(mkdtemp(dir));
    const std::string file = std::string(dir) + "/blackbox.txt", previous = std::string(dir) + "/blackbox-previous.txt";
    log_to_file(dir);
    cpu::ThreadTag tag("main");

    // Nothing from a run before.
    assert(blackbox::report_last_run(dir).empty());
    assert(!exists(file));

    heartbeat::phase_done("screen");
    heartbeat::frame_done();
    log_message(LOG_OK, "Test", "a line");

    // Off until asked (a thread isn't even started), then a record every 0.2 s.
    blackbox::keep(false, dir, 0.2);
    usleep(500 * 1000);
    assert(!exists(file));
    blackbox::keep(true, dir, 0.2);
    usleep(1500 * 1000);
    std::string text = read(file);
    std::printf("%s", text.c_str());
    assert(text.find("frame 1, last phase screen ended") != std::string::npos);
    assert(text.find("log: called") != std::string::npos);
    assert(text.find("  main ") != std::string::npos);  // the thread list
    // The last three records, oldest first (7 or so have been made).
    assert(count(text, "== ") == 3);
    double before = 0;
    for (size_t at = 0; (at = text.find("== ", at)) != std::string::npos; at += 3) {
        const double t = std::atof(text.c_str() + at + 3);
        assert(t > before);
        before = t;
    }

    // Stopped some other way than clean: the next start finds the file, puts it in the log, moves it.
    blackbox::keep(false, dir, 0.2);
    usleep(500 * 1000);
    std::string last = blackbox::report_last_run(dir);
    assert(last.find("last phase screen") != std::string::npos);
    assert(!exists(file) && exists(previous));
    assert(read(previous) == last);
    while (log_pending()) usleep(10 * 1000);
    usleep(150 * 1000);
    assert(count(read(std::string(dir) + "/coffeeflix.log"), "[Blackbox]") >= 4);
    assert(blackbox::report_last_run(dir).empty());  // and only once

    // A clean end takes the file away, even with the thread about to write another.
    blackbox::keep(true, dir, 0.2);
    usleep(500 * 1000);
    assert(exists(file));
    blackbox::stop();
    usleep(600 * 1000);
    assert(!exists(file));
    assert(blackbox::report_last_run(dir).empty());

    std::printf("PASS black box\n");
    return 0;
}
