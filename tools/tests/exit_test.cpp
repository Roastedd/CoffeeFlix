// Closing the app while a video plays must not hang (on a Wii U it froze the console): the player's
// close-down is bounded even when a download never answers, HOME pauses the video without losing the
// place, and the watchdog ends a process whose clean-up is stuck.
//
//   exit-test watchdog-stuck                arms the watchdog and hangs: the process must end by itself
//   exit-test watchdog-clean                arms it, disarms it and ends normally
//   exit-test crash-terminate <dir>         an exception nobody catches: writes the crash file, then aborts
//   exit-test crash-report <dir>            the next start: repeats the crash file in the log
//   exit-test http-cancel <base url>        a worker waits on a server that never answers: closing cancels it
//   exit-test log-long <dir>                a run that logs more than the file on the card holds
//   exit-test <healthy|stalled|paused> <base url> <data dir>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "app/crash_report.hpp"
#include "app/exit_watchdog.hpp"
#include "audio/mixer.hpp"
#include "core/http.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "gfx/gfx.hpp"
#include "logger/logger.hpp"
#include "player/player.hpp"

static void pump(unsigned ms) {
    const auto began = SDL_GetTicks();
    while (SDL_GetTicks() - began < ms) {
        SDL_PumpEvents();
        player::update();
        SDL_Delay(5);
    }
}

static bool pump_until(unsigned limit_ms, bool (*done)()) {
    const auto began = SDL_GetTicks();
    while (SDL_GetTicks() - began < limit_ms) {
        SDL_PumpEvents();
        player::update();
        if (done()) return true;
        SDL_Delay(5);
    }
    return false;
}

int main(int argc, char** argv) {
    assert(argc >= 2);
    const std::string mode = argv[1];

    if (mode == "watchdog-stuck") {
        exit_watchdog::arm(0.5);
        assert(exit_watchdog::armed());
        std::this_thread::sleep_for(std::chrono::seconds(60));  // a close-down that never ends
        std::cout << "FAIL the watchdog did not end the process" << std::endl;
        return 2;
    }
    if (mode == "watchdog-clean") {
        exit_watchdog::arm(30);
        exit_watchdog::arm(60);  // a later deadline never moves it back
        exit_watchdog::disarm();
        assert(!exit_watchdog::armed());
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::cout << "PASS a disarmed watchdog leaves the process alone" << std::endl;
        return 0;
    }

    if (mode == "crash-terminate") {
        assert(argc == 3);
        crash_report::install(argv[2]);
        std::thread([] { throw std::runtime_error("boom"); }).join();  // nobody catches this
        std::cout << "FAIL the process went on after an uncaught exception" << std::endl;
        return 2;
    }
    if (mode == "crash-report") {
        assert(argc == 3);
        crash_report::report_last_run(argv[2]);
        return 0;
    }

    if (mode == "log-long") {
        assert(argc == 3);
        log_to_file(argv[2]);
        for (int i = 0; i < 4000; i++) log_message(LOG_OK, "Test", "line %d %s", i, std::string(300, 'x').c_str());
        log_message(LOG_OK, "App", "Shutting down");
        log_shutdown();
        return 0;
    }
    if (mode == "http-cancel") {
        // Closing the app waits for the workers; one asking a server that holds the connection open would
        // keep it waiting for the whole timeout.
        assert(argc == 3);
        const std::string url = argv[2];
        http::init("content/cacert.pem");
        tasks::init();
        std::atomic<int> state{0};  // 1: asking, 2: cancelled, 3: it came out some other way
        tasks::submit(tasks::API, [&state, url]() -> std::function<void()> {
            state = 1;
            http::Request req;
            req.url = url + "/hang";
            req.timeout = 60;
            state = http::perform(req).error == "Cancelled" ? 2 : 3;
            return nullptr;
        });
        for (int i = 0; i < 500 && state != 1; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        assert(state == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // the request is out and waiting
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::seconds(15));
            std::cout << "FAIL closing waited for the server" << std::endl;
            std::_Exit(3);
        }).detach();
        const auto t0 = std::chrono::steady_clock::now();
        http::cancel_running();
        tasks::shutdown();
        const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        std::cerr << "http-cancel: the workers ended after " << ms << " ms" << std::endl;
        assert(state == 2);
        assert(ms < 3000);
        // A request made after the cancel (the last log lines going out) is not affected.
        const http::Response later = http::get(url + "/hello", {}, 10);
        assert(later.ok() && later.body == "hello");
        http::shutdown();
        std::cout << "PASS closing cancels a request a worker waits on (" << ms << " ms)" << std::endl;
        return 0;
    }

    assert(argc == 4);
    const std::string base = argv[2];
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0);
    auto window = SDL_CreateWindow("Closing while playing", 0, 0, 640, 360, SDL_WINDOW_HIDDEN);
    auto renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    assert(window && renderer);
    gfx::init(renderer);
    http::init("content/cacert.pem");
    store::load(std::string(argv[3]) + "/store.json");
    tasks::init();
    assert(audio::init());
    player::init();

    player::Source src;
    const std::string file = mode == "stalled" ? "/stall.mp4" : "/ok.mp4";
    src.url = base + file;
    src.title = mode;
    src.service = "exit";
    src.id = src.url;
    src.chunked_http = true;  // the way the Wii U reads a movie
    bool stopped = false;
    src.on_stop = [&](double, bool) { stopped = true; };
    player::open(src);
    assert(pump_until(20000, [] { return player::state() == player::PLAYING; }));
    // Long enough for the downloads to run ahead into the part the server never answers (stalled).
    pump(mode == "stalled" ? 6000 : 3000);
    std::cerr << mode << ": playing at " << player::position() << " s" << std::endl;

    if (mode == "paused") {
        // HOME: the video waits where it is.
        player::set_paused(true);
        pump(300);
        assert(player::state() == player::PAUSED);
        const double at = player::position();
        pump(1500);
        std::cerr << "paused: " << at << " -> " << player::position() << std::endl;
        assert(player::state() == player::PAUSED && player::position() - at < 0.2);
        std::cout << "PASS a video paused for HOME keeps its place" << std::endl;
    }

    // If closing hangs, this stands in for the console freezing.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(12));
        std::cout << "FAIL closing took more than 12 s" << std::endl;
        std::_Exit(3);
    }).detach();

    // What the app does when the console says close, then when it has left the loop.
    const auto t0 = std::chrono::steady_clock::now();  // not SDL_GetTicks: SDL_Quit() restarts that
    const auto since = [&] {
        return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    };
    player::close();
    const bool closed = player::wait_closed(2000);
    const long callback_ms = since();
    player::shutdown();
    tasks::shutdown();
    audio::shutdown();
    gfx::shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    http::shutdown();
    const long total_ms = since();
    std::cerr << mode << ": closed in the exit callback " << closed << " (" << callback_ms << " ms), all closed after " << total_ms
              << " ms" << std::endl;
    assert(stopped);
    assert(total_ms < 6000);
    std::cout << "PASS closing while playing (" << mode << ") took " << total_ms << " ms" << std::endl;
    return 0;
}
