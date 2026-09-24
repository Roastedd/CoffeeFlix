// Factories for every screen, so screens don't need each other's headers.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/app.hpp"

namespace screens {

std::unique_ptr<app::Screen> make_section_root(app::Section s);

std::unique_ptr<app::Screen> make_home();
std::unique_ptr<app::Screen> make_search();
std::unique_ptr<app::Screen> make_youtube();
std::unique_ptr<app::Screen> make_jellyfin();
std::unique_ptr<app::Screen> make_twitch();
std::unique_ptr<app::Screen> make_radio();
std::unique_ptr<app::Screen> make_podcasts();
std::unique_ptr<app::Screen> make_media();
std::unique_ptr<app::Screen> make_settings();

}  // namespace screens

namespace player { struct Source; }

namespace screens {

// Launch helpers (push the right player screen).
void play_video(const player::Source& src);
void play_audio(const player::Source& src, bool show_now_playing = true);
void play_audio_queue(std::vector<player::Source> queue, int index, bool show_now_playing = true);
void open_now_playing();
bool now_playing_on_top();

std::unique_ptr<app::Screen> make_photo_viewer(std::vector<std::string> paths, int index);

}  // namespace screens
