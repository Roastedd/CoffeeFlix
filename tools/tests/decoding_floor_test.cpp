// The safer decoding level the hardware decoder's watchdog falls back to belongs to one video:
// the same video reopened keeps it, any other starts from the setting again. (It once stayed for
// the rest of the run, which left every later 1080p video in software at 3 pictures a second.)
#include <cassert>
#include <cstdio>

#include "player/player.hpp"

static player::Source video(const char* service, const char* id, const char* url) {
    player::Source s;
    s.service = service;
    s.id = id;
    s.url = url;
    return s;
}

int main() {
    const auto camp = video("movies", "tt35298123", "https://a.example/camp.mkv");
    const auto monster = video("movies", "tt13207736:S4E3", "https://b.example/monster.mkv");
    const auto camp_other_stream = video("movies", "tt35298123", "https://c.example/camp-720.mkv");

    assert(player::decoding_floor(camp) == 0);  // nothing noted yet

    player::note_decoding_floor(camp, 1);
    assert(player::decoding_floor(camp) == 1);
    assert(player::decoding_floor(monster) == 0);
    assert(player::decoding_floor(camp_other_stream) == 0);  // another stream of the title is another video

    player::note_decoding_floor(camp, 2);  // the next level for the same video
    assert(player::decoding_floor(camp) == 2);
    assert(player::decoding_floor(monster) == 0);

    player::note_decoding_floor(monster, 1);  // the latest video is the one it is for
    assert(player::decoding_floor(monster) == 1);
    assert(player::decoding_floor(camp) == 0);

    player::reset_decoding_fallback();  // the viewer chose a setting
    assert(player::decoding_floor(monster) == 0);

    std::puts("decoding floor: one video at a time: OK");
}
