// Audio output: one SDL device (48 kHz stereo S16) mixing the media stream
// with short UI sound effects. The stream side is a ring buffer whose read
// position doubles as the master clock for audio/video sync.
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/ui_sounds.hpp"

namespace audio {

constexpr int RATE = 48000;
constexpr int CHANNELS = 2;

bool init();
void shutdown();
// Receives everything the device plays (desktop screen recording), on the audio thread.
void set_tap(void (*tap)(const int16_t* frames, int count));

// --- media stream ------------------------------------------------------------
// Appends interleaved S16 stereo frames; `pts` is the media time of the first
// frame. Returns the number of frames accepted (ring may be full).
// Writes tagged with an old generation are dropped, so a decoder thread that
// is still finishing after a seek/stop can't leak stale audio.
// `speed` is how many frames of media each frame written stands for: the playback speed
// the sound was sped up or slowed down to before it got here.
size_t stream_write(const int16_t* frames, size_t count, double pts, uint32_t generation, float speed = 1);
// Takes back what was written beyond the next `keep_seconds` to play (fading out where it
// now ends), so it can be written again differently. Returns the media time of the first
// frame taken back (or of the end, if there was nothing to take), or -1 when nothing of
// `generation` was written.
double stream_cut(uint32_t generation, double keep_seconds);
size_t stream_free_frames();
double stream_buffered_seconds();
void stream_reset(uint32_t generation);  // drop everything and accept only `generation`
void stream_clear();
void stream_pause(bool paused);
bool stream_paused();
// Media time currently audible, or a negative value if nothing has played yet.
double stream_clock();
// Seconds of silence inserted because the ring ran dry since the last call.
double stream_take_underrun();
void stream_set_volume(float v);  // 0..1

// 16 smoothed spectrum bands (0..1) of the stream for visualizers.
void levels(float out[16]);

// --- UI sounds ------------------------------------------------------------------
// (the sounds themselves are in audio/ui_sounds.hpp: Sfx, SoundStyle)
void play(Sfx s, float volume = 1.0f);
void set_sfx_enabled(bool on);
void set_sfx_style(SoundStyle style);   // Subtle (the soft set) or Classic (the old beeps)
// How loud the interface sounds are: the slider's 0..1, which sounds ever more even as it goes (the square of it is what plays).
void set_sfx_volume(float v);
void set_sfx_ticks(bool on);            // the tick when the selector moves, apart from the rest
void set_sfx_kids(bool on);             // a kids profile: three decibels quieter
// How loud the picture's own sound is (video, music, radio): the slider's 0..1, on the same curve.
void set_media_volume(float v);
// A multiplier on the picture's sound for fading it out (the sleep timer): 1 is as it is.
void set_media_fade(float f);

}  // namespace audio
