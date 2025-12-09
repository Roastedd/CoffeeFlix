This is a comprehensive, detailed, and accurate guide for implementing robust, state-driven controller navigation (GamePad, Wii Remote, and Pro Controller) into a Wii U homebrew application with a complex UI structure, like the one shown in your CafeMP screenshot (Horizontal Tabs + Vertical Lists/Content).

The solution centers on establishing a Focus System that determines which UI element receives the input.

Wii U Controller Navigation Guide (CafeMP Focus System)
1. The Core Input Pipeline (VPAD & KPAD)

For the most accurate and responsive UI, you should use the WUT libraries directly (vpad.h and padscore.h) rather than relying solely on SDL's generic joystick events.

1.1 Essential Headers and Initialization
code
C++
download
content_copy
expand_less
#include <vpad/input.h>        // GamePad Input (VPAD)
#include <padscore/wpad.h>     // Wii Remote & Extensions Init (WPAD)
#include <padscore/kpad.h>     // Wii Remote & Extensions Read (KPAD)
#include <whb/proc.h>          // Main Loop Check

// Global Input Data Structures
VPADStatus vpad_data;
VPADReadError vpad_error;
uint32_t button_triggers;      // Buttons *just pressed* this frame

void InitAllControllers() {
    // 1. Initialize GamePad
    VPADInit(); 

    // 2. Initialize Wii Remote/Pro Controller System
    WPADInit();
    KPADInit();
    WPADEnableURCC(1); // Enable Wii U Remote Controller communication (Pro/Classic support)
}

void UpdateControllerInput() {
    // Read GamePad (Channel 0 is always the GamePad)
    VPADRead(VPAD_CHAN_0, &vpad_data, 1, &vpad_error);
    if (vpad_error == VPAD_READ_SUCCESS) {
        // Crucial: Use 'trigger' for single-shot actions (e.g., advancing a menu item)
        button_triggers = vpad_data.trigger;
    } else {
        button_triggers = 0; // Clear triggers if controller is disconnected
    }
    
    // Read Wii Remotes/Extensions (Handled in Section 4)
    HandleWiiMoteInput(); 
}
2. The UI Focus System: Defining State

The controller does not move a cursor; it changes these variables, and your rendering code reacts.

2.1 State Definitions (Based on your Screenshot)
code
C++
download
content_copy
expand_less
// --- Global State Enums ---
enum FocusArea {
    FOCUS_TAB_BAR,          // Horizontal buttons (Setting, Debug, etc.)
    FOCUS_SIDEBAR_LIST,     // The "What's New" menu/sidebar
    FOCUS_MAIN_VIEW,        // The main content area (if it has buttons/input)
    FOCUS_DIALOG_BOX,       // Modal window (e.g., Exit confirmation)
    FOCUS_NONE              // For video playback mode
};

// --- Global State Variables ---
FocusArea currentFocus = FOCUS_TAB_BAR; // Start at the top bar

// Horizontal Tab Bar State
const int NUM_TABS = 6; // Audio, Photos, Library, YouTu, Debug, Setting
int selectedTabIndex = 0; // 0 = Setting, 1 = Debug, ... 5 = Audio

// Vertical List/Sidebar State (for "What's New")
const int SIDEBAR_ITEM_COUNT = 10; // Example: total items in the sidebar
int selectedSidebarIndex = 0;      // Current item index (0 to 9)
int sidebarScrollOffset = 0;       // For visual scrolling of large lists

// Scrollbar State (for the - button functionality)
bool isScrollbarVisible = true;
3. GamePad Navigation Logic (The Engine)

This function contains the logic to transition between states and indices. It should be called immediately after UpdateControllerInput().

3.1 Focus Switching (L/R Buttons)

We use the shoulder buttons (L/R or ZL/ZR) to make a vertical jump between the major UI zones (Tab Bar to Sidebar/List).

code
C++
download
content_copy
expand_less
void HandleFocusSwitch(uint32_t triggers) {
    if (triggers & VPAD_BUTTON_L) { // Go "Up" to the Tab Bar
        if (currentFocus == FOCUS_SIDEBAR_LIST) {
            currentFocus = FOCUS_TAB_BAR;
        } 
    }
    else if (triggers & VPAD_BUTTON_R) { // Go "Down" to the Sidebar/List
        if (currentFocus == FOCUS_TAB_BAR) {
            currentFocus = FOCUS_SIDEBAR_LIST;
        }
    }
}
3.2 Focused Area Navigation
code
C++
download
content_copy
expand_less
void HandleNavigation() {
    
    // First, handle the overall focus switch
    HandleFocusSwitch(button_triggers);
    
    // Second, handle navigation within the currently focused area
    switch (currentFocus) {
        
        case FOCUS_TAB_BAR:
            // --- Horizontal Navigation (D-Pad Left/Right) ---
            if (button_triggers & VPAD_BUTTON_RIGHT) {
                selectedTabIndex = (selectedTabIndex + 1) % NUM_TABS;
            }
            else if (button_triggers & VPAD_BUTTON_LEFT) {
                selectedTabIndex = (selectedTabIndex - 1 + NUM_TABS) % NUM_TABS;
            }
            
            // --- Action (A Button) ---
            if (button_triggers & VPAD_BUTTON_A) {
                // Call the function associated with the selected tab
                ExecuteTabAction(selectedTabIndex); 
            }
            break;

        case FOCUS_SIDEBAR_LIST:
            // --- Vertical Navigation (D-Pad Up/Down) ---
            if (button_triggers & VPAD_BUTTON_DOWN) {
                if (selectedSidebarIndex < SIDEBAR_ITEM_COUNT - 1) {
                    selectedSidebarIndex++;
                }
            }
            else if (button_triggers & VPAD_BUTTON_UP) {
                if (selectedSidebarIndex > 0) {
                    selectedSidebarIndex--;
                }
            }
            
            // --- Accelerated Scrolling (Left Stick) ---
            // Use 'hold' or 'leftStick' value to implement fast scrolling
            if (vpad_data.leftStick.y < -0.6f) { // Stick pushed down hard
                 sidebarScrollOffset += 5; // Large scroll increment
            } else if (vpad_data.leftStick.y > 0.6f) { // Stick pushed up hard
                 sidebarScrollOffset -= 5;
            }
            
            // --- Action (A Button) ---
            if (button_triggers & VPAD_BUTTON_A) {
                ExecuteSidebarAction(selectedSidebarIndex);
            }
            break;
            
        // ... (Implement other FocusArea cases)
    }
    
    // --- Global Utility Button: Scrollbar Toggle ---
    if (button_triggers & VPAD_BUTTON_MINUS) {
        isScrollbarVisible = !isScrollbarVisible; // Toggle the state
    }
}
4. Multi-Controller Support (Wii Remote / Pro Controller)

