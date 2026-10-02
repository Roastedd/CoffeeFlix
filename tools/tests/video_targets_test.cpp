#include <cassert>
#include <cstdio>
extern "C" {
#include <libavutil/buffer.h>
#include "libavcodec/wiiu_frame_targets.h"
}

static int allocations = 0;
static AVBufferRef* allocate(size_t size) {
    ++allocations;
    return av_buffer_alloc(size);
}

int main() {
    AVBufferPool* pool = av_buffer_pool_init(256, allocate);
    assert(pool);
    WiiUFrameTargets targets{};
    uint8_t* addresses[WIIU_MAX_TARGETS]{};
    // Simulate packets returning no output, including errors. Their hardware
    // destinations must not return to the pool while later packets execute.
    for (int i = 0; i < WIIU_MAX_TARGETS; ++i) {
        AVFrame** slot = wiiu_target_slot(&targets, nullptr);
        assert(slot);
        *slot = av_frame_alloc();
        (*slot)->buf[0] = av_buffer_pool_get(pool);
        assert((*slot)->buf[0]);
        addresses[i] = (*slot)->data[0] = (*slot)->buf[0]->data;
        for (int n = 0; n < i; ++n) assert(addresses[n] != addresses[i]);
    }
    assert(allocations == WIIU_MAX_TARGETS);
    assert(!wiiu_target_slot(&targets, nullptr)); // bounded fallback, no eviction
    uint8_t foreign[4];
    assert(!wiiu_target_slot(&targets, foreign));

    // A callback arrives out of order, several packets after submission.
    AVFrame** owner = wiiu_target_slot(&targets, addresses[2]);
    assert(owner && (*owner)->buf[0]->data == addresses[2]);
    (*owner)->data[0][0] = 77; // delayed decoder write is still valid
    AVFrame* display = *owner;
    *owner = nullptr; // completed picture transfers to presentation ownership
    assert(wiiu_target_slot(&targets, nullptr));
    assert(!wiiu_target_slot(&targets, addresses[2]));
    AVBufferRef* next = av_buffer_pool_get(pool);
    assert(next && next->data != addresses[2]); // GPU still owns the picture
    av_buffer_unref(&next);

    // Seek/close has drained the decoder; pending targets can now be released.
    wiiu_targets_clear(&targets);
    for (AVFrame* f : targets.frames) assert(!f);
    assert(display->data[0][0] == 77); // presentation remains valid after drain
    av_buffer_pool_uninit(&pool); // outstanding display reference survives pool shutdown
    assert(display->data[0][0] == 77);
    av_frame_free(&display); // after simulated GPU completion
    wiiu_targets_clear(&targets); // repeated teardown is safe
    std::puts("PASS delayed/error targets retained, bounded fallback, out-of-order output, GPU ownership and teardown");
}
