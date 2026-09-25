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

// A stretch the player jumps over by itself (SponsorBlock).
struct SkipSegment {
    double start = 0, end = 0;
    std::string label;  // "Skipped sponsor"
};

struct Source {
    std::string url;             // video+audio, or audio only
    std::string audio_url;       // optional separate audio stream (YouTube DASH)
    std::vector<std::pair<std::string, std::string>> headers;
    std::string user_agent;
    bool chunked_http = false;   // download http(s) files through libcurl in ranged chunks (YouTube)
    double start = 0;            // seconds
    bool live = false;           // no seeking, unknown duration
    std::vector<std::pair<std::string, std::string>> external_subs;  // {label, url or path}
    bool subs_auto = true;       // turn the first external track on (with the "on by default" setting)
    std::vector<SkipSegment> skip_segments;

    // UI / bookkeeping
    std::string title, subtitle, artwork;
    std::string service, id;     // resume key ("youtube", "<videoId>")
    std::string extra;           // service data stored with the resume point
    bool remember_position = true;
    std::string channel_id;      // YouTube uploader, for the player's Subscribe button

    // Picture heights the viewer can switch between in the player (empty: no choice). `resolve`
    // reads `quality`; `quality_setting` is the store key that keeps the choice.
    std::vector<int> qualities;
    int quality = 0;
    std::string quality_setting;

    // Audio languages the service offers as separate streams (YouTube dubs), {id, label},
    // switched by reopening. `resolve` reads `audio_language` ("" = the original), sets it to
    // the one it picked and fills the list.
    std::vector<std::pair<std::string, std::string>> audio_languages;
    std::string audio_language;

    // Optional: runs on the player's opener thread before anything is opened,
    // to turn an id into stream URLs (YouTube, Jellyfin, Twitch). Return false
    // and set the error message to fail playback.
    std::function<bool(Source& src, std::string& error)> resolve;

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
void retry();               // reopen the current source from scratch (re-resolving URLs)
void set_quality(int height);  // reopen at the same position with another of source().qualities
// The viewer chose a video_decoding setting: forget the safer level the player fell back to.
void reset_decoding_fallback();
// Highest frame rate to ask a service for at a picture height: 60 fps with the "Allow 60 fps"
// setting, but on the Wii U only up to 720p (its hardware decoder manages about 50 1080p pictures
// a second).
float max_fps(int height);
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
Source source();            // snapshot (thread-safe)
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
// Label of a segment that was just skipped (once), for a toast.
std::string take_skip_notice();

// Main thread, once per frame: advances the state machine and uploads the
// frame that is due. Returns the texture to draw (may be null).
void update();
SDL_Texture* video_texture();
// Destination rect for the current frame letterboxed into `area`.
SDL_Rect fit_rect(int area_w, int area_h);

}  // namespace player
