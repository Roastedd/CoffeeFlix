#include "ui/apple_theme.hpp"
#include "vendor/ui/nuklear.h"
#include "main.hpp"

namespace AppleTheme {

// Apple Design System Colors (Enhanced Dark Mode with better contrast)
const struct nk_color SYSTEM_BLUE = {10, 132, 255, 255};         // #0A84FF Brighter blue
const struct nk_color SYSTEM_GREEN = {48, 209, 88, 255};         // #30D158 Vibrant green
const struct nk_color SYSTEM_RED = {255, 69, 58, 255};           // #FF453A Bright red
const struct nk_color SYSTEM_ORANGE = {255, 159, 10, 255};       // #FF9F0A Warm orange
const struct nk_color SYSTEM_YELLOW = {255, 214, 10, 255};       // #FFD60A Bright yellow
const struct nk_color SYSTEM_PURPLE = {191, 90, 242, 255};       // #BF5AF2 Rich purple
const struct nk_color SYSTEM_PINK = {255, 55, 95, 255};          // #FF375F Vibrant pink

// Background Colors (Deeper, more modern)
const struct nk_color BG_PRIMARY = {18, 18, 20, 255};            // #121214 Deeper dark
const struct nk_color BG_SECONDARY = {38, 38, 42, 255};          // #26262A Elevated
const struct nk_color BG_TERTIARY = {54, 54, 58, 255};           // #36363A Card background
const struct nk_color BG_ELEVATED = {68, 68, 72, 255};           // #444448 Hover state

// Label Colors (Better contrast)
const struct nk_color LABEL_PRIMARY = {255, 255, 255, 255};      // Pure white
const struct nk_color LABEL_SECONDARY = {235, 235, 245, 178};    // 70% opacity (improved from 60%)
const struct nk_color LABEL_TERTIARY = {235, 235, 245, 102};     // 40% opacity (improved from 30%)

// Fill Colors
const struct nk_color FILL_PRIMARY = {120, 120, 128, 92};         // Gray with opacity
const struct nk_color FILL_SECONDARY = {120, 120, 128, 51};
const struct nk_color FILL_TERTIARY = {118, 118, 128, 38};

// Separator (More visible)
const struct nk_color SEPARATOR = {99, 99, 102, 178};             // #63636680 Brighter separator

// Design Metrics - Optimized for Wii U gamepad and TV with modern spacing
const float CORNER_RADIUS = 14.0f;          // Slightly more rounded
const float BUTTON_CORNER_RADIUS = 12.0f;   // Rounder buttons
const float CARD_CORNER_RADIUS = 16.0f;     // More rounded cards
const float PADDING = 22.0f;                // More generous padding
const float SPACING = 14.0f;                // Better breathing room
const float BUTTON_HEIGHT = 58.0f;          // Larger buttons for better touch
const float CONTROL_HEIGHT = 50.0f;         // Larger controls

void apply_theme(struct nk_context* ctx, float ui_scale) {
    struct nk_style* s = &ctx->style;
    
    // Window styling - Optimized for readability
    s->window.background = BG_PRIMARY;
    s->window.fixed_background = nk_style_item_color(BG_PRIMARY);
    s->window.border_color = SEPARATOR;
    s->window.combo_border_color = SEPARATOR;
    s->window.contextual_border_color = SEPARATOR;
    s->window.menu_border_color = SEPARATOR;
    s->window.group_border_color = SEPARATOR;
    s->window.tooltip_border_color = SEPARATOR;
    s->window.scrollbar_size = nk_vec2(16 * ui_scale, 16 * ui_scale);  // Larger scrollbar
    s->window.border = 1.0f;
    s->window.rounding = CARD_CORNER_RADIUS * ui_scale;
    s->window.padding = nk_vec2(PADDING * ui_scale, PADDING * ui_scale);
    s->window.spacing = nk_vec2(SPACING * ui_scale, SPACING * ui_scale);
    s->window.group_padding = nk_vec2(PADDING * ui_scale, PADDING * ui_scale);
    s->window.min_size = nk_vec2(200 * ui_scale, 150 * ui_scale);
    
    // Window header - Larger for better visibility
    s->window.header.normal = nk_style_item_color(BG_SECONDARY);
    s->window.header.hover = nk_style_item_color(BG_TERTIARY);
    s->window.header.active = nk_style_item_color(BG_TERTIARY);
    s->window.header.label_normal = LABEL_PRIMARY;
    s->window.header.label_hover = LABEL_PRIMARY;
    s->window.header.label_active = LABEL_PRIMARY;
    s->window.header.padding = nk_vec2(PADDING * ui_scale, (PADDING * 0.75f) * ui_scale);
    s->window.header.spacing = nk_vec2(SPACING * ui_scale, SPACING * ui_scale);
    
    // Button styling (Primary - Brighter Blue) - Enhanced for better visibility
    s->button.normal = nk_style_item_color(SYSTEM_BLUE);
    s->button.hover = nk_style_item_color(nk_rgb(30, 152, 255));  // Brighter hover
    s->button.active = nk_style_item_color(nk_rgb(0, 112, 245));  // Slightly darker active
    s->button.border_color = nk_rgb(50, 162, 255);  // Lighter border
    s->button.text_background = SYSTEM_BLUE;
    s->button.text_normal = LABEL_PRIMARY;
    s->button.text_hover = LABEL_PRIMARY;
    s->button.text_active = LABEL_PRIMARY;
    s->button.rounding = BUTTON_CORNER_RADIUS * ui_scale;
    s->button.border = 0.0f;
    s->button.padding = nk_vec2(PADDING * 1.4f * ui_scale, PADDING * 0.8f * ui_scale);  // Better padding
    
    // Text styling - Better readability
    s->text.color = LABEL_PRIMARY;
    s->text.padding = nk_vec2(SPACING * ui_scale, SPACING * 0.5f * ui_scale);
    
    // Edit (text input) styling - Larger for better touch interaction
    s->edit.normal = nk_style_item_color(BG_TERTIARY);
    s->edit.hover = nk_style_item_color(BG_ELEVATED);
    s->edit.active = nk_style_item_color(BG_ELEVATED);
    s->edit.border_color = SEPARATOR;
    s->edit.cursor_normal = SYSTEM_BLUE;
    s->edit.cursor_hover = SYSTEM_BLUE;
    s->edit.cursor_text_normal = LABEL_PRIMARY;
    s->edit.cursor_text_hover = LABEL_PRIMARY;
    s->edit.text_normal = LABEL_PRIMARY;
    s->edit.text_hover = LABEL_PRIMARY;
    s->edit.text_active = LABEL_PRIMARY;
    s->edit.selected_normal = SYSTEM_BLUE;
    s->edit.selected_hover = SYSTEM_BLUE;
    s->edit.selected_text_normal = LABEL_PRIMARY;
    s->edit.selected_text_hover = LABEL_PRIMARY;
    s->edit.rounding = BUTTON_CORNER_RADIUS * ui_scale;
    s->edit.border = 1.0f;
    s->edit.padding = nk_vec2(PADDING * 0.8f * ui_scale, PADDING * 0.65f * ui_scale);  // More padding
    
    // Option (radio button) styling - Larger
    s->option.normal = nk_style_item_color(BG_TERTIARY);
    s->option.hover = nk_style_item_color(BG_ELEVATED);
    s->option.active = nk_style_item_color(SYSTEM_BLUE);
    s->option.border_color = SEPARATOR;
    s->option.cursor_normal = nk_style_item_color(SYSTEM_BLUE);
    s->option.cursor_hover = nk_style_item_color(SYSTEM_BLUE);
    s->option.text_normal = LABEL_PRIMARY;
    s->option.text_hover = LABEL_PRIMARY;
    s->option.text_active = LABEL_PRIMARY;
    s->option.text_background = BG_PRIMARY;
    s->option.padding = nk_vec2(SPACING * ui_scale, SPACING * ui_scale);
    s->option.spacing = 12 * ui_scale;  // More spacing
    s->option.border = 1.0f;
    
    // Checkbox styling - Larger for touch
    s->checkbox.normal = nk_style_item_color(BG_TERTIARY);
    s->checkbox.hover = nk_style_item_color(BG_ELEVATED);
    s->checkbox.active = nk_style_item_color(SYSTEM_BLUE);
    s->checkbox.cursor_normal = nk_style_item_color(SYSTEM_BLUE);
    s->checkbox.cursor_hover = nk_style_item_color(SYSTEM_BLUE);
    s->checkbox.text_normal = LABEL_PRIMARY;
    s->checkbox.text_hover = LABEL_PRIMARY;
    s->checkbox.text_active = LABEL_PRIMARY;
    s->checkbox.text_background = BG_PRIMARY;
    s->checkbox.border_color = SEPARATOR;
    s->checkbox.padding = nk_vec2(SPACING * 1.2f * ui_scale, SPACING * 1.2f * ui_scale);  // More padding
    s->checkbox.spacing = 12 * ui_scale;  // More spacing
    s->checkbox.border = 1.0f;
    
    // Scrollbar styling - Wider for easier grabbing
    s->scrollh.normal = nk_style_item_color(BG_SECONDARY);
    s->scrollh.hover = nk_style_item_color(BG_TERTIARY);
    s->scrollh.active = nk_style_item_color(BG_TERTIARY);
    s->scrollh.cursor_normal = nk_style_item_color(FILL_PRIMARY);
    s->scrollh.cursor_hover = nk_style_item_color(FILL_SECONDARY);
    s->scrollh.cursor_active = nk_style_item_color(FILL_SECONDARY);
    s->scrollh.border_color = SEPARATOR;
    s->scrollh.cursor_border_color = SEPARATOR;
    s->scrollh.rounding = 6 * ui_scale;  // More rounded
    s->scrollh.border = 0.0f;
    
    s->scrollv.normal = nk_style_item_color(BG_SECONDARY);
    s->scrollv.hover = nk_style_item_color(BG_TERTIARY);
    s->scrollv.active = nk_style_item_color(BG_TERTIARY);
    s->scrollv.cursor_normal = nk_style_item_color(FILL_PRIMARY);
    s->scrollv.cursor_hover = nk_style_item_color(FILL_SECONDARY);
    s->scrollv.cursor_active = nk_style_item_color(FILL_SECONDARY);
    s->scrollv.border_color = SEPARATOR;
    s->scrollv.cursor_border_color = SEPARATOR;
    s->scrollv.rounding = 6 * ui_scale;  // More rounded
    s->scrollv.border = 0.0f;
    
    // Property styling - Larger for better usability
    s->property.normal = nk_style_item_color(BG_TERTIARY);
    s->property.hover = nk_style_item_color(BG_ELEVATED);
    s->property.active = nk_style_item_color(BG_ELEVATED);
    s->property.border_color = SEPARATOR;
    s->property.label_normal = LABEL_PRIMARY;
    s->property.label_hover = LABEL_PRIMARY;
    s->property.label_active = LABEL_PRIMARY;
    s->property.rounding = BUTTON_CORNER_RADIUS * ui_scale;
    s->property.border = 1.0f;
    s->property.padding = nk_vec2(PADDING * 0.8f * ui_scale, PADDING * 0.65f * ui_scale);  // More padding
    
    // Chart styling - Better visibility
    s->chart.background = nk_style_item_color(BG_TERTIARY);
    s->chart.border_color = SEPARATOR;
    s->chart.selected_color = SYSTEM_BLUE;
    s->chart.color = SYSTEM_GREEN;
    s->chart.padding = nk_vec2(SPACING * ui_scale, SPACING * ui_scale);
    s->chart.border = 1.0f;
    s->chart.rounding = CORNER_RADIUS * ui_scale;
}

struct nk_style_button get_primary_button_style(struct nk_context* ctx) {
    struct nk_style_button button = ctx->style.button;
    button.normal = nk_style_item_color(SYSTEM_BLUE);
    button.hover = nk_style_item_color(nk_rgb(30, 152, 255));  // Brighter hover
    button.active = nk_style_item_color(nk_rgb(0, 112, 245));  // Slightly darker
    button.border_color = nk_rgb(50, 162, 255);
    button.text_normal = LABEL_PRIMARY;
    button.text_hover = LABEL_PRIMARY;
    button.text_active = LABEL_PRIMARY;
    button.rounding = BUTTON_CORNER_RADIUS * UI_SCALE;
    button.border = 0.0f;
    button.padding = nk_vec2(PADDING * 1.6f * UI_SCALE, PADDING * 0.9f * UI_SCALE);  // More generous padding
    return button;
}

struct nk_style_button get_secondary_button_style(struct nk_context* ctx) {
    struct nk_style_button button = ctx->style.button;
    button.normal = nk_style_item_color(BG_TERTIARY);
    button.hover = nk_style_item_color(BG_ELEVATED);
    button.active = nk_style_item_color(nk_rgb(78, 78, 82));  // Lighter active state
    button.border_color = SEPARATOR;
    button.text_normal = LABEL_PRIMARY;
    button.text_hover = LABEL_PRIMARY;
    button.text_active = LABEL_PRIMARY;
    button.rounding = BUTTON_CORNER_RADIUS * UI_SCALE;
    button.border = 1.5f;  // Slightly thicker border for better definition
    button.padding = nk_vec2(PADDING * 1.6f * UI_SCALE, PADDING * 0.9f * UI_SCALE);
    return button;
}

struct nk_style_button get_destructive_button_style(struct nk_context* ctx) {
    struct nk_style_button button = ctx->style.button;
    button.normal = nk_style_item_color(SYSTEM_RED);
    button.hover = nk_style_item_color(nk_rgb(255, 79, 68));
    button.active = nk_style_item_color(nk_rgb(235, 39, 28));
    button.border_color = SYSTEM_RED;
    button.text_normal = LABEL_PRIMARY;
    button.text_hover = LABEL_PRIMARY;
    button.text_active = LABEL_PRIMARY;
    button.rounding = BUTTON_CORNER_RADIUS * UI_SCALE;
    button.border = 0.0f;
    button.padding = nk_vec2(PADDING * 1.5f * UI_SCALE, PADDING * 0.8f * UI_SCALE);  // Even more padding
    return button;
}

void apply_window_style(struct nk_context* ctx, float ui_scale) {
    ctx->style.window.background = BG_PRIMARY;
    ctx->style.window.fixed_background = nk_style_item_color(BG_PRIMARY);
    ctx->style.window.border_color = SEPARATOR;
    ctx->style.window.rounding = CARD_CORNER_RADIUS * ui_scale;
    ctx->style.window.padding = nk_vec2(PADDING * ui_scale, PADDING * ui_scale);
    ctx->style.window.spacing = nk_vec2(SPACING * ui_scale, SPACING * ui_scale);
}

} // namespace AppleTheme
