// What was watched and listened to, kept on this console for each profile and nowhere else: time by day and
// service, a count of what was watched, the titles most watched and how often CoffeeFlix was opened. Nothing
// is sent, logged or exported. Written at most once a minute (and when playback stops), as a few kilobytes.
#pragma once

#include <string>
#include <vector>

namespace stats {

// What is playing, as the player knows it.
struct Played {
    std::string service, id, title;
    double duration = 0;   // seconds; 0 when not known (live)
    bool video = true;
    bool live = false;
};

bool enabled();                 // for the profile in use: on unless turned off
void set_enabled(bool on);
// `seconds` of play, a few at a time. A title counts as watched, once for a run, after 80% of it or 5 minutes,
// whichever is less, and never under 2 minutes.
void playing(const Played& p, double seconds);
void app_opened();              // once, as CoffeeFlix starts
void tick();                    // from the main loop: writes what changed, at most once a minute
void flush();                   // writes it now (playback stopped, closing)
void reset();                   // forgets everything for the profile in use

struct Totals {
    double seconds = 0;
    int watched = 0;
};
struct Share {
    std::string service;        // as the player names it; label() turns it into words
    double seconds = 0;
};
struct Title {
    std::string title, service;
    int plays = 0;
    double seconds = 0;
};
struct Report {
    bool any = false;           // anything recorded at all
    Totals week, month, all;    // the last 7 days, the last 30 days (today included), everything
    int opened = 0;
    std::string since;          // "2026-10-15", the first day recorded
    std::vector<Share> services;  // this month, most time first
    std::vector<Title> top;       // most time first, at most 5
};
// The profile in use, or another (a grown-up looking at a kids profile's).
Report report(const std::string& profile = "");

// Pieces for showing it.
const char* label(const std::string& service);               // "YouTube", "My Media", translated
std::string rounded(double seconds);                          // "About 2.5 h", "About 40 min", "Under a minute"

}  // namespace stats