To support the Pro Controller, you read the input via the KPAD library, which provides the same event structures.

4.1 Wii U Pro Controller Mapping

The Pro Controller is the easiest to map because its button layout is identical to the GamePad.

code
C++
download
content_copy
expand_less
void HandleWiiMoteInput() {
    for (int chan = 0; chan < 4; chan++) {
        KPADStatus kpad;
        KPADRead(chan, &kpad, 1);
        
        if (kpad.extensionType == WPAD_EXT_PRO_CONTROLLER && kpad.deviceType == 1) {
            
            // Pro Controller Button Mappings (very similar to VPAD)
            uint32_t triggers = kpad.trigger; 
            
            // You can re-use the VPAD logic, just substitute the button constants
            if (triggers & KPAD_BUTTON_RIGHT) { /* ... logic ... */ }
            if (triggers & KPAD_BUTTON_A) { /* ... logic ... */ }

            // Since the logic is the same, create a wrapper function:
            // HandleNavigationFromKPAD(triggers, kpad.leftStick);
            
        } 
        // Add logic for Classic Controller (WPAD_EXT_CLASSIC) here if needed
    }
}
5. Visual Feedback: The Focus Box

The core of a successful console UI is immediate, visual feedback. You must draw a Focus Box or change the color of the element determined by the state variables.

5.1 Rendering the Tab Bar Focus
code
C++
download
content_copy
expand_less
// Assumes you are using SDL2 or a GX2 equivalent to draw the UI

void DrawTabBar(SDL_Renderer* renderer, const SDL_Rect tab_positions[]) {
    
    for (int i = 0; i < NUM_TABS; i++) {
        
        // 1. Check for Focus (DRAW FOCUS BOX FIRST)
        if (currentFocus == FOCUS_TAB_BAR && i == selectedTabIndex) {
            // Draw a slightly larger, bright rectangle around the current tab
            SDL_Rect focusRect = tab_positions[i];
            focusRect.x -= 5; focusRect.y -= 5; 
            focusRect.w += 10; focusRect.h += 10;
            
            SDL_SetRenderDrawColor(renderer, 255, 255, 0, 150); // Semi-transparent yellow
            SDL_RenderFillRect(renderer, &focusRect);
        }
        
        // 2. Draw the actual button background and text
        // (Draw button texture/color)
        // (Draw button text)
    }
}
5.2 Rendering the Sidebar/List Focus

For vertical lists, you must factor in the scroll offset.

code
C++
download
content_copy
expand_less
void DrawSidebarList(SDL_Renderer* renderer, const SDL_Rect list_area) {
    const int ITEM_HEIGHT = 40;
    
    for (int i = 0; i < SIDEBAR_ITEM_COUNT; i++) {
        
        // Calculate the drawing position of this item
        SDL_Rect itemRect = {
            list_area.x,
            list_area.y + (i * ITEM_HEIGHT) - sidebarScrollOffset, // Apply scroll offset
            list_area.w,
            ITEM_HEIGHT
        };
        
        // Culling: Only draw items visible within the 'list_area'
        if (itemRect.y + itemRect.h < list_area.y || itemRect.y > list_area.y + list_area.h) {
            continue; // Skip drawing off-screen items
        }
        
        // 1. Check for Focus
        if (currentFocus == FOCUS_SIDEBAR_LIST && i == selectedSidebarIndex) {
            // Draw a solid, bright background color for the selected item
            SDL_SetRenderDrawColor(renderer, 50, 50, 200, 255); // Solid Blue
            SDL_RenderFillRect(renderer, &itemRect);
        }
        
        // 2. Draw item text over the background
        // (Draw the item name, e.g., "What's New")
    }
}
5.3 Rendering the Scrollbar Toggle

You must use the isScrollbarVisible state in your rendering code to show or hide the visual scrollbar element.

code
C++
download
content_copy
expand_less
void DrawScrollBar(SDL_Renderer* renderer, const SDL_Rect scrollbar_area) {
    if (isScrollbarVisible) {
        // Draw the scrollbar track and thumb here
        // The thumb position is determined by (sidebarScrollOffset / maxScrollOffset)
    }
}