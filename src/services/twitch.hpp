// Twitch live streams through the public web GraphQL API (no account).
#pragma once

#include <string>
#include <vector>

namespace player { struct Source; }

namespace twitch {

struct Stream {
    std::string login, name, title, game, preview, avatar;
    int viewers = 0;
    bool live = false;
};

struct Category {
    std::string name, box_art;
    int viewers = 0;
};

struct Streams {
    std::vector<Stream> items;
    bool ok = false;
    std::string error;
};

struct Categories {
    std::vector<Category> items;
    bool ok = false;
    std::string error;
};

Streams top_streams(int count = 30);
Streams game_streams(const std::string& game, int count = 40);
Categories top_categories(int count = 30);
Streams search(const std::string& query);
Streams followed();  // favorites that are live right now (offline ones included, flagged)

bool is_followed(const std::string& login);
bool toggle_follow(const Stream& s);

player::Source make_source(const Stream& s);

}  // namespace twitch
