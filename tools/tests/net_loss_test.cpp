// A download that breaks mid-file is not the end of the video: playback waits and asks again, and when the
// server stays away it ends as an error at the spot it reached, never as "Finished" (which would drop the
// resume point and go on to the next episode).
#include <cassert>
#include <cmath>
#include <iostream>
#include "audio/mixer.hpp"
#include "core/http.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "gfx/gfx.hpp"
#include "player/player.hpp"

struct Outcome { bool ended = false, failed = false; int finished = 0, stopped = 0, damaged = 0; double position = 0, duration = 0; std::string error; };
static Outcome play(const std::string& url, unsigned limit_ms, bool chunked = true, double start = 0) {
    Outcome o;
    player::Source src;
    src.url = url; src.title = url; src.service = "net-loss"; src.id = url; src.chunked_http = chunked;
    src.start = start;
    src.on_stop = [&](double, bool end) { if (end) o.finished++; else o.stopped++; };
    player::open(src);
    const auto began = SDL_GetTicks();
    while (SDL_GetTicks() - began < limit_ms) {
        SDL_PumpEvents(); player::update(); SDL_Delay(5);
        player::Stats stats; if(player::stats(stats))o.damaged=std::max(o.damaged,stats.damaged_packets);
        if (player::state() == player::ENDED) { o.ended = true; break; }
        if (player::state() == player::FAILED) { o.failed = true; o.error = player::error(); break; }
    }
    o.position = player::position(); o.duration = player::duration();
    player::close();
    return o;
}
int main(int argc, char** argv) {
    assert(argc == 3 || argc == 4);
    const std::string base = argv[1];
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) == 0);
    auto window = SDL_CreateWindow("Connection loss regression", 0, 0, 640, 360, SDL_WINDOW_HIDDEN);
    auto renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    assert(window && renderer);
    gfx::init(renderer);
    http::init("content/cacert.pem");
    store::load(std::string(argv[2]) + "/store.json");
    tasks::init(); assert(audio::init()); player::init();

    if (argc == 4) {
        auto episode = play(base + "/episode.mkv", 60000, true, 3.0);
        assert(episode.ended && !episode.failed && episode.finished == 1 && episode.damaged == 0);
        assert(episode.duration > 10 && std::abs(episode.position - episode.duration) < 0.5);
        std::cout << "PASS resumed 1080p H.264/E-AC-3 Matroska survives a 503 without rejecting healthy packets" << std::endl;
    } else {
    // The server is gone for a few seconds after the first part: playback stalls, asks again, and plays to the real end.
    auto flaky = play(base + "/flaky.mp4", 90000);
    std::cerr << "flaky: ended " << flaky.ended << " failed " << flaky.failed << " " << flaky.error << " at " << flaky.position << "/" << flaky.duration << std::endl;
    assert(flaky.ended && flaky.finished == 1 && flaky.duration > 10 && std::abs(flaky.position - flaky.duration) < 0.5);
    std::cout << "PASS a download outage is waited out and the video plays to its end" << std::endl;

    // The server stays away: an error where it stopped, not a finish.
    auto dead = play(base + "/dead.mp4", 120000);
    std::cerr << "dead: ended " << dead.ended << " failed " << dead.failed << " " << dead.error << " at " << dead.position << "/" << dead.duration << std::endl;
    assert(!dead.ended && dead.failed && dead.finished == 0);
    assert(dead.error.find("Connection lost") != std::string::npos);
    assert(dead.position > 1 && dead.position < dead.duration - 3);
    std::cout << "PASS a lost connection ends as an error at the spot reached, not as Finished" << std::endl;

    // FFmpeg's own HTTP reads (not http_io's downloads) end the same way when the connection closes early.
    auto cut = play(base + "/cut.mp4", 120000, false);
    std::cerr << "cut: ended " << cut.ended << " failed " << cut.failed << " " << cut.error << " at " << cut.position << "/" << cut.duration << std::endl;
    assert(!cut.ended && cut.failed && cut.finished == 0 && cut.error.find("Connection lost") != std::string::npos);
    assert(cut.position > 1 && cut.position < cut.duration - 3);
    std::cout << "PASS a connection that closes early is an error, not Finished (FFmpeg's own reads)" << std::endl;

    auto repair = play(base + "/repair.mp4", 65000);
    assert(repair.ended && !repair.failed && repair.finished==1 && repair.damaged>0);
    std::cout << "PASS a damaged H.264 packet is rejected, cached bytes discarded, and fresh data recovers" << std::endl;
    auto corrupt = play(base + "/badframe.mp4", 85000);
    assert(corrupt.failed && !corrupt.ended && corrupt.finished==0 && corrupt.damaged>0);
    std::cout << "PASS persistent damaged video fails safely without marking the episode finished" << std::endl;

    }
    player::shutdown(); tasks::shutdown(); audio::shutdown(); gfx::shutdown();
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); http::shutdown();
}
