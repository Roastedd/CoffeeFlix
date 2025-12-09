#ifndef APPLE_THEME_HPP
#define APPLE_THEME_HPP

// Forward declarations
struct nk_context;
struct nk_color;
struct nk_vec2;
struct nk_style_button;

namespace AppleTheme {
    // Apple Design System Colors (Enhanced Dark Mode)
    
    // System Colors
    extern const struct nk_color SYSTEM_BLUE;
    extern const struct nk_color SYSTEM_GREEN;
    extern const struct nk_color SYSTEM_RED;
    extern const struct nk_color SYSTEM_ORANGE;
    extern const struct nk_color SYSTEM_YELLOW;
    extern const struct nk_color SYSTEM_PURPLE;
    extern const struct nk_color SYSTEM_PINK;
    
    // Background Colors
    extern const struct nk_color BG_PRIMARY;
    extern const struct nk_color BG_SECONDARY;
    extern const struct nk_color BG_TERTIARY;
    extern const struct nk_color BG_ELEVATED;
    
    // Label Colors
    extern const struct nk_color LABEL_PRIMARY;
    extern const struct nk_color LABEL_SECONDARY;
    extern const struct nk_color LABEL_TERTIARY;
    
    // Fill Colors
    extern const struct nk_color FILL_PRIMARY;
    extern const struct nk_color FILL_SECONDARY;
    extern const struct nk_color FILL_TERTIARY;
    
    // Separator
    extern const struct nk_color SEPARATOR;
    
    // Design Metrics
    extern const float CORNER_RADIUS;
    extern const float BUTTON_CORNER_RADIUS;
    extern const float CARD_CORNER_RADIUS;
    extern const float PADDING;
    extern const float SPACING;
    extern const float BUTTON_HEIGHT;
    
    /**
     * Apply Apple-inspired theme to Nuklear context
     */
    void apply_theme(struct nk_context* ctx, float ui_scale = 1.0f);
    
    /**
     * Get a button style with Apple design
     */
    struct nk_style_button get_primary_button_style(struct nk_context* ctx);
    struct nk_style_button get_secondary_button_style(struct nk_context* ctx);
    struct nk_style_button get_destructive_button_style(struct nk_context* ctx);
    
    /**
     * Get window style with rounded corners and proper spacing
     */
    void apply_window_style(struct nk_context* ctx, float ui_scale = 1.0f);
}

#endif // APPLE_THEME_HPP
