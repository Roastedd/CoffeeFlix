#include "widget_tooltip.hpp"

#include "main.hpp"
#include "utils/app_state.hpp"
#include "vendor/ui/nuklear.h"

// Safe margin for TV overscan
static const float TOOLTIP_SAFE_MARGIN = 24.0f * UI_SCALE;

void widget_tooltip_render(struct nk_context *ctx) {
    // Position tooltip bar higher to account for TV overscan safe zone
    float tooltip_y = SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE - TOOLTIP_SAFE_MARGIN;
    
    // Get current style and create a semi-transparent background
    struct nk_style_window window_style = ctx->style.window;
    
    // Set darker, semi-transparent background for better visibility
    ctx->style.window.fixed_background = nk_style_item_color(nk_rgba(20, 20, 20, 220));
    ctx->style.window.border_color = nk_rgba(60, 60, 60, 255);
    ctx->style.window.border = 2.0f;
    
    if (nk_begin(ctx, "tooltip_bar", nk_rect(TOOLTIP_SAFE_MARGIN, tooltip_y, SCREEN_WIDTH - (TOOLTIP_SAFE_MARGIN * 2), TOOLTIP_BAR_HEIGHT * UI_SCALE), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER | NK_WINDOW_BACKGROUND)) {
        // Use static layout for better control
        nk_layout_row_begin(ctx, NK_STATIC, (TOOLTIP_BAR_HEIGHT * UI_SCALE) - 10.0f, 1);
        nk_layout_row_push(ctx, SCREEN_WIDTH - (TOOLTIP_SAFE_MARGIN * 4));
        
        // Set text color to light gray for better visibility
        struct nk_color text_color = nk_rgb(220, 220, 220);
        
        switch(app_state_get()) {
        case STATE_MENU: 
            nk_label_colored(ctx, "(-) Toggle Sidebar  |  (D-Pad/Stick) Navigate  |  (A) Select  |  [Touch]", NK_TEXT_LEFT, text_color);
            break;
        case STATE_MENU_FILES:
        case STATE_MENU_NETWORK_FILES:
        case STATE_MENU_VIDEO_FILES:
        case STATE_MENU_AUDIO_FILES:
        case STATE_MENU_IMAGE_FILES: 
        case STATE_MENU_PDF_FILES:
            nk_label_colored(ctx, "(-) Toggle Sidebar  |  (D-Pad/Stick) Select  |  (A) Open  |  (+) Rescan  |  [Touch]", NK_TEXT_LEFT, text_color);
            break;
        case STATE_MENU_SETTINGS: 
            nk_label_colored(ctx, "(-) Toggle Sidebar  |  (D-Pad/Stick) Navigate  |  (A) Select  |  [Touch]", NK_TEXT_LEFT, text_color);
            break;
        case STATE_MENU_YOUTUBE:
            nk_label_colored(ctx, "(-) Sidebar  |  (D-Pad/Stick) Navigate Grid  |  (A) Play  |  (+) Load More  |  [Touch] Keyboard", NK_TEXT_LEFT, text_color);
            break;
        case STATE_MENU_EASTER_EGG: break;
        case STATE_PLAYING_VIDEO: break;
        case STATE_PLAYING_AUDIO: break;
        case STATE_VIEWING_PHOTO:
            nk_label_colored(ctx, "(D-Pad/Stick) Change Photo  |  (ZL) Zoom In  |  (ZR) Zoom Out  |  [Touch] Pan", NK_TEXT_LEFT, text_color);
            break;
        case STATE_VIEWING_PDF:
            nk_label_colored(ctx, "(D-Pad/Stick) Change Page  |  (ZL) Zoom In  |  (ZR) Zoom Out  |  [Touch] Pan", NK_TEXT_LEFT, text_color);
            break;
        }
        
        nk_layout_row_end(ctx);
        nk_end(ctx);
    }
    
    // Restore original style
    ctx->style.window = window_style;
}
