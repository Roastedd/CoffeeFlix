#include "focus_system.hpp"
#include "logger/logger.hpp"
#include <algorithm>

static FocusState g_focus_state;

void focus_system_init() {
    g_focus_state = FocusState{};
    g_focus_state.current_area = FOCUS_SIDEBAR;
    g_focus_state.sidebar_selected_index = 0;
    g_focus_state.content_selected_index = 0;
    g_focus_state.content_scroll_offset = 0;
    g_focus_state.sidebar_visible = true;
    log_message(LOG_OK, "FocusSystem", "Initialized with sidebar focus");
}

FocusState* focus_system_get_state() {
    return &g_focus_state;
}

void focus_system_handle_input(const InputState& input) {
    // Handle focus area switching with L/R buttons
    if (input_pressed(input, BTN_L)) {
        if (g_focus_state.current_area == FOCUS_MAIN_CONTENT && g_focus_state.sidebar_visible) {
            g_focus_state.current_area = FOCUS_SIDEBAR;
            log_message(LOG_DEBUG, "FocusSystem", "Switched focus to SIDEBAR");
        }
    }
    else if (input_pressed(input, BTN_R)) {
        if (g_focus_state.current_area == FOCUS_SIDEBAR) {
            g_focus_state.current_area = FOCUS_MAIN_CONTENT;
            log_message(LOG_DEBUG, "FocusSystem", "Switched focus to MAIN_CONTENT");
        }
    }
}

void focus_system_navigate(const InputState& input) {
    switch (g_focus_state.current_area) {
        case FOCUS_SIDEBAR: {
            // Vertical navigation in sidebar (7 items: Home, Video, Audio, Photos, Library, YouTube, Settings)
            const int SIDEBAR_ITEM_COUNT = 7;
            
            if (input_pressed(input, BTN_DOWN) || (input.left_stick.y < -0.5f && input_pressed(input, BTN_LSTICK_DOWN))) {
                g_focus_state.sidebar_selected_index = 
                    std::min(g_focus_state.sidebar_selected_index + 1, SIDEBAR_ITEM_COUNT - 1);
                log_message(LOG_DEBUG, "FocusSystem", "Sidebar index: %d", g_focus_state.sidebar_selected_index);
            }
            else if (input_pressed(input, BTN_UP) || (input.left_stick.y > 0.5f && input_pressed(input, BTN_LSTICK_UP))) {
                g_focus_state.sidebar_selected_index = 
                    std::max(g_focus_state.sidebar_selected_index - 1, 0);
                log_message(LOG_DEBUG, "FocusSystem", "Sidebar index: %d", g_focus_state.sidebar_selected_index);
            }
            break;
        }
        
        case FOCUS_MAIN_CONTENT: {
            // Vertical navigation in content lists
            if (input_pressed(input, BTN_DOWN)) {
                g_focus_state.content_selected_index++;
                log_message(LOG_DEBUG, "FocusSystem", "Content index: %d", g_focus_state.content_selected_index);
            }
            else if (input_pressed(input, BTN_UP)) {
                g_focus_state.content_selected_index = std::max(g_focus_state.content_selected_index - 1, 0);
                log_message(LOG_DEBUG, "FocusSystem", "Content index: %d", g_focus_state.content_selected_index);
            }
            
            // Fast scrolling with analog stick
            if (input.left_stick.y < -0.6f) {
                g_focus_state.content_scroll_offset += 5;
            } else if (input.left_stick.y > 0.6f) {
                g_focus_state.content_scroll_offset = std::max(g_focus_state.content_scroll_offset - 5, 0);
            }
            break;
        }
        
        default:
            break;
    }
}

void focus_system_reset() {
    g_focus_state.current_area = FOCUS_SIDEBAR;
    g_focus_state.sidebar_selected_index = 0;
    g_focus_state.content_selected_index = 0;
    g_focus_state.content_scroll_offset = 0;
}

bool focus_system_should_highlight_sidebar() {
    return g_focus_state.current_area == FOCUS_SIDEBAR;
}

bool focus_system_should_highlight_content() {
    return g_focus_state.current_area == FOCUS_MAIN_CONTENT;
}

int focus_system_get_sidebar_selection() {
    return g_focus_state.sidebar_selected_index;
}

int focus_system_get_content_selection() {
    return g_focus_state.content_selected_index;
}
