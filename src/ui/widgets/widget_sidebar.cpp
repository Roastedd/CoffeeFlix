
#ifdef DEBUG
#include "shader/easter_egg.hpp"
#endif
#include "main.hpp"
#include "utils/app_state.hpp"
#include "utils/utils.hpp"
#include "ui/apple_theme.hpp"
#include "ui/focus_system.hpp"
#include "vendor/ui/nuklear.h"
#include "utils/sdl.hpp"
#include "logger/logger.hpp"

#include <SDL2/SDL_image.h>

#include "widget_sidebar.hpp"

// Sidebar visibility state
static bool sidebar_visible = true;  // Visible by default
static float sidebar_width = 220 * UI_SCALE;  // Slightly wider for better touch targets

// Icon textures and images
static bool icons_loaded = false;
struct IconEntry {
    const char* path;
    SDL_Texture* texture;
    struct nk_image image;
};

static IconEntry g_icons[] = {
    {"content/icons/home_icon.png", nullptr, {}},
    {"content/icons/movie_icon.png", nullptr, {}},
    {"content/icons/music_note_icon.png", nullptr, {}},
    {"content/icons/image_icon.png", nullptr, {}},
    {"content/icons/radio_icon.png", nullptr, {}},
    {"content/icons/settings_icon.png", nullptr, {}},
};

static void ensure_icons_loaded() {
    if (icons_loaded) return;
    SDL_Renderer* renderer = sdl_get()->sdl_renderer;
    if (!renderer) return;

    for (auto& entry : g_icons) {
        SDL_Surface* surface = IMG_Load(entry.path);
        if (!surface) {
            log_message(LOG_ERROR, "UI", "Failed to load icon %s: %s", entry.path, IMG_GetError());
            continue;
        }
        entry.texture = SDL_CreateTextureFromSurface(renderer, surface);
        SDL_SetTextureBlendMode(entry.texture, SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(surface);
        if (entry.texture) entry.image = nk_image_ptr(entry.texture);
    }
    icons_loaded = true;
}

bool widget_sidebar_is_visible() {
    return sidebar_visible;
}

void widget_sidebar_toggle() {
    sidebar_visible = !sidebar_visible;
}

void widget_sidebar_set_visible(bool visible) {
    sidebar_visible = visible;
}

float widget_sidebar_get_width() {
    return sidebar_visible ? sidebar_width : 0;
}

void widget_sidebar_render(struct nk_context *ctx) {
    ensure_icons_loaded();

    if (!sidebar_visible) return;  // Don't render if hidden
    
    // Clean, functional sidebar with improved spacing
    float button_height = 58 * UI_SCALE;  // Larger for better touch interaction   
    
    nk_layout_row_push(ctx, sidebar_width);
    if (nk_group_begin(ctx, "Sidebar", NK_WINDOW_BORDER)) {
        nk_layout_row_dynamic(ctx, button_height, 1);
        
        // More spacing at top for better visual breathing room
        nk_spacing(ctx, 2);

        // Get current state for highlighting
        AppState current_state = app_state_get();
        bool sidebar_has_focus = focus_system_should_highlight_sidebar();
        int focused_index = focus_system_get_sidebar_selection();
        
        auto icon_or_null = [&](size_t idx) -> const struct nk_image* {
            if (idx < (sizeof(g_icons)/sizeof(g_icons[0])) && g_icons[idx].texture) return &g_icons[idx].image;
            return nullptr;
        };
        
        // Helper lambda to style buttons based on active state and focus
        int button_index = 0;
        auto render_nav_button = [&](const char* label, const struct nk_image* icon, AppState target_state, auto action) {
            struct nk_style_button old_style = ctx->style.button;
            
            // Show focus indicator when sidebar has focus
            if (sidebar_has_focus && button_index == focused_index) {
                // Focus ring - bright yellow outline
                ctx->style.button.border_color = nk_rgb(255, 204, 0);
                ctx->style.button.border = 6.0f * UI_SCALE;
                ctx->style.button.rounding = 12.0f * UI_SCALE;
            }
            else if (current_state == target_state) {
                // Active/selected state - subtle orange highlight
                ctx->style.button.border_color = nk_rgb(255, 149, 0);
                ctx->style.button.border = 3.0f * UI_SCALE;
            }
            
            button_index++;
            
            nk_layout_row_dynamic(ctx, button_height, 1);
            if (icon && icon->handle.ptr) {
                if (nk_button_image_label(ctx, *icon, label, NK_TEXT_LEFT)) {
                    action();
                }
            } else {
                if (nk_button_label(ctx, label)) {
                    action();
                }
            }
            
            ctx->style.button = old_style;
        };

        button_index = 0;  // Reset for each render
        render_nav_button("Home", icon_or_null(0), STATE_MENU, []() {
            app_state_set(STATE_MENU);
        });
        
        render_nav_button("Video", icon_or_null(1), STATE_MENU_VIDEO_FILES, []() {
            app_state_set(STATE_MENU_VIDEO_FILES);
            scan_directory(MEDIA_PATH_VIDEO);
        });
        
        render_nav_button("Audio", icon_or_null(2), STATE_MENU_AUDIO_FILES, []() {
            app_state_set(STATE_MENU_AUDIO_FILES);
            scan_directory(MEDIA_PATH_AUDIO);
        });
        
        render_nav_button("Photos", icon_or_null(3), STATE_MENU_IMAGE_FILES, []() {
            app_state_set(STATE_MENU_IMAGE_FILES);
            scan_directory(MEDIA_PATH_PHOTO);
        });
        
        render_nav_button("Library", icon_or_null(4), STATE_MENU_PDF_FILES, []() {
            app_state_set(STATE_MENU_PDF_FILES);
            scan_directory(MEDIA_PATH_PDF);
        });
        
        render_nav_button("YouTube", nullptr, STATE_MENU_YOUTUBE, []() {
            app_state_set(STATE_MENU_YOUTUBE);
        });

#ifdef DEBUG
        render_nav_button("Debug", nullptr, STATE_MENU_EASTER_EGG, []() {
            app_state_set(STATE_MENU_EASTER_EGG);
            easter_egg_init();
        });
#endif
        
        render_nav_button("Settings", icon_or_null(5), STATE_MENU_SETTINGS, []() {
            app_state_set(STATE_MENU_SETTINGS);
        });

        nk_group_end(ctx);
    }
}
