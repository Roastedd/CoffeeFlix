#include "services/podcasts.hpp"

#include <tinyxml2.h>

#include <algorithm>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "player/player.hpp"

namespace podcasts {

namespace {

Show parse_itunes(json_t* r) {
    Show s;
    s.title = json::str(r, {"collectionName"});
    s.author = json::str(r, {"artistName"});
    s.artwork = json::str(r, {"artworkUrl600"});
    if (s.artwork.empty()) s.artwork = json::str(r, {"artworkUrl100"});
    s.feed_url = json::str(r, {"feedUrl"});
    s.genre = json::str(r, {"primaryGenreName"});
    return s;
}

const char* text_of(const tinyxml2::XMLElement* e, const char* child) {
    const tinyxml2::XMLElement* c = e ? e->FirstChildElement(child) : nullptr;
    return c && c->GetText() ? c->GetText() : "";
}

double parse_duration(const std::string& d) {
    if (d.empty()) return 0;
    auto parts = util::split(d, ':');
    double total = 0;
    for (auto& p : parts) total = total * 60 + atof(p.c_str());
    return total;
}

// "Wed, 18 Sep 2024 10:00:00 +0000" -> "18 Sep 2024"
std::string short_date(const std::string& rfc822) {
    auto parts = util::split(util::trim(rfc822), ' ');
    if (parts.size() >= 4 && parts[0].back() == ',') return parts[1] + " " + parts[2] + " " + parts[3];
    if (parts.size() >= 3) return parts[0] + " " + parts[1] + " " + parts[2];
    return rfc822;
}

std::string upsize_artwork(std::string url) {
    // Apple artwork URLs encode the size: ".../100x100bb.png"
    size_t p = url.rfind("100x100");
    if (p != std::string::npos) url.replace(p, 7, "400x400");
    return url;
}

}  // namespace

Shows search(const std::string& query) {
    Shows out;
    http::Response r = http::get(util::env_or("COFFEEFLIX_ITUNES_API", "https://itunes.apple.com") +
                                 "/search?media=podcast&entity=podcast&limit=40&term=" +
                                 util::url_encode(query), {}, 15);
    if (!r.ok()) {
        out.error = r.error;
        return out;
    }
    json::Doc doc = json::Doc::parse(r.body);
    json_t* res = json_object_get(doc.get(), "results");
    for (size_t i = 0; i < json::size(res); i++) {
        Show s = parse_itunes(json_array_get(res, i));
        if (!s.feed_url.empty()) out.items.push_back(std::move(s));
    }
    out.ok = true;
    return out;
}

namespace {

// Apple's lookups, once more after a timeout or a server error.
http::Response get_apple(const std::string& url) {
    http::Response r = http::get(url, {}, 12);
    if (!r.ok() && (r.status == 0 || r.status >= 500)) r = http::get(url, {}, 12);
    return r;
}

// The chart's show IDs, comma-separated ("": it didn't load, `error` says why). iTunes' own chart
// first: it's on the server the lookup goes to next (one connection on the Wii U), and it answered
// at once while the newer one hung (a 504 now and then, or nothing for 20 s: for a country's top
// 40 one minute and its top 25 the next, September 2026). Both list the same shows.
std::string chart_ids(const std::string& country, std::string& error) {
    std::string ids;
    auto add = [&](const std::string& id) {
        if (id.empty()) return;
        if (!ids.empty()) ids += ",";
        ids += id;
    };
    http::Response r = http::get(util::env_or("COFFEEFLIX_ITUNES_API", "https://itunes.apple.com") + "/" + country +
                                 "/rss/toppodcasts/limit=40/json", {}, 10);
    if (r.ok()) {
        json::Doc doc = json::Doc::parse(r.body);
        json_t* entries = json::at(doc.get(), {"feed", "entry"});
        if (json_is_object(entries)) add(json::str(entries, {"id", "attributes", "im:id"}));  // a chart of one
        for (size_t i = 0; i < json::size(entries); i++)
            add(json::str(json_array_get(entries, i), {"id", "attributes", "im:id"}));
    }
    if (!ids.empty()) return ids;
    log_message(LOG_WARNING, "Podcasts", "iTunes' chart for %s: %s", country.c_str(),
                r.ok() ? "no shows" : r.error.c_str());
    // rss.applemarketingtools.com redirects here: a second connection on the Wii U.
    r = http::get(util::env_or("COFFEEFLIX_CHARTS_API", "https://rss.marketingtools.apple.com") + "/api/v2/" + country +
                  "/podcasts/top/40/podcasts.json", {}, 10);
    if (!r.ok()) {
        error = r.error;
        return "";
    }
    json::Doc doc = json::Doc::parse(r.body);
    json_t* results = json::at(doc.get(), {"feed", "results"});
    for (size_t i = 0; i < json::size(results); i++) add(json::str(json_array_get(results, i), {"id"}));
    if (ids.empty()) error = tr("No podcasts found");
    return ids;
}

// The last chart that loaded, shown when Apple's doesn't.
constexpr const char* TOP_KEPT = "podcast_top";

void keep_top(const std::string& country, const std::vector<Show>& shows) {
    std::vector<store::Fav> list;
    for (const Show& s : shows) list.push_back({s.feed_url, s.title, s.author, s.artwork, s.genre});
    const std::vector<store::Fav> was = store::favs(TOP_KEPT);
    const bool same = was.size() == list.size() && std::equal(was.begin(), was.end(), list.begin(), [](auto& a, auto& b) {
        return a.id == b.id && a.title == b.title && a.subtitle == b.subtitle && a.image == b.image && a.extra == b.extra;
    });
    if (same && store::get_str("podcast_top_country") == country) return;  // no write to the SD card
    store::fav_replace(TOP_KEPT, list);
    store::set_str("podcast_top_country", country);
}

// `out` with the kept chart for `country` instead of an error, when there is one.
Shows kept_top(const std::string& country, Shows out) {
    if (store::get_str("podcast_top_country") != country) return out;
    for (const store::Fav& f : store::favs(TOP_KEPT)) out.items.push_back({f.title, f.subtitle, f.image, f.id, f.extra});
    if (!out.items.empty()) {
        out.ok = true;
        out.error.clear();
    }
    return out;
}

}  // namespace

Shows top(const std::string& country) {
    Shows out;
    const std::string ids = chart_ids(country, out.error);
    if (ids.empty()) return kept_top(country, out);
    // The chart doesn't include feed URLs: look them up in one batch, in the chart's store (the US
    // one, the default, doesn't have some of another country's shows).
    http::Response lr = get_apple(util::env_or("COFFEEFLIX_ITUNES_API", "https://itunes.apple.com") +
                                  "/lookup?entity=podcast&country=" + country + "&id=" + ids);
    if (!lr.ok()) {
        out.error = lr.error;
        return kept_top(country, out);
    }
    json::Doc ld = json::Doc::parse(lr.body);
    json_t* res = json_object_get(ld.get(), "results");
    for (size_t i = 0; i < json::size(res); i++) {
        Show s = parse_itunes(json_array_get(res, i));
        if (!s.feed_url.empty()) out.items.push_back(std::move(s));
    }
    out.ok = true;
    if (!out.items.empty()) keep_top(country, out.items);
    return out;
}

Feed load(const std::string& feed_url) {
    Feed f;
    http::Request req;
    req.url = feed_url;
    req.timeout = 25;
    req.max_bytes = 24u << 20;
    http::Response r = http::perform(req);
    if (!r.ok()) {
        f.error = r.error;
        return f;
    }
    tinyxml2::XMLDocument doc;
    if (doc.Parse(r.body.data(), r.body.size()) != tinyxml2::XML_SUCCESS) {
        f.error = tr("This feed couldn't be read");
        return f;
    }
    const tinyxml2::XMLElement* ch = doc.FirstChildElement("rss");
    ch = ch ? ch->FirstChildElement("channel") : nullptr;
    if (!ch) {
        f.error = tr("Not a podcast feed");
        return f;
    }
    f.show.feed_url = feed_url;
    f.show.title = util::trim(text_of(ch, "title"));
    f.show.author = util::trim(text_of(ch, "itunes:author"));
    if (const tinyxml2::XMLElement* img = ch->FirstChildElement("itunes:image")) {
        if (const char* href = img->Attribute("href")) f.show.artwork = href;
    }
    if (f.show.artwork.empty()) f.show.artwork = text_of(ch->FirstChildElement("image"), "url");
    std::string desc = text_of(ch, "itunes:summary");
    if (desc.empty()) desc = text_of(ch, "description");
    f.description = util::html_to_text(desc);

    int count = 0;
    for (const tinyxml2::XMLElement* it = ch->FirstChildElement("item"); it && count < 150;
         it = it->NextSiblingElement("item")) {
        const tinyxml2::XMLElement* enc = it->FirstChildElement("enclosure");
        const char* url = enc ? enc->Attribute("url") : nullptr;
        if (!url || !*url) continue;
        Episode e;
        e.url = url;
        e.title = util::trim(text_of(it, "title"));
        e.guid = text_of(it, "guid");
        if (e.guid.empty()) e.guid = e.url;
        e.published = short_date(text_of(it, "pubDate"));
        e.duration = parse_duration(text_of(it, "itunes:duration"));
        std::string d = text_of(it, "itunes:summary");
        if (d.empty()) d = text_of(it, "description");
        e.description = util::html_to_text(d);
        if (const tinyxml2::XMLElement* img = it->FirstChildElement("itunes:image"))
            if (const char* href = img->Attribute("href")) e.image = href;
        f.episodes.push_back(std::move(e));
        count++;
    }
    f.ok = true;
    return f;
}

bool is_subscribed(const std::string& feed_url) { return store::fav_has("podcast", feed_url); }

bool toggle_subscription(const Show& s) {
    return store::fav_toggle("podcast", store::Fav{s.feed_url, s.title, s.author, upsize_artwork(s.artwork), s.genre});
}

std::vector<Show> subscriptions() {
    std::vector<Show> out;
    for (auto& f : store::favs("podcast")) out.push_back(Show{f.title, f.subtitle, f.image, f.id, f.extra});
    return out;
}

player::Source make_source(const Show& show, const Episode& ep) {
    player::Source s;
    s.url = ep.url;
    s.title = ep.title;
    s.subtitle = show.title;
    s.artwork = ep.image.empty() ? upsize_artwork(show.artwork) : ep.image;
    s.service = "podcast";
    s.id = ep.guid;
    s.chunked_http = true;  // the redirects through the trackers once, not for every read (http_io.hpp)
    s.extra = show.feed_url + "\x1f" + ep.url;  // lets "Continue listening" resume directly
    s.start = store::resume_position("podcast", ep.guid);
    return s;
}

player::Source source_from_resume(const std::string& guid, const std::string& title, const std::string& show_title,
                                  const std::string& image, const std::string& extra) {
    player::Source s;
    auto parts = util::split(extra, '\x1f');
    s.url = parts.size() > 1 ? parts[1] : "";
    s.title = title;
    s.subtitle = show_title;
    s.artwork = image;
    s.service = "podcast";
    s.id = guid;
    s.chunked_http = true;
    s.extra = extra;
    s.start = store::resume_position("podcast", guid);
    return s;
}

std::string feed_of_resume(const std::string& extra) { return util::split(extra, '\x1f')[0]; }

}  // namespace podcasts
