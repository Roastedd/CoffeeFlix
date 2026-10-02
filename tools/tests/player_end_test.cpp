// Exercise the real demuxers, decoders, mixer and player update loop with short fixtures.
#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include "audio/mixer.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "gfx/gfx.hpp"
#include "player/player.hpp"

static void tick() {
    SDL_PumpEvents();
    player::update();
    SDL_Delay(5);
}
static void until(const std::function<bool()>& done, int timeout = 8000) {
    const auto start = SDL_GetTicks();
    while (!done() && SDL_GetTicks() - start < (unsigned)timeout) {
        tick();
        if (player::state() == player::FAILED) {
            std::cerr << player::error() << std::endl;
            std::abort();
        }
    }
    assert(done());
}
static void stable_end(int& finished, int expected) {
    assert(player::state() == player::ENDED);
    assert(!player::active() && audio::stream_paused());
    const double end = player::position();
    assert(std::abs(end - player::duration()) < 0.001);
    auto texture = player::video_texture();
    for (int i = 0; i < 120; i++) {
        tick();
        assert(player::state() == player::ENDED);
        assert(player::position() == end);
        assert(player::buffered_until() <= player::duration());
        assert(player::video_texture() == texture);
        assert(finished == expected);
    }
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string folder = argv[1];
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0);
    auto window = SDL_CreateWindow("Playback end regression", 0, 0, 640, 360, SDL_WINDOW_HIDDEN);
    assert(window);
    auto renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    assert(renderer);
    gfx::init(renderer);
    store::load(folder + "/store.json");
    tasks::init();
    assert(audio::init());
    player::init();
    for (const auto* name : {"av.mp4", "silent.mp4", "audio.m4a"}) {
        int finished = 0, stopped = 0;
        player::Source src;
        src.url = folder + "/" + name;
        src.title = name;
        src.service = "end-test";
        src.id = name;
        src.on_stop = [&](double pos, bool end) {
            if (end) { finished++; assert(std::abs(pos - player::duration()) < 0.001); }
            else stopped++;
        };
        player::open(src);
        until([] { return player::state() == player::PLAYING; });
        player::set_paused(true);
        const auto paused = player::position();
        for (int i = 0; i < 20; i++) tick();
        assert(player::state() == player::PAUSED);
        assert(std::abs(player::position() - paused) < 0.05);
        player::set_paused(false);
        until([] { return player::state() == player::ENDED; });
        stable_end(finished, 1);
        assert(store::resume_position(src.service, src.id) == 0);
        player::toggle_pause();
        until([] { return player::state() == player::PLAYING; });
        assert(player::position() < 0.5);
        until([] { return player::state() == player::ENDED; });
        stable_end(finished, 2);
        player::seek(0.5);
        until([] { return player::state() == player::PLAYING; });
        assert(player::position() < player::duration());
        until([] { return player::state() == player::ENDED; });
        stable_end(finished, 3);
        player::close();
        assert(stopped == 0 && finished == 3);
        std::cout << "PASS natural end, frozen position, pause, replay, seek, callback: " << name << std::endl;
    }
    int first = 0, second = 0;
    player::Source a, b;
    a.url = b.url = folder + "/av.mp4";
    a.title = "Queue first"; b.title = "Queue second";
    a.on_stop = [&](double, bool end) { if (end) first++; };
    b.on_stop = [&](double, bool end) { if (end) second++; };
    player::open_queue({a, b}, 0);
    until([&] { return first == 1 && player::source().title == b.title; });
    until([] { return player::state() == player::ENDED; });
    stable_end(second, 1);
    assert(first == 1);
    std::cout << "PASS queue advances once, last item stays ended" << std::endl;
    // HOME or sleep: the video is closed, the queue stays, and it opens again at the same second
    // (paused when it was paused).
    assert(!player::suspend_video() && !player::resume_video());
    first = second = 0;
    player::open_queue({a, b}, 0);
    until([] { return player::state() == player::PLAYING && player::position() > 1.2; });
    assert(player::suspend_video());
    assert(player::wait_closed(3000));
    assert(player::state() == player::IDLE && !player::active() && !player::has_video());
    assert(player::has_next());
    assert(player::resume_video() && !player::resume_video());
    until([] { return player::state() == player::PLAYING; });
    assert(player::position() > 0.9 && player::has_next());  // from the keyframe at or before 1.2 s; a start under a second is not jumped to
    player::set_paused(true);
    for (int i = 0; i < 20; i++) tick();
    assert(player::state() == player::PAUSED);
    const double held = player::position();
    assert(player::suspend_video());
    assert(player::wait_closed(3000));
    assert(player::state() == player::IDLE);
    assert(player::resume_video());
    until([] { return player::state() == player::PAUSED; });
    for (int i = 0; i < 40; i++) tick();
    assert(player::state() == player::PAUSED && std::abs(player::position() - held) < 0.5);
    player::close();
    std::cout << "PASS leaving the foreground: video closed, queue kept, reopened at the same second, paused stays paused" << std::endl;
    player::shutdown();
    tasks::shutdown();
    audio::shutdown();
    gfx::shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
