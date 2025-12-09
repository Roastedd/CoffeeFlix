#include <string>
#include <vector>

#include "main.hpp"
#include "ui/widgets/widget_tooltip.hpp"
#include "ui/widgets/widget_sidebar.hpp"
#include "ui/apple_theme.hpp"
#include "utils/utils.hpp"
#include "utils/media_files.hpp"
#include "utils/app_state.hpp"
#include "vendor/ui/nuklear.h"
#include "logger/logger.hpp"

#include "ui/scenes/scene_file_browser.hpp"

int selected_index = 0;

// Safe zones for TV overscan
const float SAFE_MARGIN_X = 64.0f * UI_SCALE;
const float SAFE_MARGIN_Y = 36.0f * UI_SCALE;

void scene_file_browser_render(struct nk_context *ctx) {
    float sidebar_width = widget_sidebar_get_width();
    float content_width = SCREEN_WIDTH - sidebar_width;
    
    if (nk_begin(ctx, VERSION_STRING, nk_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
        nk_layout_row_begin(ctx, NK_STATIC, SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE, 2);
        widget_sidebar_render(ctx);

        nk_layout_row_push(ctx, content_width);

        if (nk_group_begin(ctx, "FileList", NK_WINDOW_BORDER)) {
            nk_layout_row_dynamic(ctx, 64 * UI_SCALE, 1);

            int total_file_count = static_cast<int>(get_media_files().size());

            for (int i = 0; i < total_file_count; ++i) {
                std::string display_str = truncate_filename(get_media_files()[i], 80);
                struct nk_style_button button_style = ctx->style.button;

                if (i == selected_index) {
                    // Highlight selected item with Apple-style selection
                    ctx->style.button.normal = nk_style_item_color(nk_rgb(0, 122, 255));
                    ctx->style.button.hover = nk_style_item_color(nk_rgb(10, 132, 255));
                    ctx->style.button.active = nk_style_item_color(nk_rgb(0, 112, 245));
                    ctx->style.button.text_normal = nk_rgb(255, 255, 255);
                    ctx->style.button.text_hover = nk_rgb(255, 255, 255);
                    ctx->style.button.text_active = nk_rgb(255, 255, 255);
                    ctx->style.button.border = 3.0f;
                    ctx->style.button.border_color = nk_rgb(0, 122, 255);
                }

                if (nk_button_label(ctx, display_str.c_str())) {
                    selected_index = i;
                    start_file(selected_index);
                }

                ctx->style.button = button_style;
            }

            nk_group_end(ctx);
        }

        nk_layout_row_end(ctx);
        nk_end(ctx);
    }

    widget_tooltip_render(ctx);
}

void scene_file_browser_input(InputState& input) {
    static uint64_t last_input_state = 0;
    int total_file_count = static_cast<int>(get_media_files().size());
    if (total_file_count == 0) return;

    // Toggle sidebar with MINUS button
    if (input_pressed(input, BTN_MINUS)) {
        widget_sidebar_toggle();
    }
    
    // Rescan media files with PLUS button
    if (input_pressed(input, BTN_PLUS)) {
        // Determine which directory to rescan based on current state
        int state = app_state_get();
        const char* scan_path = nullptr;
        
        switch (state) {
            case STATE_MENU_VIDEO_FILES:
                scan_path = MEDIA_PATH_VIDEO;
                break;
            case STATE_MENU_AUDIO_FILES:
                scan_path = MEDIA_PATH_AUDIO;
                break;
            case STATE_MENU_IMAGE_FILES:
                scan_path = MEDIA_PATH_PHOTO;
                break;
            case STATE_MENU_PDF_FILES:
                scan_path = MEDIA_PATH_PDF;
                break;
        }
        
        if (scan_path) {
            scan_directory(scan_path);
            selected_index = 0;  // Reset selection after rescan
            log_message(LOG_OK, "FileBrowser", "Rescanned media directory");
        }
    }

    // Navigate with D-Pad or Left Stick - check for button transitions
    bool down_pressed = (input.pressed & (1ull << BTN_DOWN)) && !(last_input_state & (1ull << BTN_DOWN));
    bool up_pressed = (input.pressed & (1ull << BTN_UP)) && !(last_input_state & (1ull << BTN_UP));
    
    // Also handle stick input
    if (down_pressed || (input.left_stick.y < -0.5f && !(last_input_state & (1ull << BTN_DOWN)))) {
        selected_index++;
        if (selected_index >= total_file_count) selected_index = 0;
    }
    else if (up_pressed || (input.left_stick.y > 0.5f && !(last_input_state & (1ull << BTN_UP)))) {
        selected_index--;
        if (selected_index < 0) selected_index = total_file_count - 1;
    }

    // Select file with A button
    if (input_pressed(input, BTN_A)) {
        start_file(selected_index);
    }
    
    last_input_state = input.pressed;
}
