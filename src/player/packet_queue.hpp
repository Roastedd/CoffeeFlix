#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace player {

struct QueuedPacket {
    AVPacket* packet = nullptr;
    uint32_t generation = 0;
    unsigned damage = 0;
};

class PacketQueue {
public:
    void Push(AVPacket* packet, uint32_t generation, double pts, unsigned damage = 0);
    bool Pop(QueuedPacket& packet, const std::atomic<bool>& abort, int timeoutMs);
    void Flush();
    void SetEof();
    void Wake();

    size_t Count();
    size_t Bytes();
    bool Drained();
    bool HasRoom(size_t packetLimit, size_t byteLimit);
    double BufferedSeconds(double from);
    double LastTimestamp();

    template <class F>
    bool NextKey(uint32_t generation, int64_t& pts, bool& valid, F accept) {
        std::lock_guard<std::mutex> lock(Mutex);
        for (const auto& entry : Queue) {
            if (entry.generation != generation) return false;
            if (entry.packet->flags & AV_PKT_FLAG_KEY) {
                pts = entry.packet->pts;
                valid = pts != AV_NOPTS_VALUE && accept(entry.packet);
                return true;
            }
        }
        return false;
    }

private:
    std::mutex Mutex;
    std::condition_variable Changed;
    std::deque<QueuedPacket> Queue;
    size_t QueuedBytes = 0;
    bool Eof = false;
    double LastPts = -1;
};

}
