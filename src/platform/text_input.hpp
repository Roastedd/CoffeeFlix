// Text entry: the Wii U's system keyboard (swkbd) on console, physical
// keyboard typing on the desktop preview.
#pragma once

#include <string>

namespace platform {

struct TextInputOptions {
    std::string initial;
    std::string hint;
    std::string ok_label = "OK";
    bool password = false;
    bool url = false;  // prefer a layout suited to addresses
};

enum TextInputState { TEXT_IDLE, TEXT_ACTIVE, TEXT_DONE, TEXT_CANCELLED };

void text_input_start(const TextInputOptions& opts);
TextInputState text_input_state();
// Current text (live while typing on desktop, final value once DONE).
const std::string& text_input_value();
// True when the OS draws its own keyboard UI (Wii U).
bool text_input_native();
void text_input_cancel();
// Acknowledge a DONE/CANCELLED result and return to IDLE.
void text_input_reset();

}  // namespace platform
