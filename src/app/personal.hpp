// What each person chose for their own Home: the page CoffeeFlix opens to, and which services show on the
// side menu. Kept per profile, by stable names (never the Section numbers, which differ between builds and
// change when a section is added).
#pragma once

#include <string>
#include <string_view>

#include "app/app.hpp"

namespace personal {

struct SectionInfo {
    const char* id;          // stored
    app::Section section;
    const char* label;       // shown (translated by the caller)
    bool can_hide;           // Home and Settings stay
    bool can_start;          // may be the page CoffeeFlix opens to
};

extern const SectionInfo SECTIONS[];
extern const size_t SECTION_COUNT;
const SectionInfo* info(app::Section s);
const SectionInfo* info_for_id(std::string_view id);

// Services turned off for the profile in use. Home and Settings can't be.
bool hidden(app::Section s);
void set_hidden(app::Section s, bool hide);
int hidden_count();
void show_all();

// The section a saved play position belongs to (SEC_NONE: none in particular), so a turned-off service's
// Continue watching entries stay out of Home.
app::Section section_of_service(const std::string& service);

// "Reduce motion" in Settings > Make it yours: screens fade quickly instead of sliding, and the optional animations
// (the birthday one, the idle mug) show a still picture. Read from the store at most twice a second.
bool reduce_motion();

// Where CoffeeFlix opens: "home" and the other ids, or "last" (the page used last). An unknown or stored id of a
// page that is gone or hidden opens Home.
extern const char* const START_LAST;
std::string start_id();
void set_start_id(const std::string& id);
app::Section start_section();
// Remembered for "last" (called whenever a section opens).
void note_section(app::Section s);

}  // namespace personal
