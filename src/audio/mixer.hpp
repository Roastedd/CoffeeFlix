// Audio output: one SDL device (48 kHz stereo S16) mixing the media stream
// with short UI sound effects. The stream side is a ring buffer whose read
// position doubles as the master clock for audio/video sync.
#pragma once

#include <cstddef>
#include <cstdint>

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
size_t stream_write(const int16_t* frames, size_t count, double pts, uint32_t generation);
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
enum Sfx { SFX_MOVE, SFX_SELECT, SFX_BACK, SFX_BUMP, SFX_TOGGLE, SFX_ERROR, SFX_OPEN, SFX_COUNT };
void play(Sfx s, float volume = 1.0f);
void set_sfx_enabled(bool on);
void set_sfx_volume(float v);

}  // namespace audio
