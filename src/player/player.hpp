// Media engine shared by every source: local files, YouTube (separate
// video/audio streams), Jellyfin, Twitch/HLS, internet radio and podcasts.
//
// Threads per session: one demuxer per input, one video decoder (+1 helper for
// color conversion), one audio decoder. The audio output clock is the master;
// video frames are shown when their timestamp comes due.
#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
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
    bool chunked_http = false;   // download http(s) files through libcurl in ranged chunks (YouTube, media servers)
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
    // What the player shows under the title (YouTube): "1.2M views" and when it was posted, as the
    // feed had them ("3 weeks ago") until `resolve` knows better ("September 14, 2026").
    std::string views, posted;

    // Qualities the viewer can switch between in the player (empty: no choice): picture heights,
    // or heights with a frame rate (quality_height()). `resolve` reads `quality`, and may set both
    // to what the video has: `quality` to the one of `qualities` it played (`hfr` false: the video
    // has no 60 fps streams, so its qualities' names leave the frame rate out). `quality_setting`
    // is the store key that keeps the choice.
    std::vector<int> qualities;
    int quality = 0;
    bool hfr = true;
    std::string quality_setting;
    // "Auto" is offered too, as quality 0: `resolve` picks the best stream the downloads carry
    // (auto_budget(); YouTube: 720p60 at least), or the best within `bitrate_cap` (bits/s) once
    // the player saw the network fall behind it (auto_cap()).
    bool auto_quality = false;
    int bitrate_cap = 0;
    // Set by `resolve`: bits/s of the chosen video + audio, and of the lightest choice there was
    // (0: unknown; for a file the player then goes by FFmpeg's reckoning). Auto steps down only
    // while there is something lighter.
    int bitrate = 0, min_bitrate = 0;

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
    std::string language = {};
};

void init();
// Closes the session and waits (at most 3 s) for it to finish closing. When that doesn't happen,
// what the stuck close-down may still use is left alone: the process is about to end anyway.
void shutdown();
// Waits until sessions that were closed have finished letting go of their decoders and downloads,
// for at most `milliseconds`. False when one still hasn't.
bool wait_closed(int milliseconds);

void open(const Source& src);
// Queue of sources (album, podcast episodes): plays `index`, advances on end.
void open_queue(std::vector<Source> queue, int index);
void close();
void retry();               // reopen the current source from scratch (re-resolving URLs)
// The app is leaving the front (HOME, sleep): a video that is open is closed, decoder, video memory
// and downloads with it, and remembered where it was; nothing of it may be held while the system
// takes the screen. True when there was one. `resume_video` opens it again at the same second,
// paused when it was.
bool suspend_video();
bool resume_video();
void set_quality(int quality);  // reopen at the same position with another of source().qualities (0: Auto)
// A quality with a frame rate is the picture height × 100 plus the most pictures a second to show:
// 108060 is 1080p at 60 (on the Wii U as many as its decoder shows: about 45-48), 108030 1080p at
// 30 (every other picture of a 60 fps stream, evenly), 72060 and 72030 720p at 60 and 30. A plain height
// (and 0, Auto) shows all the stream has, as far as max_fps(height) lets it pick 60 fps streams.
inline int quality_height(int quality) { return quality >= 10000 ? quality / 100 : quality; }
inline int quality_fps(int quality) { return quality >= 10000 ? quality % 100 : 0; }
// "720p 60 fps", "480p"; `hfr` false (a video without 60 fps streams): "720p".
std::string quality_label(int quality, bool hfr = true);

