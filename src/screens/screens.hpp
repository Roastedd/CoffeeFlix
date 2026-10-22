// Factories for every screen, so screens don't need each other's headers.
#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "app/app.hpp"

namespace podcasts { struct Show; }

namespace screens {

std::unique_ptr<app::Screen> make_section_root(app::Section s);

std::unique_ptr<app::Screen> make_home();
std::unique_ptr<app::Screen> make_search();
std::unique_ptr<app::Screen> make_youtube();
std::unique_ptr<app::Screen> make_jellyfin();
std::unique_ptr<app::Screen> make_navidrome();
std::unique_ptr<app::Screen> make_twitch();
std::unique_ptr<app::Screen> make_radio();
std::unique_ptr<app::Screen> make_podcasts();
std::unique_ptr<app::Screen> make_media();
std::unique_ptr<app::Screen> make_receive_files();
std::unique_ptr<app::Screen> make_settings();
// One page of Settings (1 General ... 7 Updates and about), for the screenshot tests.
std::unique_ptr<app::Screen> make_settings_page(int page);
// What a profile watched and listened to (the profile in use when empty): rounded, kept on this console.
std::unique_ptr<app::Screen> make_activity(const std::string& profile = "");
// A profile's birthday: the month and day (no year), and how it is celebrated. Kept on this console.
std::unique_ptr<app::Screen> make_birthday(const std::string& profile = "");
// Screen time for a kids profile (the grown-ups' page), and the screen a kids profile meets when it is up.
std::unique_ptr<app::Screen> make_screen_time(const std::string& profile);
std::unique_ptr<app::Screen> make_time_up(bool quiet, int quiet_ends);
bool time_up_active();
// The sleep timer: its menu (how long, or at the end of what plays), and the words for its button.
void show_sleep_timer_menu();
std::string sleep_timer_label();
// "Who's watching?": at the start, or from Settings to switch and manage profiles.
std::unique_ptr<app::Screen> make_profile_picker(bool at_start);
// A profile's round avatar (its colour and first letter), for the status bar.
void draw_profile_avatar(const std::string& profile_id, float cx, float cy, float r);
// A kids profile's Home (and YouTube): the channels a grown-up allowed and their newest videos.
std::unique_ptr<app::Screen> make_kids_home();
// A podcast's page (its episodes), opened from a feed.
std::unique_ptr<app::Screen> make_podcast_show(const podcasts::Show& s);
// The grown-ups' code (core/lock): asks for it, then runs `granted` once it's right and the screen has closed.
std::unique_ptr<app::Screen> make_lock_prompt(std::function<void()> granted);
// Sets or changes it: which kind, then twice over; `done(true)` when it was set.
std::unique_ptr<app::Screen> make_lock_setup(std::function<void(bool)> done);

}  // namespace screens

namespace youtube { struct Video; }
namespace player { struct Source; }
namespace jellyfin { struct Item; }

namespace screens {

// `play_related`: asked to play another video (from the GamePad remote's list of what comes next);
// the screen that made this one starts it once the frame is over.
std::unique_ptr<app::Screen> make_video_player(std::function<void()> choose_source = {},
                                               std::function<void(const youtube::Video&)> play_related = {});
// Launch helpers (push the right player screen).
void play_video(const player::Source& src);
// A Jellyfin video; an episode offers the next one when it ends.
void play_jellyfin_video(const jellyfin::Item& item, bool from_start = false);
void play_audio(const player::Source& src, bool show_now_playing = true);
void play_audio_queue(std::vector<player::Source> queue, int index, bool show_now_playing = true);
void play_video_queue(std::vector<player::Source> queue, int index);
void open_now_playing();
bool now_playing_on_top();

std::unique_ptr<app::Screen> make_photo_viewer(std::vector<std::string> paths, int index);
std::unique_ptr<app::Screen> make_youtube_search(const std::string& query);
std::unique_ptr<app::Screen> make_jellyfin_search(const std::string& query);
std::unique_ptr<app::Screen> make_navidrome_search(const std::string& query);
std::unique_ptr<app::Screen> make_radio_search(const std::string& query);
std::unique_ptr<app::Screen> make_podcast_search(const std::string& query);
std::unique_ptr<app::Screen> make_twitch_search(const std::string& query);
std::unique_ptr<app::Screen> make_jellyfin_item(const jellyfin::Item& item);
// The Jellyfin accounts menu: switch, add another, sign out (opens sign-in when there are none).
void jellyfin_account_menu();
// The Navidrome accounts menu: switch, add another, sign out (opens sign-in when there are none).
void navidrome_account_menu();
std::unique_ptr<app::Screen> make_podcast_show(const std::string& feed_url, const std::string& title,
                                               const std::string& author, const std::string& artwork);

}  // namespace screens
