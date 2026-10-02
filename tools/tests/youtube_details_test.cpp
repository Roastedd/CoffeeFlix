// What the player shows under a YouTube title: views and the day it was posted, read from the
// "player" response. With a video id as the argument it asks YouTube too (the network).
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/store.hpp"
#include "player/player.hpp"
#include "services/youtube.hpp"

int main(int argc, char** argv) {
    char dir[] = "/tmp/youtube-details-XXXXXX";
    assert(mkdtemp(dir));
    store::load(std::string(dir) + "/store.json");
    i18n::init("content");
    i18n::choose("en");
    assert(i18n::long_date("2026-09-14") == "September 14, 2026");
    assert(i18n::long_date("2026-09-14T07:00:00-07:00") == "September 14, 2026");  // the time is ignored
    assert(i18n::long_date("2024-01-05") == "January 5, 2024");
    assert(i18n::long_date("").empty() && i18n::long_date("3 weeks ago").empty() && i18n::long_date("2026-13-01").empty());

    // The WEB client's answer: all three.
    youtube::Details d = youtube::parse_details(
        R"({"videoDetails":{"viewCount":"1234567"},"microformat":{"playerMicroformatRenderer":
            {"viewCount":"1234567","likeCount":"19453529","publishDate":"2009-10-24T23:57:33-07:00","uploadDate":"2009-10-23"}}})");
    assert(d.views == "1.2M views" && d.likes == "19.5M likes" && d.posted == "October 24, 2009");

    // One view and one like; the upload date when there is no publish date; the microformat's count when the details have none.
    d = youtube::parse_details(R"({"videoDetails":{"viewCount":"1"},"microformat":{"playerMicroformatRenderer":{"likeCount":"1","uploadDate":"2025-12-31"}}})");
    assert(d.views == "1 view" && d.likes == "1 like" && d.posted == "December 31, 2025");
    d = youtube::parse_details(R"({"microformat":{"playerMicroformatRenderer":{"viewCount":"950","likeCount":"0"}}})");
    assert(d.views == "950 views" && d.likes.empty() && d.posted.empty());

    // A playback client's answer (the views, no microformat), nothing, and nonsense.
    d = youtube::parse_details(R"({"videoDetails":{"viewCount":"1822159203"}})");
    assert(d.views == "1.8B views" && d.likes.empty() && d.posted.empty());
    for (const char* body : {"{}", "{\"videoDetails\":{\"viewCount\":\"many\"},\"microformat\":{\"playerMicroformatRenderer\":{\"publishDate\":\"soon\",\"likeCount\":\"lots\"}}}",
                             "not json", ""}) {
        d = youtube::parse_details(body);
        assert(d.views.empty() && d.likes.empty() && d.posted.empty());
    }

    // In another language: the day in that language's order (Japanese: 2026年9月14日).
    const char* body = R"({"videoDetails":{"viewCount":"2"},"microformat":{"playerMicroformatRenderer":{"likeCount":"3","publishDate":"2026-09-14"}}})";
    i18n::choose("ja");
    d = youtube::parse_details(body);
    assert(d.views == "2回視聴" && d.likes == "高評価3件" && d.posted == "2026年9月14日");
    i18n::choose("de");
    d = youtube::parse_details(body);
    assert(d.views == "2 Aufrufe" && d.likes == "3 Likes" && d.posted == "14. September 2026");
    i18n::choose("en");

    std::printf("PASS views, likes and posted date from the player response\n");

    if (argc > 1) {  // the real thing: what do the playback client and the WEB client send?
        http::init("content/cacert.pem");
        store::set_bool("yt_sponsorblock", false);
        youtube::Video v;
        v.id = argv[1];
        v.views = "feed views";
        v.published = "feed date";
        player::Source played = youtube::make_source(v);
        assert(played.views == "feed views" && played.posted == "feed date");
        std::string err;
        const bool ok = played.resolve(played, err);
        std::printf("%s: %s | views \"%s\" | posted \"%s\" | %s\n", argv[1], ok ? "resolved" : "FAILED", played.views.c_str(),
                    played.posted.c_str(), err.c_str());
        d = youtube::details(argv[1], err);
        std::printf("details: views \"%s\" | likes \"%s\" | posted \"%s\" | %s\n", d.views.c_str(), d.likes.c_str(),
                    d.posted.c_str(), err.c_str());
        http::shutdown();
        return ok && played.views != "feed views" && !d.likes.empty() && !d.posted.empty() ? 0 : 1;
    }
    return 0;
}
