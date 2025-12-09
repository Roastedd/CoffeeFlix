#include "widget_video_controls.hpp"

#include <string>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "main.hpp"
#include "player/video_player.hpp"
#include "player/audio_player.hpp"
#include "player/subtitle.hpp"
#include "ui/apple_theme.hpp"
#include "utils/app_state.hpp"
#include "utils/media_info.hpp"
#include "utils/utils.hpp"
#include "utils/sdl.hpp"
#include "logger/logger.hpp"
#include "vendor/ui/nuklear.h"

// Icon textures and images
static bool icons_loaded = false;
struct IconEntry {
    const char* path;
    SDL_Texture* texture;
    struct nk_image image;
};

static IconEntry g_control_icons[] = {
    {"content/icons/pause_icon.png", nullptr, {}},
    {"content/icons/settings_icon.png", nullptr, {}},
};

enum ControlIcon {
    ICON_PAUSE = 0,
    ICON_SETTINGS = 1,
};

static void ensure_icons_loaded() {
    if (icons_loaded) return;
    SDL_Renderer* renderer = sdl_get()->sdl_renderer;
    if (!renderer) return;

    for (auto& entry : g_control_icons) {
        SDL_Surface* surface = IMG_Load(entry.path);
        if (!surface) {
            log_message(LOG_ERROR, "VideoControls", "Failed to load icon %s: %s", entry.path, IMG_GetError());
            continue;
        }
        entry.texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_SetTextureBlendMode(entry.texture, SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(surface);
        if (entry.texture) entry.image = nk_image_ptr(entry.texture);
    }
    icons_loaded = true;
}

// Helper to get current time in milliseconds
static uint64_t get_current_time_ms() {
    return SDL_GetTicks64();
}

void widget_video_controls_init(video_controls_state* state) {
    state->controls_visible = true;
    state->fade_alpha = 1.0f;
    state->last_input_time_ms = get_current_time_ms();
    state->last_update_time_ms = get_current_time_ms();
    state->scrubbing = false;
    state->volume_dragging = false;
    state->settings_open = false;
    state->show_center_indicator = false;
    state->center_indicator_time_ms = 0;
    state->last_playing_state = false;
    state->volume = 1.0f;  // Default to 100% volume
    state->playback_speed = 1.0f;  // Default to normal speed
    state->selected_audio_track = 0;
    state->selected_subtitle_track = -1;  // Off by default
    
    // Load icons
    ensure_icons_loaded();
}

void widget_video_controls_show(video_controls_state* state) {
    state->last_input_time_ms = get_current_time_ms();
    state->controls_visible = true;
}

void widget_video_controls_show_center_indicator(video_controls_state* state) {
    state->show_center_indicator = true;
    state->center_indicator_time_ms = get_current_time_ms();
}

void widget_video_controls_update(video_controls_state* state) {
    uint64_t current_time = get_current_time_ms();
    state->last_update_time_ms = current_time;
    
    // Check if controls should auto-hide
    if (state->controls_visible && !state->scrubbing && !state->volume_dragging && !state->settings_open) {
        if (current_time - state->last_input_time_ms > state->AUTO_HIDE_DELAY_MS) {
            state->controls_visible = false;
        }
    }
    
    // Update fade animation
    if (state->controls_visible && state->fade_alpha < 1.0f) {
        state->fade_alpha += state->FADE_SPEED;
        if (state->fade_alpha > 1.0f) state->fade_alpha = 1.0f;
    } else if (!state->controls_visible && state->fade_alpha > 0.0f) {
        state->fade_alpha -= state->FADE_SPEED;
        if (state->fade_alpha < 0.0f) state->fade_alpha = 0.0f;
    }
    
    // Update center indicator
    if (state->show_center_indicator) {
        if (current_time - state->center_indicator_time_ms > state->CENTER_INDICATOR_DURATION_MS) {
            state->show_center_indicator = false;
        }
    }
}

// Render the top info bar (title and back button)
static void render_top_bar(struct nk_context* ctx, video_controls_state* state, media_info* info, float alpha) {
    if (alpha <= 0.01f) return;
    
    const int bar_height = 60 * UI_SCALE;
    struct nk_rect bar_rect = nk_rect(0, 0, SCREEN_WIDTH, bar_height);
    
    // Set window background with alpha
    struct nk_color bg = AppleTheme::BG_ELEVATED;
    bg.a = (nk_byte)(bg.a * alpha * 0.95f);  // Slight transparency
    
    nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, bg);
    
    if (nk_begin(ctx, "TopBar", bar_rect, 
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_INPUT)) {
        
        nk_layout_row_begin(ctx, NK_DYNAMIC, bar_height - 10, 2);
        
        // Back button (left side - 15%)
        nk_layout_row_push(ctx, 0.15f);
        {
            struct nk_color btn_color = AppleTheme::SYSTEM_BLUE;
            btn_color.a = (nk_byte)(255 * alpha);
            nk_style_push_color(ctx, &ctx->style.button.normal.data.color, btn_color);
            
            if (nk_button_label(ctx, "< Back")) {
                // Exit player
                if (app_state_get() == STATE_PLAYING_VIDEO) {
                    video_player_cleanup();
                    app_state_set(STATE_MENU_VIDEO_FILES);
                } else if (app_state_get() == STATE_PLAYING_AUDIO) {
                    audio_player_cleanup();
                    app_state_set(STATE_MENU_AUDIO_FILES);
                }
            }
            nk_style_pop_color(ctx);
        }
        
        // Title (right side - 85%)
        nk_layout_row_push(ctx, 0.85f);
        {
            struct nk_color text_color = AppleTheme::LABEL_PRIMARY;
            text_color.a = (nk_byte)(255 * alpha);
            nk_style_push_color(ctx, &ctx->style.text.color, text_color);
            
            std::string title = !info->filename.empty() ? info->filename : "Media Player";
            nk_label(ctx, title.c_str(), NK_TEXT_LEFT);
            
            nk_style_pop_color(ctx);
        }
        
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
    
    nk_style_pop_color(ctx);
}

// Render the center play/pause indicator
static void render_center_indicator(struct nk_context* ctx, video_controls_state* state, bool is_playing) {
    if (!state->show_center_indicator) return;
    
    // Calculate fade based on time
    uint64_t elapsed = get_current_time_ms() - state->center_indicator_time_ms;
    float indicator_alpha = 1.0f - (float)elapsed / state->CENTER_INDICATOR_DURATION_MS;
    if (indicator_alpha < 0.0f) indicator_alpha = 0.0f;
    
    const int indicator_size = 120 * UI_SCALE;
    struct nk_rect indicator_rect = nk_rect(
        (SCREEN_WIDTH - indicator_size) / 2,
        (SCREEN_HEIGHT - indicator_size) / 2,
        indicator_size,
        indicator_size
    );
    
    struct nk_color bg = AppleTheme::BG_ELEVATED;
    bg.a = (nk_byte)(200 * indicator_alpha);
    
    nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, bg);
    
    if (nk_begin(ctx, "CenterIndicator", indicator_rect,
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND | NK_WINDOW_NO_INPUT)) {
        
        nk_layout_row_dynamic(ctx, indicator_size - 20, 1);
        
        struct nk_color text_color = AppleTheme::LABEL_PRIMARY;
        text_color.a = (nk_byte)(255 * indicator_alpha);
        nk_style_push_color(ctx, &ctx->style.text.color, text_color);
        
        // Large play/pause symbol
        nk_label(ctx, is_playing ? "▶" : "⏸", NK_TEXT_CENTERED);
        
        nk_style_pop_color(ctx);
    }
    nk_end(ctx);
    
    nk_style_pop_color(ctx);
}

