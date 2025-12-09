#include <string>
#include <atomic>

#include "input/input_actions.hpp"
#include "utils/app_state.hpp"
#include "utils/utils.hpp"
#include "utils/media_info.hpp"
#include "player/audio_player.hpp"
#include "player/video_player.hpp"
#include "ui/widgets/widget_player_hud.hpp"
#include "ui/widgets/widget_video_controls.hpp"

#include "ui/scenes/scene_video_player.hpp"

// YouTube-style control state
static video_controls_state controls_state;

void scene_video_player_init(std::string full_path) {
	video_player_init(full_path.c_str());
    video_player_play(true);
    
    // Initialize YouTube-style controls
    widget_video_controls_init(&controls_state);
}

void scene_video_player_render(struct nk_context *ctx) {
    video_player_update();
    
    // Update control animations and auto-hide
    widget_video_controls_update(&controls_state);
    
    // Render YouTube-style controls overlay
    widget_video_controls_render(ctx, &controls_state, media_info_get());
}

void scene_video_player_input(InputState& input) {
    // Show controls on any input
    if (input.pressed || input.touch.touched) {
        widget_video_controls_show(&controls_state);
    }
    
    if (input_pressed(input, BTN_A)) {
        video_player_play(!media_info_get()->playback_status);
        widget_video_controls_show_center_indicator(&controls_state);
    } else if (input_pressed(input, BTN_B)) {
        video_player_cleanup();
        app_state_set(STATE_MENU_VIDEO_FILES);
        scan_directory(MEDIA_PATH_VIDEO);
    } else if (input_pressed(input, BTN_LEFT)) {
        // Seek backward 5 seconds
        double current_time = media_info_get()->current_video_playback_time;
        double new_time = std::max(0.0, current_time - 5.0);
        video_player_seek(new_time);
        audio_player_seek(-5.0f);
    } else if (input_pressed(input, BTN_RIGHT)) {
        // Seek forward 5 seconds
        double current_time = media_info_get()->current_video_playback_time;
        double total_time = media_info_get()->total_video_playback_time;
        double new_time = std::min((double)total_time, current_time + 5.0);
        video_player_seek(new_time);
        audio_player_seek(5.0f);
    } else if (input_pressed(input, BTN_X)) {
        if (media_info_get()->total_audio_track_count == 1) return;

        video_player_play(false);
        audio_player_play(false);

        media_info_get()->current_audio_track_id++;
        if (media_info_get()->current_audio_track_id > media_info_get()->total_audio_track_count)
            media_info_get()->current_audio_track_id = 1;

        audio_player_switch_audio_stream(media_info_get()->current_audio_track_id);

        video_player_play(true);
        audio_player_play(true);
    }
}

void scene_video_player_shutdown() {
	video_player_cleanup();
}
