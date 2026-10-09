// Factories for every screen, so screens don't need each other's headers.
#pragma once

#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "app/app.hpp"

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
std::unique_ptr<app::Screen> make_support();
// Counts an app start; asks on Home, once, whether to say thanks (support_screen.cpp).
void count_start();
void maybe_ask_for_support();
std::unique_ptr<app::Screen> make_settings();

}  // namespace screens

namespace player { struct Source; }
namespace jellyfin { struct Item; }

namespace screens {

std::unique_ptr<app::Screen> make_video_player();
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
