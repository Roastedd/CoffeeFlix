// The sounds of the interface, made in code. Two sets: Subtle, soft and short and all in one family (D major
// pentatonic: a wooden tick, a bell pair going up for select, a soft blip going down for back, a muted thud, two
// ticks for a switch, a gentle pair of falling notes for an error), and Classic, the plain beeps CoffeeFlix had
// before. Each is mono at 48 kHz, starts and ends at exactly zero, and has no DC. Nothing here plays: the mixer
// takes them from here.
#pragma once

#include <vector>

namespace audio {

enum Sfx {
    SFX_MOVE, SFX_SELECT, SFX_BACK, SFX_BUMP, SFX_TOGGLE, SFX_ERROR, SFX_OPEN,
    SFX_TOGGLE_OFF,  // the switch going off (SFX_TOGGLE is on)
    SFX_SPARKLE,     // a birthday
    SFX_SHUFFLE,     // Surprise me
    SFX_COUNT
};
enum SoundStyle { STYLE_SUBTLE = 0, STYLE_CLASSIC = 1 };

namespace ui_sounds {

constexpr int RATE = 48000;

// How many versions of the sound there are (the tick comes in three pitches a few cents apart).
int variants(Sfx s, SoundStyle style);
std::vector<float> render(Sfx s, SoundStyle style, int variant = 0);

// A held D-pad ticks ten times a second: this thins it out. Ask for each tick as it is wanted; it says whether to
// play it and how much quieter (in dB, 0 or below).
class TickLimiter {
public:
    bool allow(double now_seconds, float* gain_db);

private:
    static constexpr int MAX = 32;
    double attempts_[MAX] = {};
    int count_ = 0;
    double last_played_ = -1;
    long total_ = 0;
};

}  // namespace ui_sounds
}  // namespace audio
