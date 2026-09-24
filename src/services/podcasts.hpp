// Podcasts: Apple's public directory for discovery, RSS feeds for episodes.
#pragma once

#include <string>
#include <vector>

namespace player { struct Source; }

namespace podcasts {

struct Show {
    std::string title, author, artwork, feed_url, genre;
};

struct Episode {
    std::string guid, title, url, published, description, image;
    double duration = 0;
};

struct Shows {
    std::vector<Show> items;
    bool ok = false;
    std::string error;
};

struct Feed {
    Show show;
    std::string description;
    std::vector<Episode> episodes;
    bool ok = false;
    std::string error;
};

Shows search(const std::string& query);
Shows top(const std::string& country = "us");
Feed load(const std::string& feed_url);

bool is_subscribed(const std::string& feed_url);
bool toggle_subscription(const Show& s);
std::vector<Show> subscriptions();

player::Source make_source(const Show& show, const Episode& ep);
// Rebuilds a playable source from a "Continue listening" entry.
player::Source source_from_resume(const std::string& guid, const std::string& title, const std::string& show_title,
                                  const std::string& image, const std::string& extra);
std::string feed_of_resume(const std::string& extra);

}  // namespace podcasts
