#include <string>

#include "input/input_actions.hpp"
#include "utils/app_state.hpp"
#include "utils/utils.hpp"
#include "utils/media_info.hpp"
#include "player/audio_player.hpp"
#include "ui/widgets/widget_player_hud.hpp"
#include "ui/widgets/widget_video_controls.hpp"

#include "ui/scenes/scene_audio_player.hpp"

// YouTube-style control state for audio player
static video_controls_state controls_state;

void scene_audio_player_init(std::string full_path) {
	audio_player_init(full_path.c_str());
    audio_player_play(true);
    
    // Initialize YouTube-style controls
    widget_video_controls_init(&controls_state);
}

void scene_audio_player_render(struct nk_context *ctx) {
    if (media_info_get()->total_audio_playback_time == 0) media_info_get()->total_audio_playback_time = audio_player_get_total_play_time();
    media_info_get()->current_audio_playback_time = audio_player_get_current_play_time();
    
    // Update control animations and auto-hide
    widget_video_controls_update(&controls_state);
    
    // Render YouTube-style controls overlay
    widget_video_controls_render(ctx, &controls_state, media_info_get());
}

void scene_audio_player_input(InputState& input) {
    // Show controls on any input
    if (input.pressed || input.touch.touched) {
        widget_video_controls_show(&controls_state);
    }
    
    if (input_pressed(input, BTN_A)) {
        audio_player_play(!audio_player_get_audio_play_state());
        widget_video_controls_show_center_indicator(&controls_state);
    } else if (input_pressed(input, BTN_B)) {
        audio_player_cleanup();
        app_state_set(STATE_MENU_AUDIO_FILES);
        scan_directory(MEDIA_PATH_AUDIO);
    } else if (input_pressed(input, BTN_LEFT)) {
        audio_player_seek(-5.0f);
    } else if (input_pressed(input, BTN_RIGHT)) {
        audio_player_seek(5.0f);
    }
}

void scene_audio_player_shutdown() {
	audio_player_cleanup();
}