// Render the bottom control bar
static void render_bottom_controls(struct nk_context* ctx, video_controls_state* state, media_info* info, float alpha) {
    if (alpha <= 0.01f) return;
    
    const int bar_height = 100 * UI_SCALE;
    struct nk_rect bar_rect = nk_rect(0, SCREEN_HEIGHT - bar_height, SCREEN_WIDTH, bar_height);
    
    // Set window background with alpha
    struct nk_color bg = AppleTheme::BG_ELEVATED;
    bg.a = (nk_byte)(bg.a * alpha * 0.95f);
    
    nk_style_push_color(ctx, &ctx->style.window.fixed_background.data.color, bg);
    
    if (nk_begin(ctx, "BottomControls", bar_rect,
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BACKGROUND)) {
        
        // Progress bar (first row)
        nk_layout_row_dynamic(ctx, 25 * UI_SCALE, 1);
        {
            double progress_seconds = 0.0;
            double total_seconds = 0.0;
            
            switch(info->type) {
                case 'V': // Video
                    progress_seconds = std::min(info->current_video_playback_time, info->total_video_playback_time);
                    total_seconds = info->total_video_playback_time;
                    break;
                case 'A': // Audio
                    progress_seconds = std::min(info->current_audio_playback_time, info->total_audio_playback_time);
                    total_seconds = info->total_audio_playback_time;
                    break;
                default:
                    progress_seconds = 0.0;
                    total_seconds = 1.0;
                    break;
            }
            
            nk_size progress = static_cast<nk_size>(progress_seconds);
            nk_size max_val = static_cast<nk_size>(total_seconds > 0 ? total_seconds : 1);
            
            // Make progress bar interactive for scrubbing
            nk_size old_progress = progress;
            nk_progress(ctx, &progress, max_val, NK_MODIFIABLE);
            
            // Detect scrubbing
            if (progress != old_progress) {
                double new_time = (double)progress;
                if (app_state_get() == STATE_PLAYING_VIDEO) {
                    video_player_seek(new_time);
                }
                state->scrubbing = true;
                widget_video_controls_show(state);
            } else if (state->scrubbing) {
                state->scrubbing = false;
            }
        }
        
        // Time display (second row)
        nk_layout_row_dynamic(ctx, 20 * UI_SCALE, 1);
        {
            double progress_seconds = (info->type == 'V') ? info->current_video_playback_time : info->current_audio_playback_time;
            double total_seconds = (info->type == 'V') ? info->total_video_playback_time : info->total_audio_playback_time;
            
            std::string time_str = format_time(progress_seconds) + " / " + format_time(total_seconds);
            
            struct nk_color text_color = AppleTheme::LABEL_SECONDARY;
            text_color.a = (nk_byte)(255 * alpha);
            nk_style_push_color(ctx, &ctx->style.text.color, text_color);
            
            nk_label(ctx, time_str.c_str(), NK_TEXT_CENTERED);
            
            nk_style_pop_color(ctx);
        }
        
        // Control buttons (third row)
        nk_layout_row_begin(ctx, NK_DYNAMIC, 40 * UI_SCALE, 5);
        
        // Play/Pause button (20%)
        nk_layout_row_push(ctx, 0.20f);
        {
            struct nk_color btn_color = AppleTheme::SYSTEM_BLUE;
            btn_color.a = (nk_byte)(255 * alpha);
            nk_style_push_color(ctx, &ctx->style.button.normal.data.color, btn_color);
            
            bool clicked = false;
            if (icons_loaded && g_control_icons[ICON_PAUSE].texture && info->playback_status) {
                // Use pause icon when playing
                struct nk_image icon = g_control_icons[ICON_PAUSE].image;
                clicked = nk_button_image_label(ctx, icon, "Pause", NK_TEXT_RIGHT);
            } else {
                // Use text fallback
                const char* play_pause_text = info->playback_status ? "⏸ Pause" : "▶ Play";
                clicked = nk_button_label(ctx, play_pause_text);
            }
            
            if (clicked) {
                if (app_state_get() == STATE_PLAYING_VIDEO) {
                    video_player_play(!info->playback_status);
                } else if (app_state_get() == STATE_PLAYING_AUDIO) {
                    audio_player_play(!info->playback_status);
                }
                
                // Show center indicator
                widget_video_controls_show_center_indicator(state);
                widget_video_controls_show(state);
            }
            
            nk_style_pop_color(ctx);
        }
        
        // Volume controls (30%)
        nk_layout_row_push(ctx, 0.30f);
        {
            // Volume slider with icon
            nk_layout_row_begin(ctx, NK_DYNAMIC, 40 * UI_SCALE, 2);
            
            // Volume icon (10% of this section)
            nk_layout_row_push(ctx, 0.15f);
            {
                struct nk_color text_color = AppleTheme::LABEL_PRIMARY;
                text_color.a = (nk_byte)(255 * alpha);
                nk_style_push_color(ctx, &ctx->style.text.color, text_color);
                
                // Show appropriate volume icon based on level
                const char* volume_icon = state->volume > 0.5f ? "🔊" : (state->volume > 0.0f ? "🔉" : "🔇");
                nk_label(ctx, volume_icon, NK_TEXT_LEFT);
                
                nk_style_pop_color(ctx);
            }
            
            // Volume slider (85% of this section)
            nk_layout_row_push(ctx, 0.85f);
            {
                float old_volume = state->volume;
                nk_slider_float(ctx, 0.0f, &state->volume, 1.0f, 0.01f);
                
                if (state->volume != old_volume) {
                    // Volume changed - apply it
                    // Note: SDL audio volume control would be implemented here
                    // For now, just track the state for future implementation
                    state->volume_dragging = true;
                    widget_video_controls_show(state);
                } else if (state->volume_dragging) {
                    state->volume_dragging = false;
                }
            }
            
            nk_layout_row_end(ctx);
        }
        
        // Spacer (20%)
        nk_layout_row_push(ctx, 0.20f);
        {
            // Empty space for balance
        }
        
        // Audio/Subtitle track info (15%)
        if (app_state_get() == STATE_PLAYING_VIDEO) {
            nk_layout_row_push(ctx, 0.15f);
            {
                struct nk_color text_color = AppleTheme::LABEL_SECONDARY;
                text_color.a = (nk_byte)(255 * alpha);
                nk_style_push_color(ctx, &ctx->style.text.color, text_color);
                
                std::string track_info = "A:" + std::to_string(info->current_audio_track_id) + 
                                        "/" + std::to_string(info->total_audio_track_count);
                nk_label(ctx, track_info.c_str(), NK_TEXT_RIGHT);
                
                nk_style_pop_color(ctx);
            }
        }
        
        // Settings button (15%)
        nk_layout_row_push(ctx, 0.15f);
        {
            struct nk_color btn_color = AppleTheme::FILL_SECONDARY;
            btn_color.a = (nk_byte)(255 * alpha);
            nk_style_push_color(ctx, &ctx->style.button.normal.data.color, btn_color);
            
            bool clicked = false;
            if (icons_loaded && g_control_icons[ICON_SETTINGS].texture) {
                // Use settings icon
                struct nk_image icon = g_control_icons[ICON_SETTINGS].image;
                clicked = nk_button_image(ctx, icon);
            } else {
                // Use text fallback
                clicked = nk_button_label(ctx, "⚙ Settings");
            }
            
            if (clicked) {
                state->settings_open = !state->settings_open;
                widget_video_controls_show(state);
            }
            
            nk_style_pop_color(ctx);
        }
        
        nk_layout_row_end(ctx);
    }
    nk_end(ctx);
    
    nk_style_pop_color(ctx);
}

