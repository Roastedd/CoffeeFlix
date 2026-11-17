// What was chosen for sound: the interface sounds (Subtle, Classic or off), how loud they are, the tick when the
// selector moves apart from the rest, and how loud the pictures' own sound is. Applied to the mixer from here.
#pragma once

namespace sound_settings {

enum Mode { SUBTLE = 0, CLASSIC = 1, OFF = 2 };

Mode mode();
void set_mode(Mode m);
float volume();                 // the interface sounds, 0..1 on the slider (70% unless chosen)
void set_volume(float v);
bool ticks();
void set_ticks(bool on);
float media_volume();           // video, music and radio, 0..2: 1 is as it came (100% unless chosen), 2 twice as loud
void set_media_volume(float v);
// The same as a slider's 0..1 position over the whole range (100% is halfway).
float media_slider();
void set_media_slider(float pos);
// Louder (steps > 0) or quieter by 5% a step, kept on the 5% grid; returns the new level.
float nudge_media_volume(int steps);
// Silent, or back to the level before (100% if there was none).
void toggle_media_mute();

// Everything to the mixer (at start, and when the profile changes: a kids profile has quieter interface sounds).
void apply();
// Plays a sound at the level chosen, so the slider can be heard as it moves (at most four times a second).
void preview();

}  // namespace sound_settings
