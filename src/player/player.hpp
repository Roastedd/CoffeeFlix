// Media engine shared by every source: local files, YouTube (separate
// video/audio streams), Jellyfin, Twitch/HLS, internet radio and podcasts.
//
// Threads per session: one demuxer per input, one video decoder (+1 helper for
// color conversion), one audio decoder. The audio output clock is the master;
// video frames are shown when their timestamp comes due.
#pragma once

#include <SDL2/SDL.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace player {

struct Source {
    std::string url;             // video+audio, or audio only
    std::string audio_url;       // optional separate audio stream (YouTube DASH)
    std::vector<std::pair<std::string, std::string>> headers;
    std::string user_agent;
    double start = 0;            // seconds
    bool live = false;           // no seeking, unknown duration
    std::vector<std::pair<std::string, std::string>> external_subs;  // {label, url or path}

    // UI / bookkeeping
    std::string title, subtitle, artwork;
    std::string service, id;     // resume key ("youtube", "<videoId>")
    std::string extra;           // service data stored with the resume point
    bool remember_position = true;

    // Service hooks (called on the main thread)
    std::function<void(double position, bool paused)> on_progress;  // ~every 10 s
    std::function<void(double position, bool finished)> on_stop;
};

enum State { IDLE, OPENING, BUFFERING, PLAYING, PAUSED, ENDED, FAILED };

struct Track {
    int index = -1;
    std::string label;
};

void init();
void shutdown();

void open(const Source& src);
// Queue of sources (album, podcast episodes): plays `index`, advances on end.
void open_queue(std::vector<Source> queue, int index);
void close();
bool next();
bool previous();
bool has_next();
bool has_previous();

void set_paused(bool paused);
void toggle_pause();
void seek(double seconds);
void seek_relative(double delta);

State state();
bool active();              // opening/buffering/playing/paused
const std::string& error();
const Source& source();
double position();
double duration();
double buffered_until();    // media time buffered ahead (for the seek bar)
bool seekable();
bool live();
float buffering_progress(); // 0..1 while buffering/opening
bool has_video();
bool audio_only();
int video_width();
int video_height();
std::string codec_info();
std::string stream_title(); // ICY "now playing" for radio
std::string artwork_key();  // embedded cover art (images::get key) or ""

std::vector<Track> audio_tracks();
int audio_track();
void set_audio_track(int index);
std::vector<Track> subtitle_tracks();
int subtitle_track();       // -1 = off
void set_subtitle_track(int index);
std::string subtitle_text();

// Main thread, once per frame: advances the state machine and uploads the
// frame that is due. Returns the texture to draw (may be null).
void update();
SDL_Texture* video_texture();
// Destination rect for the current frame letterboxed into `area`.
SDL_Rect fit_rect(int area_w, int area_h);

}  // namespace player
