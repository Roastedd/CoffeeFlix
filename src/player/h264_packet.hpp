#pragma once
#include <cstddef>
#include <cstdint>
namespace player {
// Validate framing before sending untrusted bytes to the console's hardware decoder.
// length_size: 1..4 for AVC/MP4/MKV, zero for Annex B. Does not validate slice syntax.
bool complete_h264_packet(const uint8_t* data, size_t size, int length_size);
}
