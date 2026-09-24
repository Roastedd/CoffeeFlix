#include "services/podcasts.hpp"

#include <tinyxml2.h>

#include "core/http.hpp"
#include "core/json.hpp"
#include "core/store.hpp"
#include "core/util.hpp"
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
    http::Response r = http::get("https://itunes.apple.com/search?media=podcast&entity=podcast&limit=40&term=" +
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

Shows top(const std::string& country) {
    Shows out;
    http::Response r = http::get("https://rss.applemarketingtools.com/api/v2/" + country + "/podcasts/top/40/podcasts.json", {}, 15);
    if (!r.ok()) {
        out.error = r.error;
        return out;
    }
    json::Doc doc = json::Doc::parse(r.body);
    json_t* results = json::at(doc.get(), {"feed", "results"});
    std::string ids;
    for (size_t i = 0; i < json::size(results); i++) {
        if (!ids.empty()) ids += ",";
        ids += json::str(json_array_get(results, i), {"id"});
    }
    if (ids.empty()) {
        out.error = "No podcasts found";
        return out;
    }
    // The chart doesn't include feed URLs: look them up in one batch.
    http::Response lr = http::get("https://itunes.apple.com/lookup?entity=podcast&id=" + ids, {}, 15);
    if (!lr.ok()) {
        out.error = lr.error;
        return out;
    }
    json::Doc ld = json::Doc::parse(lr.body);
    json_t* res = json_object_get(ld.get(), "results");
    for (size_t i = 0; i < json::size(res); i++) {
        Show s = parse_itunes(json_array_get(res, i));
        if (!s.feed_url.empty()) out.items.push_back(std::move(s));
    }
    out.ok = true;
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
        f.error = "This feed couldn't be read";
        return f;
    }
    const tinyxml2::XMLElement* ch = doc.FirstChildElement("rss");
    ch = ch ? ch->FirstChildElement("channel") : nullptr;
    if (!ch) {
        f.error = "Not a podcast feed";
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
    s.extra = extra;
    s.start = store::resume_position("podcast", guid);
    return s;
}

std::string feed_of_resume(const std::string& extra) { return util::split(extra, '\x1f')[0]; }

}  // namespace podcasts
