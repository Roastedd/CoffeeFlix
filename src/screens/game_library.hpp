#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace game_library {

struct Game {
    uint64_t titleId = 0;
    std::string name, path, icon;
    bool usb = false;
    bool wii = false;
    bool gamepad = false;
};

struct Recent {
    Game game;
    int option = 1;
};

struct Progress {
    std::atomic<int> phase{0};
    std::atomic<int> checked{0};
};

struct Result {
    std::vector<Game> games;
    bool failed = false;
};

std::vector<Game> cached();
std::vector<Recent> recent(size_t max = 12);
std::string boot_tv_image(const Game& game);
Result scan(const std::shared_ptr<Progress>& progress, bool full = false);
void launch(const Game& game, int option);

}
