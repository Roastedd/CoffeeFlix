#ifndef WIDGET_VIDEO_CONTROLS_HPP
#define WIDGET_VIDEO_CONTROLS_HPP

#include <stdint.h>

// Forward declarations
struct nk_context;
struct media_info;

// Control state for YouTube-style overlay
struct video_controls_state {
    // Visibility and animation
    bool controls_visible;
    float fade_alpha;
    uint64_t last_input_time_ms;
    uint64_t last_update_time_ms;
    
    // Auto-hide configuration
    static constexpr uint64_t AUTO_HIDE_DELAY_MS = 3000;  // 3 seconds
    static constexpr float FADE_SPEED = 0.1f;              // Alpha change per frame
    
    // Interaction states
    bool scrubbing;
    bool volume_dragging;
    bool settings_open;
    
    // Center indicator
    bool show_center_indicator;
    uint64_t center_indicator_time_ms;
    static constexpr uint64_t CENTER_INDICATOR_DURATION_MS = 500;  // 0.5 seconds
    
    // Last values for change detection
    bool last_playing_state;
    
    // Volume control
    float volume;  // 0.0 to 1.0
    
    // Playback speed
    float playback_speed;  // Current speed multiplier
    
    // Track selection
    int selected_audio_track;
    int selected_subtitle_track;  // -1 for off
};

/**
 * Initialize video controls state
 */
void widget_video_controls_init(video_controls_state* state);

/**
 * Update control visibility and animations
 * Call this every frame to handle auto-hide and fade animations
 */
void widget_video_controls_update(video_controls_state* state);

/**
 * Render YouTube-style video controls overlay
 * Shows/hides automatically based on user interaction
 * 
 * @param ctx Nuklear context
 * @param state Control state (tracks visibility, animations, etc)
 * @param info Media info (playback time, status, etc)
 */
void widget_video_controls_render(struct nk_context* ctx, video_controls_state* state, media_info* info);

/**
 * Show controls (called when user interacts)
 */
void widget_video_controls_show(video_controls_state* state);

/**
 * Show center play/pause indicator
 */
void widget_video_controls_show_center_indicator(video_controls_state* state);

#endif // WIDGET_VIDEO_CONTROLS_HPP
