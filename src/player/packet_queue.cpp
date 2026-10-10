#include "player/packet_queue.hpp"

#include <algorithm>
#include <chrono>

namespace player {

void PacketQueue::Push(AVPacket* packet, uint32_t generation, double pts, unsigned damage) {
    std::lock_guard<std::mutex> lock(Mutex);
    Queue.push_back(QueuedPacket{packet, generation, damage});
    QueuedBytes += packet->size;
    Eof = false;
    if (pts >= 0) LastPts = pts;
    Changed.notify_one();
}

bool PacketQueue::Pop(QueuedPacket& packet, const std::atomic<bool>& abort, int timeoutMs) {
    std::unique_lock<std::mutex> lock(Mutex);
    if (!Changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
        return abort.load() || !Queue.empty();
    })) return false;
    if (abort.load() || Queue.empty()) return false;
    packet = Queue.front();
    Queue.pop_front();
    QueuedBytes -= packet.packet->size;
    return true;
}

void PacketQueue::Flush() {
    std::lock_guard<std::mutex> lock(Mutex);
    for (auto& entry : Queue) av_packet_free(&entry.packet);
    Queue.clear();
    QueuedBytes = 0;
    Eof = false;
    LastPts = -1;
}

void PacketQueue::SetEof() {
    std::lock_guard<std::mutex> lock(Mutex);
    Eof = true;
    Changed.notify_all();
}

void PacketQueue::Wake() {
    Changed.notify_all();
}

size_t PacketQueue::Count() {
    std::lock_guard<std::mutex> lock(Mutex);
    return Queue.size();
}

size_t PacketQueue::Bytes() {
    std::lock_guard<std::mutex> lock(Mutex);
    return QueuedBytes;
}

bool PacketQueue::Drained() {
    std::lock_guard<std::mutex> lock(Mutex);
    return Eof && Queue.empty();
}

bool PacketQueue::HasRoom(size_t packetLimit, size_t byteLimit) {
    std::lock_guard<std::mutex> lock(Mutex);
    return Queue.size() < packetLimit && QueuedBytes <= byteLimit;
}

double PacketQueue::BufferedSeconds(double from) {
    std::lock_guard<std::mutex> lock(Mutex);
    return Queue.empty() || LastPts < 0 ? 0 : std::max(0.0, LastPts - from);
}

double PacketQueue::LastTimestamp() {
    std::lock_guard<std::mutex> lock(Mutex);
    return LastPts;
}

}