// Render settings overlay
static void render_settings_overlay(struct nk_context* ctx, video_controls_state* state, media_info* info) {
    if (!state->settings_open) return;
    
    const int overlay_width = 400 * UI_SCALE;
    const int overlay_height = 500 * UI_SCALE;
    struct nk_rect overlay_rect = nk_rect(
        (SCREEN_WIDTH - overlay_width) / 2,
        (SCREEN_HEIGHT - overlay_height) / 2,
        overlay_width,
        overlay_height
    );
    
    if (nk_begin(ctx, "Settings Overlay", overlay_rect,
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_MOVABLE | NK_WINDOW_CLOSABLE)) {
        
        nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
        
        // Playback speed section
        nk_label(ctx, "Playback Speed:", NK_TEXT_LEFT);
        nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 4);
        
        // Speed buttons with checkmark for current speed
        const float speeds[] = {0.5f, 1.0f, 1.5f, 2.0f};
        const char* speed_labels[] = {"0.5x", "1.0x", "1.5x", "2.0x"};
        
        for (int i = 0; i < 4; i++) {
            std::string label = speed_labels[i];
            if (std::abs(state->playback_speed - speeds[i]) < 0.01f) {
                label += " ✓";
            }
            
            if (nk_button_label(ctx, label.c_str())) {
                state->playback_speed = speeds[i];
                // Note: FFmpeg playback speed would be implemented here
                // This would require av_frame timing adjustments
                log_message(LOG_OK, "VideoControls", "Playback speed changed to %.1fx", speeds[i]);
                widget_video_controls_show(state);
            }
        }
        
        nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
        nk_label(ctx, "", NK_TEXT_LEFT); // Spacer
        
        // Audio tracks section
        if (app_state_get() == STATE_PLAYING_VIDEO) {
            nk_label(ctx, "Audio Track:", NK_TEXT_LEFT);
            for (int i = 0; i < info->total_audio_track_count && i < 5; i++) {
                nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
                std::string track_label = "Track " + std::to_string(i + 1);
                if (i == info->current_audio_track_id) {
                    track_label += " ✓";
                }
                if (nk_button_label(ctx, track_label.c_str())) {
                    state->selected_audio_track = i;
                    // Note: Audio track switching for video player not yet exposed in API
                    // This would use video_player_switch_audio_track(i) when available
                    log_message(LOG_OK, "VideoControls", "Audio track selected: %d", i);
                    widget_video_controls_show(state);
                }
            }
            
            nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
            nk_label(ctx, "", NK_TEXT_LEFT); // Spacer
            
            // Subtitle tracks section
            nk_label(ctx, "Subtitles:", NK_TEXT_LEFT);
            nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
            
            // Off button
            std::string off_label = "Off";
            if (state->selected_subtitle_track == -1) {
                off_label += " ✓";
            }
            if (nk_button_label(ctx, off_label.c_str())) {
                state->selected_subtitle_track = -1;
                subtitle_cleanup();
                log_message(LOG_OK, "VideoControls", "Subtitles disabled");
                widget_video_controls_show(state);
            }
            
            for (int i = 0; i < info->total_caption_count && i < 5; i++) {
                std::string caption_label = "Caption " + std::to_string(i + 1);
                if (i == state->selected_subtitle_track) {
                    caption_label += " ✓";
                }
                if (nk_button_label(ctx, caption_label.c_str())) {
                    state->selected_subtitle_track = i;
                    // Note: Subtitle track switching would require access to caption file paths
                    // This would use subtitle_start(caption_path) when available
                    log_message(LOG_OK, "VideoControls", "Subtitle track selected: %d", i);
                    widget_video_controls_show(state);
                }
            }
        } else if (app_state_get() == STATE_PLAYING_AUDIO) {
            // Audio track switching for audio player
            nk_label(ctx, "Audio Track:", NK_TEXT_LEFT);
            for (int i = 0; i < info->total_audio_track_count && i < 10; i++) {
                nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
                std::string track_label = "Track " + std::to_string(i + 1);
                if (i == info->current_audio_track_id) {
                    track_label += " ✓";
                }
                if (nk_button_label(ctx, track_label.c_str())) {
                    // Use audio player track switching
                    bool success = audio_player_switch_audio_stream(i);
                    if (success) {
                        state->selected_audio_track = i;
                        log_message(LOG_OK, "VideoControls", "Switched to audio track %d", i);
                    } else {
                        log_message(LOG_ERROR, "VideoControls", "Failed to switch to audio track %d", i);
                    }
                    widget_video_controls_show(state);
                }
            }
        }
        
        // Close button
        nk_layout_row_dynamic(ctx, 40 * UI_SCALE, 1);
        nk_label(ctx, "", NK_TEXT_LEFT); // Spacer
        if (nk_button_label(ctx, "Close")) {
            state->settings_open = false;
        }
        
    } else {
        // Window was closed via X button
        state->settings_open = false;
    }
    nk_end(ctx);
}

void widget_video_controls_render(struct nk_context* ctx, video_controls_state* state, media_info* info) {
    // Detect play state changes for center indicator
    bool current_playing_state = info->playback_status;
    if (current_playing_state != state->last_playing_state) {
        widget_video_controls_show_center_indicator(state);
        state->last_playing_state = current_playing_state;
    }
    
    // Render center play/pause indicator (always visible when showing)
    render_center_indicator(ctx, state, current_playing_state);
    
    // Only render other controls if they have some visibility
    if (state->fade_alpha > 0.01f) {
        render_top_bar(ctx, state, info, state->fade_alpha);
        render_bottom_controls(ctx, state, info, state->fade_alpha);
    }
    
    // Render settings overlay (always full opacity when open)
    render_settings_overlay(ctx, state, info);
}
