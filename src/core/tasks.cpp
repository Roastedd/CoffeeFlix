#include "core/tasks.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace tasks {

namespace {

struct WorkerPool {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<std::function<void()>()>> queue;
    std::vector<std::thread> threads;
    bool lifo = false;  // image loads: newest requests first (what's on screen now)
    bool held = false;
};

WorkerPool g_pools[2];
std::mutex g_main_m;
std::vector<std::function<void()>> g_main;
std::atomic<bool> g_running{false};

void worker_loop(WorkerPool* pool) {
    for (;;) {
        std::function<std::function<void()>()> job;
        {
            std::unique_lock<std::mutex> lk(pool->m);
            pool->cv.wait(lk, [&] { return !g_running || (!pool->held && !pool->queue.empty()); });
            if (!g_running) return;
            if (pool->lifo) {
                job = std::move(pool->queue.back());
                pool->queue.pop_back();
            } else {
                job = std::move(pool->queue.front());
                pool->queue.pop_front();
            }
        }
        std::function<void()> done;
        try {
            done = job();
        } catch (...) {
            done = nullptr;
        }
        if (done) on_main(std::move(done));
    }
}

}  // namespace

void init() {
    g_running = true;
    g_pools[IMAGES].lifo = true;
    const int counts[2] = {3, 2};
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < counts[p]; i++) g_pools[p].threads.emplace_back(worker_loop, &g_pools[p]);
}

void shutdown() {
    g_running = false;
    for (auto& p : g_pools) {
        {
            std::lock_guard<std::mutex> lk(p.m);
            p.queue.clear();
        }
        p.cv.notify_all();
    }
    for (auto& p : g_pools)
        for (auto& t : p.threads)
            if (t.joinable()) t.join();
    std::lock_guard<std::mutex> lk(g_main_m);
    g_main.clear();
}

void submit(Pool pool, std::function<std::function<void()>()> work) {
    WorkerPool& p = g_pools[pool];
    {
        std::lock_guard<std::mutex> lk(p.m);
        p.queue.push_back(std::move(work));
    }
    p.cv.notify_one();
}

void hold(Pool pool, bool held) {
    WorkerPool& p = g_pools[pool];
    {
        std::lock_guard<std::mutex> lk(p.m);
        p.held = held;
    }
    if (!held) p.cv.notify_all();
}

void on_main(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(g_main_m);
    g_main.push_back(std::move(fn));
}

void pump() {
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard<std::mutex> lk(g_main_m);
        todo.swap(g_main);
    }
    for (auto& fn : todo) fn();
}

}  // namespace tasks
