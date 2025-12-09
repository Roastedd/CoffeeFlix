#include "ui/scenes/scene_main_menu.hpp"

#include "main.hpp"
#include "vendor/ui/nuklear.h"
#include "ui/focus_system.hpp"
#include "ui/widgets/widget_sidebar.hpp"
#include "ui/widgets/widget_tooltip.hpp"
#include "ui/apple_theme.hpp"
#include "utils/sdl.hpp"
#include "utils/utils.hpp"
#include "utils/app_state.hpp"
#include "logger/logger.hpp"

#include <SDL2/SDL_image.h>

// Safe zones for TV overscan (as per UI guide)
const float SAFE_MARGIN_X = 64.0f * UI_SCALE;
const float SAFE_MARGIN_Y = 36.0f * UI_SCALE;
const float TOOLTIP_SAFE_MARGIN = 24.0f * UI_SCALE;

static SDL_Texture* g_mascot_texture = nullptr;
static struct nk_image g_mascot_image;

static void ensure_mascot_texture_loaded() {
    if (g_mascot_texture) return;

    SDL_Renderer* renderer = sdl_get()->sdl_renderer;
    SDL_Surface* surface = IMG_Load("content/mascot/coffee_mascot.png");
    if (!surface) {
        log_message(LOG_ERROR, "UI", "Failed to load mascot surface: %s", IMG_GetError());
        return;
    }

    g_mascot_texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_SetTextureBlendMode(g_mascot_texture, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surface);

    if (!g_mascot_texture) {
        log_message(LOG_ERROR, "UI", "Failed to create mascot texture: %s", SDL_GetError());
        return;
    }

    g_mascot_image = nk_image_ptr(g_mascot_texture);
}

