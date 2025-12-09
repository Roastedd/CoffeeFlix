#ifndef FOCUS_SYSTEM_HPP
#define FOCUS_SYSTEM_HPP

#include "input/input_actions.hpp"

// Focus areas based on the navigation guide
enum FocusArea {
    FOCUS_SIDEBAR,          // The sidebar menu (Home, Video, Audio, etc.)
    FOCUS_MAIN_CONTENT,     // Main content area (file lists, settings, etc.)
    FOCUS_DIALOG,           // Modal dialogs
    FOCUS_VIDEO_CONTROLS,   // Video player controls
    FOCUS_NONE              // No focus (video playback mode)
};

// Focus system state
struct FocusState {
    FocusArea current_area = FOCUS_SIDEBAR;
    int sidebar_selected_index = 0;
    int content_selected_index = 0;
    int content_scroll_offset = 0;
    bool sidebar_visible = true;
};

// Initialize the focus system
void focus_system_init();

// Get the current focus state
FocusState* focus_system_get_state();

// Handle input for focus navigation (L/R to switch areas)
void focus_system_handle_input(const InputState& input);

// Navigate within the current focus area
void focus_system_navigate(const InputState& input);

// Reset focus to default state
void focus_system_reset();

// Get visual feedback parameters
bool focus_system_should_highlight_sidebar();
bool focus_system_should_highlight_content();
int focus_system_get_sidebar_selection();
int focus_system_get_content_selection();

#endif // FOCUS_SYSTEM_HPP
