#include <string>
#include <cstring>

#include "main.hpp"
#include "ui/widgets/widget_tooltip.hpp"
#include "ui/widgets/widget_sidebar.hpp"
#include "ui/apple_theme.hpp"
#include "utils/utils.hpp"
#include "vendor/ui/nuklear.h"
#include "logger/logger.hpp"
#include "network/innertube.hpp"
#include "player/video_player.hpp"
#include "input/input_actions.hpp"

#include "ui/scenes/scene_youtube.hpp"

// Tab system
enum YouTubeTab {
    TAB_DIRECT_URL,
    TAB_SEARCH,
    TAB_TRENDING
};

static YouTubeTab current_tab = TAB_SEARCH;
static char input_buffer[512] = {0};
static bool is_loading = false;
static std::string status_message = "";
static std::string last_video_url = "";
static InnerTube::VideoInfo last_video_info;
static int selected_quality = 0; // Default to 360p
static InnerTube::SearchResults search_results;
static int selected_result_index = 0;
static std::string last_search_query = "";
static float scroll_offset = 0.0f;
static uint64_t last_input_state = 0;
static bool trending_loaded = false;

const char* quality_labels[] = {"360p", "480p", "720p", "Auto"};
const InnerTube::VideoQuality quality_values[] = {
    InnerTube::QUALITY_360P,
    InnerTube::QUALITY_480P,
    InnerTube::QUALITY_720P,
    InnerTube::QUALITY_AUTO
};

// Helper to format duration
std::string format_duration(int seconds) {
    int hours = seconds / 3600;
    int mins = (seconds % 3600) / 60;
    int secs = seconds % 60;
    
    char buf[32];
    if (hours > 0) {
        snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, mins, secs);
    } else {
        snprintf(buf, sizeof(buf), "%d:%02d", mins, secs);
    }
    return std::string(buf);
}

// Helper to format view count
std::string format_views(int64_t views) {
    char buf[32];
    if (views >= 1000000000) {
        snprintf(buf, sizeof(buf), "%.1fB views", views / 1000000000.0);
    } else if (views >= 1000000) {
        snprintf(buf, sizeof(buf), "%.1fM views", views / 1000000.0);
    } else if (views >= 1000) {
        snprintf(buf, sizeof(buf), "%.1fK views", views / 1000.0);
    } else {
        return std::to_string(views) + " views";
    }
    return std::string(buf);
}

