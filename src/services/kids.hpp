// What a kids profile may watch: the YouTube channels a grown-up allows. No search, no recommendations, no
// comments: only these channels' videos. One list for every kids profile on the console.
#pragma once

#include <string>
#include <vector>

namespace kids {

struct Channel {
    std::string id, name, avatar;
};

std::vector<Channel> channels();
bool allowed(const std::string& channel_id);
constexpr size_t MAX_CHANNELS = 60;
// false when the list is full; adding one that's there only refreshes its name and picture.
bool add(const Channel& c);
void remove(const std::string& channel_id);

// Well-known children's channels of established publishers, as offered by "Add suggested channels". Their IDs
// were checked against the channels' own pages and feeds; a grown-up still decides, and the list isn't a promise
// that everything they post suits every child.
const std::vector<Channel>& suggested();
// Adds the suggested ones; returns how many were new.
int add_suggested();

// Podcasts and radio stations, the same way: a feed or a stream is allowed or it isn't.
struct Podcast {
    std::string feed_url, name, artwork;
};
struct Station {
    std::string url, name;
};
constexpr size_t MAX_AUDIO = 40;  // each of podcasts and stations
std::vector<Podcast> podcasts();
bool podcast_allowed(const std::string& feed_url);
bool add_podcast(const Podcast& p);
void remove_podcast(const std::string& feed_url);
std::vector<Station> stations();
bool station_allowed(const std::string& url);
bool add_station(const Station& s);
void remove_station(const std::string& url);
// Children's podcasts (kept apart from the others: their feeds are public and marked clean) and radio streams, as offered
// next to the suggested channels. Same promise: a grown-up decides.
const std::vector<Podcast>& suggested_podcasts();
const std::vector<Station>& suggested_stations();
int add_suggested_audio();  // how many were new
// The first time a kids profile is made (or opened, if it was made before this): the suggested channels, podcasts and
// radio are added, so there's something to watch at once. After that the lists are the grown-ups': taking one off stays.
// Returns how many were added (0 when it had been done already).
int seed_defaults_once();

}  // namespace kids
