// Background work with main-thread completion callbacks.
//
//   tasks::Scope scope;   // member of a screen
//   scope.run<Results>([]{ return api::search("cats"); },
//                      [this](Results r){ results = std::move(r); });
//
// The completion runs on the main thread during tasks::pump(), and is skipped
// if the Scope was destroyed (e.g. the user left the screen) in the meantime.
#pragma once

#include <atomic>
#include <functional>
#include <memory>

namespace tasks {

enum Pool { API, IMAGES };

void init();
void shutdown();
void pump();  // main thread, once per frame

// Low-level: run `work` on a worker, then `done` on the main thread.
void submit(Pool pool, std::function<std::function<void()>()> work);
void on_main(std::function<void()> fn);  // thread-safe

class Scope {
public:
    Scope() : alive_(std::make_shared<std::atomic<bool>>(true)) {}
    ~Scope() { alive_->store(false); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    template <typename T, typename Work, typename Done>
    void run(Work work, Done done, Pool pool = API) {
        std::weak_ptr<std::atomic<bool>> weak = alive_;
        submit(pool, [work = std::move(work), done = std::move(done), weak]() mutable -> std::function<void()> {
            {
                auto a = weak.lock();
                if (!a || !a->load()) return nullptr;  // cancelled before starting
            }
            auto result = std::make_shared<T>(work());
            return [done = std::move(done), result, weak]() mutable {
                auto a = weak.lock();
                if (a && a->load()) done(std::move(*result));
            };
        });
    }

    // Invalidate all in-flight completions (e.g. a new search replaces the old).
    void reset() {
        alive_->store(false);
        alive_ = std::make_shared<std::atomic<bool>>(true);
    }

    std::shared_ptr<std::atomic<bool>> token() const { return alive_; }

private:
    std::shared_ptr<std::atomic<bool>> alive_;
};

}  // namespace tasks
