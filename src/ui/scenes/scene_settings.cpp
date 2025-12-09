#include "ui/scenes/scene_settings.hpp"

#include "main.hpp"
#include "vendor/ui/nuklear.h"
#include "ui/widgets/widget_sidebar.hpp"
#include "ui/widgets/widget_tooltip.hpp"
#include "ui/apple_theme.hpp"
#include "settings/settings.hpp"
#include "logger/logger.hpp"

// Safe zones for TV overscan
const float SAFE_MARGIN_X = 64.0f * UI_SCALE;
const float SAFE_MARGIN_Y = 36.0f * UI_SCALE;

void scene_settings_render(struct nk_context *ctx) {
    float sidebar_width = widget_sidebar_get_width();
    float content_width = SCREEN_WIDTH - sidebar_width;
    
    if (nk_begin(ctx, VERSION_STRING, nk_rect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE), NK_WINDOW_NO_SCROLLBAR | NK_WINDOW_BORDER)) {
        nk_layout_row_begin(ctx, NK_STATIC, SCREEN_HEIGHT - TOOLTIP_BAR_HEIGHT * UI_SCALE, 2);
        widget_sidebar_render(ctx);

        nk_layout_row_push(ctx, content_width);
        if (nk_group_begin(ctx, "SettingsContent", NK_WINDOW_BORDER)) {
            // Apply safe zones
            float usable_width = content_width - (SAFE_MARGIN_X * 2);
            
            // Top margin
            nk_layout_row_dynamic(ctx, SAFE_MARGIN_Y, 1);
            nk_spacing(ctx, 1);
            
            // Hero section
            float hero_height = 55 * UI_SCALE;
            nk_layout_row_begin(ctx, NK_STATIC, hero_height, 1);
            nk_layout_row_push(ctx, usable_width);
            
            struct nk_style_text old_text = ctx->style.text;
            ctx->style.text.color = AppleTheme::LABEL_PRIMARY;
            nk_label(ctx, "Settings", NK_TEXT_CENTERED);
            ctx->style.text = old_text;
            nk_layout_row_end(ctx);
            
            // Spacing
            nk_layout_row_dynamic(ctx, 25 * UI_SCALE, 1);
            nk_spacing(ctx, 1);
            
            // Settings card
            nk_layout_row_begin(ctx, NK_STATIC, 350 * UI_SCALE, 1);
            nk_layout_row_push(ctx, usable_width);
            
            struct nk_style_window old_window = ctx->style.window;
            ctx->style.window.fixed_background = nk_style_item_color(AppleTheme::BG_SECONDARY);
            ctx->style.window.rounding = AppleTheme::CARD_CORNER_RADIUS * UI_SCALE;
            ctx->style.window.padding = nk_vec2(AppleTheme::PADDING * 2 * UI_SCALE, AppleTheme::PADDING * 1.5f * UI_SCALE);
            
            if (nk_group_begin(ctx, "SettingsCard", NK_WINDOW_BORDER)) {
                const settings_struct* settings = settings_get_all();
                
                // Section heading
                float heading_height = 40 * UI_SCALE;
                nk_layout_row_dynamic(ctx, heading_height, 1);
                
                struct nk_style_text section_text = ctx->style.text;
                section_text.color = AppleTheme::SYSTEM_BLUE;
                ctx->style.text = section_text;
                nk_label(ctx, "Application Settings", NK_TEXT_LEFT);
                ctx->style.text = old_text;
                
                // Background Music Toggle
                nk_layout_row_dynamic(ctx, 35 * UI_SCALE, 1);
                int bg_music = settings->background_music_enabled ? 1 : 0;
                if (nk_checkbox_label(ctx, "Background Music", &bg_music)) {
                    bool new_value = bg_music != 0;
                    settings_set(SETTINGS_BKG_MUSIC_ENABLED, &new_value);
                    settings_save();
                    log_message(LOG_OK, "Settings", "Background music %s", new_value ? "enabled" : "disabled");
                }
                
                // Spacing
                nk_layout_row_dynamic(ctx, 25 * UI_SCALE, 1);
                nk_spacing(ctx, 1);
                
                // Info section
                nk_layout_row_dynamic(ctx, 35 * UI_SCALE, 1);
                section_text.color = AppleTheme::LABEL_SECONDARY;
                ctx->style.text = section_text;
                nk_label(ctx, "Version: " VERSION_STRING, NK_TEXT_LEFT);
                nk_label(ctx, "Press L or R to toggle the sidebar", NK_TEXT_LEFT);
                nk_label(ctx, "Use (-) button to rescan media files", NK_TEXT_LEFT);
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