void scene_youtube_render(struct nk_context *ctx) {
    float sidebar_width = widget_sidebar_get_width();
    float content_width = SCREEN_WIDTH - sidebar_width;
    
    if (nk_begin(ctx, VERSION_STRING, nk_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE), 
                 NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
        
        nk_layout_row_begin(ctx, NK_STATIC, SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE, 2);
        widget_sidebar_render(ctx);

        nk_layout_row_push(ctx, content_width);  // Dynamic content area

        if (nk_group_begin(ctx, "YouTubeContent", NK_WINDOW_BORDER)) {
            // Add top padding
            nk_layout_row_dynamic(ctx, 10 * UI_SCALE, 1);
            nk_spacing(ctx, 1);
            
            // Title - Compact and clean
            nk_layout_row_dynamic(ctx, 35 * UI_SCALE, 1);
            nk_label(ctx, "YouTube Streaming", NK_TEXT_CENTERED);
            
            nk_layout_row_dynamic(ctx, 8 * UI_SCALE, 1);
            nk_spacing(ctx, 1);
            
            // Tabs - Compact radio buttons
            nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 3);
            if (nk_option_label(ctx, "Direct URL", current_tab == TAB_DIRECT_URL)) {
                if (current_tab != TAB_DIRECT_URL) {
                    current_tab = TAB_DIRECT_URL;
                    status_message = "";
                    search_results.results.clear();
                    selected_result_index = 0;
                }
            }
            if (nk_option_label(ctx, "Search", current_tab == TAB_SEARCH)) {
                if (current_tab != TAB_SEARCH) {
                    current_tab = TAB_SEARCH;
                    status_message = "";
                    search_results.results.clear();
                    selected_result_index = 0;
                }
            }
            if (nk_option_label(ctx, "Trending", current_tab == TAB_TRENDING)) {
                if (current_tab != TAB_TRENDING) {
                    current_tab = TAB_TRENDING;
                    status_message = "";
                    search_results.results.clear();
                    selected_result_index = 0;
                    // Auto-load trending on first switch
                    if (!trending_loaded && !is_loading) {
                        is_loading = true;
                        search_results = InnerTube::get_trending();
                        is_loading = false;
                        trending_loaded = true;
                        scroll_offset = 0.0f;
                        if (search_results.success) {
                            status_message = "Loaded " + std::to_string(search_results.results.size()) + " trending videos";
                        } else {
                            status_message = "Error loading trending videos";
                            trending_loaded = false; // Allow retry
                        }
                    }
                }
            }
            
            nk_layout_row_dynamic(ctx, 15 * UI_SCALE, 1);
            nk_spacing(ctx, 1);
            
            // Tab content
            if (current_tab == TAB_DIRECT_URL) {
                // Direct URL input
                nk_layout_row_dynamic(ctx, 25 * UI_SCALE, 1);
                nk_label(ctx, "YouTube URL:", NK_TEXT_LEFT);
                
                nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, input_buffer, sizeof(input_buffer), nk_filter_default);
                
                nk_layout_row_dynamic(ctx, 40 * UI_SCALE, 1);
                if (!is_loading && nk_button_label(ctx, "Fetch Video Info")) {
                    if (strlen(input_buffer) > 0) {
                        is_loading = true;
                        std::string video_id = InnerTube::extract_video_id(input_buffer);
                        if (!video_id.empty()) {
                            // Try ANDROID_VR client first (best compatibility)
                            last_video_info = InnerTube::get_video_info(video_id, true);
                            
                            if (last_video_info.success) {
                                last_video_url = InnerTube::get_best_stream_url(last_video_info, quality_values[selected_quality]);
                                
                                if (!last_video_url.empty()) {
                                    status_message = "Ready: " + last_video_info.title;
                                } else {
                                    status_message = "Error: No compatible streams";
                                }
                            } else {
                                status_message = "Error: Video not available";
                                last_video_url = "";
                            }
                        } else {
                            status_message = "Error: Invalid YouTube URL";
                            last_video_url = "";
                        }
                        is_loading = false;
                    }
                }
                
            } else if (current_tab == TAB_SEARCH) {
                // Search input
                nk_layout_row_dynamic(ctx, 25 * UI_SCALE, 1);
                nk_label(ctx, "Search Query:", NK_TEXT_LEFT);
                
                nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
                nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, input_buffer, sizeof(input_buffer), nk_filter_default);
                
                nk_layout_row_dynamic(ctx, 40 * UI_SCALE, 1);
                if (!is_loading && nk_button_label(ctx, "Search")) {
                    if (strlen(input_buffer) > 0) {
                        is_loading = true;
                        last_search_query = input_buffer;
                        search_results = InnerTube::search_videos(last_search_query, "");
                        selected_result_index = 0;
                        is_loading = false;
                        
                        if (!search_results.results.empty()) {
                            status_message = "Found " + std::to_string(search_results.results.size()) + " results";
                        } else {
                            status_message = "No results found";
                        }
                    }
                }
            }
            
            nk_layout_row_dynamic(ctx, 12 * UI_SCALE, 1);
            nk_spacing(ctx, 1);
            
            // Quality selection (for all tabs)
            nk_layout_row_dynamic(ctx, 25 * UI_SCALE, 1);
            nk_label(ctx, "Quality:", NK_TEXT_LEFT);
            
            nk_layout_row_dynamic(ctx, 28 * UI_SCALE, 4);
            for (int i = 0; i < 4; i++) {
                if (nk_option_label(ctx, quality_labels[i], selected_quality == i)) {
                    selected_quality = i;
                }
            }
            
            nk_layout_row_dynamic(ctx, 12 * UI_SCALE, 1);
            nk_spacing(ctx, 1);
            
            // Results display (for Search, Trending)
            if (current_tab != TAB_DIRECT_URL) {
                // Calculate remaining height dynamically
                float remaining_height = (SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE) - 260 * UI_SCALE;
                nk_layout_row_dynamic(ctx, remaining_height, 1);
                if (nk_group_begin(ctx, "Results", NK_WINDOW_BORDER)) {
                    if (is_loading) {
                        nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
                        nk_label(ctx, "Loading...", NK_TEXT_CENTERED);
                    } else if (!search_results.results.empty()) {
                        // Grid layout: 2 columns
                        const float card_height = 110 * UI_SCALE;
                        const int cols = 2;
                        
                        // Calculate rows needed
                        int rows = (search_results.results.size() + cols - 1) / cols;
                        
                        for (int row = 0; row < rows; row++) {
                            nk_layout_row_dynamic(ctx, card_height, cols);
                            
                            for (int col = 0; col < cols; col++) {
                                int idx = row * cols + col;
                                if (idx >= search_results.results.size()) break;
                                
                                const auto& result = search_results.results[idx];
                                
                                // Create video card group
                                struct nk_style_window group_style = ctx->style.window;
                                if (idx == selected_result_index) {
                                    ctx->style.window.fixed_background = nk_style_item_color(AppleTheme::SYSTEM_BLUE);
                                    ctx->style.window.border_color = nk_rgb(0, 122, 255);
                                    ctx->style.window.border = 3.0f;
                                } else {
                                    ctx->style.window.fixed_background = nk_style_item_color(AppleTheme::BG_TERTIARY);
                                    ctx->style.window.border_color = AppleTheme::SEPARATOR;
                                    ctx->style.window.border = 1.0f;
                                }
                                
                                char card_name[64];
                                snprintf(card_name, sizeof(card_name), "VideoCard_%d", idx);
                                
                                if (nk_group_begin(ctx, card_name, NK_WINDOW_BORDER)) {
                                    // Thumbnail placeholder (we'll show text for now)
                                    nk_layout_row_dynamic(ctx, 60 * UI_SCALE, 1);
                                    struct nk_rect thumbnail_rect = nk_widget_bounds(ctx);
                                    struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
                                    if (canvas) {
                                        nk_fill_rect(canvas, thumbnail_rect, 0, nk_rgb(40, 40, 40));
                                    }
                                    
                                    // Duration overlay in bottom-right of thumbnail
                                    if (canvas && ctx->style.font && !result.duration_text.empty()) {
                                        const char* duration_str = result.duration_text.c_str();
                                        float text_width = ctx->style.font->width(ctx->style.font->userdata, 
                                                                                  ctx->style.font->height, 
                                                                                  duration_str, strlen(duration_str));
                                        struct nk_rect duration_rect = nk_rect(
                                            thumbnail_rect.x + thumbnail_rect.w - text_width - 8,
                                            thumbnail_rect.y + thumbnail_rect.h - 20,
                                            text_width + 6,
                                            18
                                        );
                                        nk_fill_rect(canvas, duration_rect, 0, nk_rgba(0, 0, 0, 180));
                                        nk_draw_text(canvas, duration_rect, duration_str, strlen(duration_str),
                                                   ctx->style.font, nk_rgba(0, 0, 0, 0), nk_rgb(255, 255, 255));
                                    }
                                    
                                    nk_spacing(ctx, 1);
                                    
                                    // Title (truncated if too long)
                                    nk_layout_row_dynamic(ctx, 20 * UI_SCALE, 1);
                                    std::string title = result.title;
                                    if (title.length() > 40) {
                                        title = title.substr(0, 37) + "...";
                                    }
                                    struct nk_color text_color = (idx == selected_result_index) ? 
                                        nk_rgb(255, 255, 255) : AppleTheme::LABEL_PRIMARY;
                                    nk_label_colored(ctx, title.c_str(), NK_TEXT_LEFT, text_color);
                                    
                                    // Author + views
                                    nk_layout_row_dynamic(ctx, 15 * UI_SCALE, 1);
                                    std::string metadata = result.author;
                                    if (result.view_count > 0) {
                                        metadata += " • " + format_views(result.view_count);
                                    } else if (!result.view_count_text.empty()) {
                                        metadata += " • " + result.view_count_text;
                                    }
                                    if (metadata.length() > 45) {
                                        metadata = metadata.substr(0, 42) + "...";
                                    }
                                    struct nk_color secondary_color = (idx == selected_result_index) ? 
                                        nk_rgb(200, 200, 200) : AppleTheme::LABEL_SECONDARY;
                                    nk_label_colored(ctx, metadata.c_str(), NK_TEXT_LEFT, secondary_color);
                                    
                                    nk_group_end(ctx);
                                }
                                
                                ctx->style.window = group_style;
                            }
                        }
                        
                        // Load More button if continuation token available
                        if (!search_results.continuation_token.empty()) {
                            nk_layout_row_dynamic(ctx, 45 * UI_SCALE, 1);
                            if (!is_loading && nk_button_label(ctx, "Load More Results (+)")) {
                                is_loading = true;
                                bool success = InnerTube::load_more_results(search_results);
                                is_loading = false;
                                
                                if (success) {
                                    status_message = "Loaded " + std::to_string(search_results.results.size()) + " total results";
                                } else {
                                    status_message = "Error loading more results";
                                }
                            }
                        }
                    } else {
                        nk_layout_row_dynamic(ctx, 30 * UI_SCALE, 1);
                        nk_spacing(ctx, 1);
                        nk_layout_row_dynamic(ctx, 25 * UI_SCALE, 1);
                        if (!status_message.empty()) {
                            nk_label_wrap(ctx, status_message.c_str());
                        } else {
                            nk_label(ctx, "No results", NK_TEXT_CENTERED);
                        }
                    }
                    
                    nk_group_end(ctx);
                }
            } else {
                // Play button for direct URL
                if (!last_video_url.empty()) {
                    nk_layout_row_dynamic(ctx, 45 * UI_SCALE, 1);
                    if (nk_button_label(ctx, "Play Video")) {
                        log_message(LOG_OK, "YouTube", "Starting playback");
                        video_player_init(last_video_url.c_str());
                    }
                }
                
                // Status display for direct URL
                if (!status_message.empty()) {
                    nk_layout_row_dynamic(ctx, 12 * UI_SCALE, 1);
                    nk_spacing(ctx, 1);
                    
                    float remaining_height = (SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE) - 280 * UI_SCALE;
                    nk_layout_row_dynamic(ctx, remaining_height, 1);
                    if (nk_group_begin(ctx, "StatusGroup", NK_WINDOW_BORDER)) {
                        nk_layout_row_dynamic(ctx, 10 * UI_SCALE, 1);
                        nk_spacing(ctx, 1);
                        nk_layout_row_dynamic(ctx, 20 * UI_SCALE, 1);
                        nk_label_wrap(ctx, status_message.c_str());
                        nk_group_end(ctx);
                    }
                }
            }

            nk_group_end(ctx);
        }

        nk_layout_row_end(ctx);
        nk_end(ctx);
    }

    widget_tooltip_render(ctx);
}