// Bits per second Auto streams (video + audio) stay under for a while after Auto stepped down for
// a slow connection (0: none): the next videos don't run out first too. Auto otherwise starts at
// the best it has.
int auto_cap();
// Bits per second Auto picks streams within (video + audio): 80% of what the downloads got lately
// (kept between runs), or before any were measured, of what a Wii U's Wi-Fi usually gets. YouTube
// goes by it above 720p, Twitch and Jellyfin for all their streams. 0: no limit.
int auto_budget();
// The viewer chose a video_decoding setting: forget the safer level the player fell back to.
void reset_decoding_fallback();
// The safer decoding level (0-2) the hardware decoder's watchdog fell back to for `video` (the
// original Source, as it was opened), 0 for any other video than the one it was noted for.
int decoding_floor(const Source& video);
void note_decoding_floor(const Source& video, int level);
// Highest frame rate to ask a service for at a picture height: 60 fps, but on the Wii U only up to
// 720p. Its hardware decoder manages about 50 1080p pictures a second: 1080p60 only for a quality
// with a frame rate (1080p 60 fps shows about 45-48 of them, leaving out unreferenced ones evenly;
// 1080p 30 fps every other one) or Auto, and not in a video where even that was too much for it
// (the player steps down to 720p60 then).
float max_fps(int height);
float max_fps(int height, int quality);
bool next();
bool previous();
bool has_next();
bool has_previous();

void set_paused(bool paused);
void toggle_pause();
// Playback speed, 0.5 to 2 (the sound keeps its pitch). Kept for the next videos until the app
// closes; live streams always play at 1.
void set_speed(float speed);
float speed();
void seek(double seconds);
void seek_relative(double delta);

State state();
bool active();              // opening/buffering/playing/paused
bool started();             // the current source got going (it may be paused or buffering since)
std::string error();
Source source();            // snapshot (thread-safe)
double position();
double duration();
double buffered_until();    // media time buffered ahead (for the seek bar)
bool seekable();
bool live();
float buffering_progress(); // 0..1 while buffering/opening
bool has_video();
bool audio_only();
int video_width();          // as coded (stored), before rotation and pixel shape
int video_height();
// Quarter turns clockwise the picture is drawn turned by (0-3): phone videos filmed upright are
// often stored sideways. Draw video_texture() with gfx::image_rotated() into fit_rect().
int video_rotation();
float video_fps();          // 0 when unknown
// "H.264 · 1920×1080 · HW · AAC · 44 kHz"; `fps`: with the frame rate, and when the decoder
// leaves out pictures, how many of them it shows ("45/60 fps").
std::string codec_info(bool fps = false);
// What the developer's stats panel shows (0: not known). Rates in KB/s.
struct Stats {
    float download = 0;      // all downloads, over the last couple of seconds
    float average = 0;       // this video's downloads, smoothed (what Auto quality goes by)
    float stream = 0;        // what the stream takes on average, video and sound
    float video_ahead = 0;   // seconds of video in ahead of the picture: demuxed and downloaded
    float audio_ahead = 0;   // ...of sound
    int connections = 0;     // the video's downloads at once
    int requests = 0, new_connections = 0, gone_quiet = 0, failed = 0;  // since it opened
    int damaged_packets = 0; // refused before any codec receives them
    int waits = 0;           // times it ran out while playing and waited
    float waited = 0;        // seconds, in all
    // Of the last 10 s of playing.
    float shown_fps = 0, decode_ms = 0;
    int left_out = 0, dropped = 0, late = 0;
};
bool stats(Stats& out);  // false while nothing plays
std::string stream_title(); // ICY "now playing" for radio
std::string artwork_key();  // embedded cover art (images::get key) or ""

std::string language_code(const std::string& value);
std::vector<Track> audio_tracks();
int audio_track();
void set_audio_track(int index);
std::vector<Track> subtitle_tracks();
int subtitle_track();       // -1 = off
void set_subtitle_track(int index);
std::string subtitle_text();
// Label of a segment that was just skipped (once), for a toast.
std::string take_skip_notice();
// Auto just stepped the quality down (once), for a toast.
std::string take_quality_notice();

// Main thread, once per frame: advances the state machine and uploads the
// frame that is due. Returns the texture to draw (may be null).
void update();
SDL_Texture* video_texture();
// Destination rect for the current frame letterboxed into `area`, in the shape it is shown at
// (turned by video_rotation(), anamorphic pixels made square).
SDL_Rect fit_rect(int area_w, int area_h);

}  // namespace player
