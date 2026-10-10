#include "player/packet_queue.hpp"

#include <cassert>
#include <atomic>
#include <cstdio>

int main() {
    player::PacketQueue queue;
    std::atomic<bool> abort{false};
    player::QueuedPacket popped;
    assert(!queue.Pop(popped, abort, 1));
    assert(queue.Count() == 0);
    assert(queue.Bytes() == 0);
    assert(queue.BufferedSeconds(0) == 0);
    assert(queue.LastTimestamp() == -1);
    assert(!queue.Drained());

    AVPacket* first = av_packet_alloc();
    assert(first && av_new_packet(first, 64) == 0);
    first->flags |= AV_PKT_FLAG_KEY;
    first->pts = 90000;
    queue.Push(first, 2, 1.0, 7);

    assert(queue.Count() == 1);
    assert(queue.Bytes() == 64);
    assert(queue.BufferedSeconds(0.25) == 0.75);
    assert(queue.LastTimestamp() == 1.0);
    assert(queue.HasRoom(2, 64));
    assert(!queue.HasRoom(1, 64));
    assert(!queue.HasRoom(2, 63));

    int64_t pts = -1;
    bool valid = false;
    assert(!queue.NextKey(3, pts, valid, [](const AVPacket*) { return true; }));
    assert(queue.NextKey(2, pts, valid, [](const AVPacket* packet) {
        return packet->size == 64;
    }));
    assert(pts == 90000 && valid);

    queue.SetEof();
    assert(!queue.Drained());
    assert(queue.Pop(popped, abort, 1));
    assert(popped.packet == first && popped.generation == 2 && popped.damage == 7);
    assert(queue.Count() == 0);
    assert(queue.Drained());
    assert(queue.BufferedSeconds(0) == 0);
    assert(queue.LastTimestamp() == 1.0);
    av_packet_free(&popped.packet);

    AVPacket* second = av_packet_alloc();
    assert(second && av_new_packet(second, 8) == 0);
    queue.Push(second, 3, 1.5);
    assert(!queue.Drained());
    queue.Flush();
    assert(queue.Count() == 0);
    assert(queue.Bytes() == 0);
    assert(queue.LastTimestamp() == -1);
    assert(!queue.Drained());

    abort = true;
    queue.Wake();
    assert(!queue.Pop(popped, abort, 1));
    std::puts("PASS packet queue ownership, buffering, keyframes, EOF and flush");
}