void scene_youtube_input(InputState& input) {
    // Toggle sidebar with MINUS button
    if (input_pressed(input, BTN_MINUS)) {
        widget_sidebar_toggle();
        last_input_state = input.pressed;
        return;
    }
    
    // Only handle navigation if not in Direct URL input mode and have results
    if (current_tab != TAB_DIRECT_URL && !search_results.results.empty() && !is_loading) {
        const int cols = 2;
        int max_index = search_results.results.size() - 1;
        
        // Clamp selected_result_index to valid range (safety check)
        if (selected_result_index < 0) selected_result_index = 0;
        if (selected_result_index > max_index) selected_result_index = max_index;
        
        // D-pad/Left Stick navigation
        bool left_pressed = (input.pressed & (1ull << BTN_LEFT)) && !(last_input_state & (1ull << BTN_LEFT));
        bool right_pressed = (input.pressed & (1ull << BTN_RIGHT)) && !(last_input_state & (1ull << BTN_RIGHT));
        bool up_pressed = (input.pressed & (1ull << BTN_UP)) && !(last_input_state & (1ull << BTN_UP));
        bool down_pressed = (input.pressed & (1ull << BTN_DOWN)) && !(last_input_state & (1ull << BTN_DOWN));
        bool a_pressed = (input.pressed & (1ull << BTN_A)) && !(last_input_state & (1ull << BTN_A));
        bool plus_pressed = (input.pressed & (1ull << BTN_PLUS)) && !(last_input_state & (1ull << BTN_PLUS));
        
        if (left_pressed && selected_result_index > 0) {
            selected_result_index--;
        }
        else if (right_pressed && selected_result_index < max_index) {
            selected_result_index++;
        }
        else if (up_pressed) {
            // Move up by one row (2 columns)
            if (selected_result_index >= cols) {
                selected_result_index -= cols;
            }
        }
        else if (down_pressed) {
            // Move down by one row (2 columns)
            if (selected_result_index + cols <= max_index) {
                selected_result_index += cols;
            }
        }
        else if (a_pressed && selected_result_index < search_results.results.size()) {
            // Play selected video
            const auto& result = search_results.results[selected_result_index];
            
            status_message = "Loading video...";
            is_loading = true;
            
            last_video_info = InnerTube::get_video_info(result.video_id, true);
            
            if (last_video_info.success) {
                last_video_url = InnerTube::get_best_stream_url(last_video_info, quality_values[selected_quality]);
                
                if (!last_video_url.empty()) {
                    log_message(LOG_OK, "YouTube", "Playing: %s", result.title.c_str());
                    video_player_init(last_video_url.c_str());
                    status_message = "Now playing: " + result.title;
                } else {
                    status_message = "Error: No compatible streams";
                }
            } else {
                status_message = "Error: Video not available";
            }
            
            is_loading = false;
        }
        else if (plus_pressed && !search_results.continuation_token.empty()) {
            // Load more results
            is_loading = true;
            bool success = InnerTube::load_more_results(search_results);
            is_loading = false;
            
            if (success) {
                status_message = "Loaded " + std::to_string(search_results.results.size()) + " total results";
            } else {
                status_message = "Error loading more results";
            }
        }
    }
    
    last_input_state = input.pressed;
}
