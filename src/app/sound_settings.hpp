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
float media_volume();           // video, music and radio, 0..1 on the slider (100% unless chosen)
void set_media_volume(float v);

// Everything to the mixer (at start, and when the profile changes: a kids profile has quieter interface sounds).
void apply();
// Plays a sound at the level chosen, so the slider can be heard as it moves (at most four times a second).
void preview();

}  // namespace sound_settings
