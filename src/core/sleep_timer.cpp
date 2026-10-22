#include "core/sleep_timer.hpp"

#include <algorithm>

namespace sleep_timer {

namespace {
struct State {
    bool on = false;
    int minutes = 0;          // as started
    double left = 0;          // seconds, minutes mode
    double waited = 0;        // seconds waiting for the end of an item
    bool blocked = false;     // stopped at the end of an item that is still showing
};
State g;
constexpr double END_GIVE_UP = 6 * 3600;  // "at the end of this one" waiting for hours for an end that does not come
}  // namespace

void start(int minutes) {
    g.on = minutes == END_OF_ITEM || minutes > 0;
    g.minutes = minutes;
    g.left = minutes > 0 ? minutes * 60.0 : 0;
    g.waited = 0;
    g.blocked = false;
}

void cancel() {
    g.on = false;
    g.blocked = false;
}

bool active() { return g.on; }
bool at_end_of_item() { return g.on && g.minutes == END_OF_ITEM; }
double remaining_seconds() { return g.on && g.minutes > 0 ? std::max(0.0, g.left) : 0; }
int chosen_minutes() { return g.on ? g.minutes : 0; }

bool update(double dt, bool item_ended) {
    if (!g.on) return false;
    if (g.minutes == END_OF_ITEM) {
        g.waited += dt;
        if (item_ended) {
            g.on = false;
            g.blocked = true;
            return true;
        }
        if (g.waited > END_GIVE_UP) g.on = false;
        return false;
    }
    g.left -= dt;
    if (g.left <= 0) {
        g.on = false;
        return true;
    }
    return false;
}

bool end_pending() { return at_end_of_item() || g.blocked; }
void item_left() { g.blocked = false; }

float fade() {
    if (!g.on || g.minutes <= 0) return 1.0f;
    if (g.left >= FADE_SECONDS) return 1.0f;
    const float k = (float)std::max(0.0, g.left / FADE_SECONDS);
    return k * k * (3 - 2 * k);  // a smooth ramp, not a corner
}

}  // namespace sleep_timer
