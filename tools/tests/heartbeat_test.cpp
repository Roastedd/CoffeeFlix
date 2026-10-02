// The heartbeat: a line a second while armed, none after, and a new window starts again; or a
// slow line all the time once kept.
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "core/heartbeat.hpp"
#include "logger/logger.hpp"

static std::string source() { return "downloads: fixture"; }

static std::string log_so_far(const std::string& dir) {
    std::ifstream in(dir + "/coffeeflix.log");
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}

// Everything logged so far is in the file (the writer takes a line off the queue before it writes it).
static void settle() {
    while (log_pending()) usleep(10 * 1000);
    usleep(150 * 1000);
}

static int beats(const std::string& text) {
    int n = 0;
    for (size_t at = 0; (at = text.find("[Beat]", at)) != std::string::npos; at++) n++;
    return n;
}

int main() {
    char dir[] = "/tmp/heartbeat-test-XXXXXX";
    assert(mkdtemp(dir));
    log_to_file(dir);

    // Silent until armed, whatever the main loop does.
    for (int i = 0; i < 5; i++) {
        heartbeat::phase_done("screen");
        heartbeat::frame_done();
        usleep(100 * 1000);
    }
    usleep(1200 * 1000);
    settle();
    assert(beats(log_so_far(dir)) == 0);

    // A window of 2.5 s: beats at 0, 1 and 2 s, frames going on at 10 a second.
    heartbeat::arm(2.5, source);
    for (int i = 0; i < 30; i++) {
        heartbeat::phase_done("screen");
        heartbeat::frame_done();
        usleep(100 * 1000);
    }
    settle();
    std::string text = log_so_far(dir);
    const int first_window = beats(text);
    std::printf("first window: %d beats\n", first_window);
    assert(first_window >= 2 && first_window <= 3);
    assert(text.find("downloads: fixture") != std::string::npos);
    assert(text.find("its screen phase ended") != std::string::npos);
    assert(text.find("log lines waiting") != std::string::npos);
    // The first line has no second to count; a later one counts the frames (about 10).
    assert(text.find(" in the last second)") != std::string::npos);

    // Quiet after the window (1.5 s more), then a new window beats again.
    usleep(1500 * 1000);
    settle();
    assert(beats(log_so_far(dir)) == first_window);
    heartbeat::arm(1.2, source);
    usleep(1500 * 1000);
    settle();
    const int second_window = beats(log_so_far(dir));
    std::printf("second window: %d beats in all\n", second_window);
    assert(second_window > first_window);

    // Kept: a line every 2 s whatever happens, a second apart while a window is armed, and none after
    // it is switched off.
    const int before_kept = beats(log_so_far(dir));
    heartbeat::keep(2, source);
    for (int i = 0; i < 40; i++) {
        heartbeat::phase_done("player");
        heartbeat::frame_done();
        usleep(100 * 1000);
    }
    settle();
    text = log_so_far(dir);
    const int kept = beats(text) - before_kept;
    std::printf("kept for 4 s: %d beats\n", kept);
    assert(kept >= 2 && kept <= 4);  // 0, 2 and 4 s
    assert(text.find(" in the last 2 s)") != std::string::npos);
    heartbeat::arm(2.2, source);
    const int before_window = beats(log_so_far(dir));
    usleep(2500 * 1000);
    settle();
    const int in_window = beats(log_so_far(dir)) - before_window;
    std::printf("armed over it: %d beats in 2.5 s\n", in_window);
    assert(in_window >= 3);  // a second apart, not 2
    heartbeat::keep(0);
    usleep(300 * 1000);
    settle();
    const int stopped = beats(log_so_far(dir));
    usleep(2200 * 1000);
    settle();
    assert(beats(log_so_far(dir)) == stopped);

    log_shutdown();
    std::puts("Heartbeat test passed");
    return 0;
}