void scene_main_menu_render(struct nk_context *ctx) {
    ensure_mascot_texture_loaded();

    float sidebar_width = widget_sidebar_get_width();
    float content_width = SCREEN_WIDTH - sidebar_width;
    // Account for tooltip safe margin in main content area
    float available_height = SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE - TOOLTIP_SAFE_MARGIN;
    
    if (nk_begin(ctx, VERSION_STRING, nk_rect(0, 0, SCREEN_WIDTH, available_height), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
        nk_layout_row_begin(ctx, NK_STATIC, available_height, 2);
        widget_sidebar_render(ctx);

        nk_layout_row_push(ctx, content_width);
        if (nk_group_begin(ctx, "Content", NK_WINDOW_BORDER)) {
            // Apply safe zones for content area
            float usable_width = content_width - (SAFE_MARGIN_X * 2);
            
            // Add top margin
            nk_layout_row_dynamic(ctx, SAFE_MARGIN_Y, 1);
            nk_spacing(ctx, 1);
            
            // Hero row: mascot + heading
            float hero_height = 130 * UI_SCALE;
            nk_layout_row_begin(ctx, NK_DYNAMIC, hero_height, 3);
            nk_layout_row_push(ctx, 0.25f);
            if (g_mascot_texture) {
                nk_image(ctx, g_mascot_image);
            } else {
                nk_spacing(ctx, 1);
            }

            nk_layout_row_push(ctx, 0.5f);
            struct nk_style_text old_text = ctx->style.text;
            ctx->style.text.color = AppleTheme::SYSTEM_ORANGE;  // Warm coffee color
            nk_label(ctx, "Welcome to CafeMP", NK_TEXT_CENTERED);
            ctx->style.text = old_text;

            nk_layout_row_push(ctx, 0.25f);
            nk_spacing(ctx, 1);
            nk_layout_row_end(ctx);
            
            // Subtitle
            nk_layout_row_begin(ctx, NK_STATIC, 30 * UI_SCALE, 1);
            nk_layout_row_push(ctx, usable_width);
            ctx->style.text.color = AppleTheme::LABEL_SECONDARY;
            nk_label(ctx, "Your Wii U Media Center", NK_TEXT_CENTERED);
            ctx->style.text = old_text;
            nk_layout_row_end(ctx);
            
            // Generous spacing
            nk_layout_row_dynamic(ctx, 20 * UI_SCALE, 1);
            nk_spacing(ctx, 1);
            
            // Card container for "What's New"
            nk_layout_row_begin(ctx, NK_STATIC, 280 * UI_SCALE, 1);
            nk_layout_row_push(ctx, usable_width);
            
            struct nk_style_window old_window = ctx->style.window;
            ctx->style.window.fixed_background = nk_style_item_color(AppleTheme::BG_SECONDARY);
            ctx->style.window.rounding = AppleTheme::CARD_CORNER_RADIUS * UI_SCALE;
            ctx->style.window.padding = nk_vec2(AppleTheme::PADDING * 2 * UI_SCALE, AppleTheme::PADDING * 1.5f * UI_SCALE);
            
            if (nk_group_begin(ctx, "WhatsNewCard", NK_WINDOW_BORDER)) {
                // Section heading
                float heading_height = 45 * UI_SCALE;
                nk_layout_row_dynamic(ctx, heading_height, 1);
                
                struct nk_style_text section_text = ctx->style.text;
                section_text.color = AppleTheme::SYSTEM_BLUE;
                ctx->style.text = section_text;
                nk_label(ctx, "What's New", NK_TEXT_LEFT);
                ctx->style.text = old_text;
                
                // Feature list with better spacing
                float line_height = 38 * UI_SCALE;
                
                nk_layout_row_dynamic(ctx, line_height, 1);
                nk_label(ctx, "\u2022  Hardware video decoding (H.264 720p@30fps)", NK_TEXT_LEFT);
                
                nk_layout_row_dynamic(ctx, line_height, 1);
                nk_label(ctx, "\u2022  Redesigned UI with improved navigation", NK_TEXT_LEFT);
                
                nk_layout_row_dynamic(ctx, line_height, 1);
                nk_label(ctx, "\u2022  Enhanced touch controls for GamePad", NK_TEXT_LEFT);
                
                nk_layout_row_dynamic(ctx, line_height, 1);
                nk_label(ctx, "\u2022  PDF and EPUB support in Library", NK_TEXT_LEFT);
                
                nk_layout_row_dynamic(ctx, line_height, 1);
                nk_label(ctx, "\u2022  Optimized performance and stability", NK_TEXT_LEFT);
                
                nk_layout_row_dynamic(ctx, line_height, 1);
                nk_label(ctx, "\u2022  Safe zone support for all TV sizes", NK_TEXT_LEFT);
                
                nk_group_end(ctx);
            }
            ctx->style.window = old_window;
            nk_layout_row_end(ctx);
            
            // Spacing before instructions
            nk_layout_row_dynamic(ctx, 20 * UI_SCALE, 1);
            nk_spacing(ctx, 1);
            
            // Instructions card - updated to show MINUS button
            nk_layout_row_begin(ctx, NK_STATIC, 100 * UI_SCALE, 1);
            nk_layout_row_push(ctx, usable_width);
            
            ctx->style.window.fixed_background = nk_style_item_color(AppleTheme::BG_TERTIARY);
            ctx->style.window.rounding = AppleTheme::CARD_CORNER_RADIUS * UI_SCALE;
            ctx->style.window.padding = nk_vec2(AppleTheme::PADDING * 2 * UI_SCALE, AppleTheme::PADDING * 1.5f * UI_SCALE);
            
            if (nk_group_begin(ctx, "GetStartedCard", NK_WINDOW_BORDER)) {
                nk_layout_row_dynamic(ctx, 32 * UI_SCALE, 1);
                
                struct nk_style_text hint_text = ctx->style.text;
                hint_text.color = AppleTheme::LABEL_SECONDARY;
                ctx->style.text = hint_text;
                nk_label(ctx, "Press (-) to toggle the menu sidebar", NK_TEXT_CENTERED);
                nk_label(ctx, "Use D-Pad or Left Stick to navigate, (A) to select", NK_TEXT_CENTERED);
                ctx->style.text = old_text;
                
                nk_group_end(ctx);
            }
            ctx->style.window = old_window;
            nk_layout_row_end(ctx);
            
            nk_group_end(ctx);
        }

        nk_layout_row_end(ctx);
        nk_end(ctx);
    }

    widget_tooltip_render(ctx);
}

void scene_main_menu_input(InputState& input) {
    // Toggle sidebar with MINUS button
    if (input_pressed(input, BTN_MINUS)) {
        widget_sidebar_toggle();
        FocusState* focus = focus_system_get_state();
        focus->sidebar_visible = widget_sidebar_is_visible();
    }
    
    // Activate selected sidebar item with A button when sidebar has focus
    if (input_pressed(input, BTN_A) && focus_system_should_highlight_sidebar()) {
        int selected = focus_system_get_sidebar_selection();
        
        // Map sidebar indices to actions
        switch(selected) {
            case 0: // Home
                app_state_set(STATE_MENU);
                break;
            case 1: // Video
                app_state_set(STATE_MENU_VIDEO_FILES);
                scan_directory(MEDIA_PATH_VIDEO);
                break;
            case 2: // Audio
                app_state_set(STATE_MENU_AUDIO_FILES);
                scan_directory(MEDIA_PATH_AUDIO);
                break;
            case 3: // Photos
                app_state_set(STATE_MENU_IMAGE_FILES);
                scan_directory(MEDIA_PATH_PHOTO);
                break;
            case 4: // Library
                app_state_set(STATE_MENU_PDF_FILES);
                scan_directory(MEDIA_PATH_PDF);
                break;
            case 5: // YouTube
                app_state_set(STATE_MENU_YOUTUBE);
                break;
            case 6: // Settings
                app_state_set(STATE_MENU_SETTINGS);
                break;
        }
    }
}
