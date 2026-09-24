// Internet radio via the community radio-browser.info directory.
#pragma once

#include <string>
#include <vector>

namespace player { struct Source; }

namespace radio {

struct Station {
    std::string uuid, name, url, favicon, country, countrycode, tags, codec, homepage;
    int bitrate = 0, votes = 0;
    bool hls = false;
};

struct List {
    std::vector<Station> items;
    bool ok = false;
    std::string error;
};

List top(int limit = 40);
List by_country(const std::string& country_code, int limit = 40);
List by_tag(const std::string& tag, int limit = 40);
List search(const std::string& query, int limit = 60);

struct Genre { const char* name; const char* tag; int icon; };
const std::vector<Genre>& genres();

player::Source make_source(const Station& s);
// Favorites store stations as serialized records.
bool is_favorite(const Station& s);
bool toggle_favorite(const Station& s);
std::vector<Station> favorites();

}  // namespace radio
