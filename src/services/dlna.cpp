#include "services/dlna.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <tinyxml2.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#include "core/http.hpp"
#include "core/i18n.hpp"
#include "core/util.hpp"
#include "logger/logger.hpp"
#include "services/hls.hpp"

namespace dlna {

namespace {

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

std::mutex g_m;
std::vector<Server> g_known;

// UPnP documents use namespace prefixes freely ("dc:title", "upnp:class"), so
// elements are matched by their local name.
const char* local_name(const XMLElement* e) {
    const char* n = e->Name();
    const char* c = strchr(n, ':');
    return c ? c + 1 : n;
}

const XMLElement* child(const XMLElement* e, const char* name) {
    for (const XMLElement* c = e ? e->FirstChildElement() : nullptr; c; c = c->NextSiblingElement())
        if (strcmp(local_name(c), name) == 0) return c;
    return nullptr;
}

std::string text(const XMLElement* e, const char* name) {
    const XMLElement* c = child(e, name);
    const char* t = c ? c->GetText() : nullptr;
    return t ? util::trim(t) : std::string();
}

std::string attr(const XMLElement* e, const char* name) {
    const char* v = e ? e->Attribute(name) : nullptr;
    return v ? v : "";
}

// --- discovery -------------------------------------------------------------------------

// Where to send the search (overridable so the desktop build can test against a
// local responder).
void ssdp_target(sockaddr_in& dst) {
    std::string target = util::env_or("COFFEEFLIX_SSDP", "239.255.255.250:1900");
    size_t colon = target.rfind(':');
    std::string host = target.substr(0, colon);
    int port = colon == std::string::npos ? 1900 : atoi(target.c_str() + colon + 1);
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    inet_aton(host.c_str(), &dst.sin_addr);
}

std::vector<std::string> ssdp_search(int wait_ms, std::string& error) {
    std::vector<std::string> found;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        error = tr("Can't search the network");
        return found;
    }
    sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd, (sockaddr*)&local, sizeof(local));

    sockaddr_in dst;
    ssdp_target(dst);
    const char* msg =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
        "USER-AGENT: CoffeeFlix/2.0 UPnP/1.1\r\n\r\n";
    // UDP can drop packets; ask twice.
    bool sent = false;
    for (int i = 0; i < 2; i++)
        if (sendto(fd, msg, strlen(msg), 0, (sockaddr*)&dst, sizeof(dst)) > 0) sent = true;
    if (!sent) {
        error = tr("Can't search the network");
        close(fd);
        return found;
    }

    using clock = std::chrono::steady_clock;
    clock::time_point deadline = clock::now() + std::chrono::milliseconds(wait_ms);
    char buf[2048];
    for (;;) {
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) break;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, left) <= 0) break;
        int n = (int)recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) continue;
        buf[n] = 0;
        // Pick the LOCATION header out of the HTTP-style reply.
        for (const std::string& line : util::split(buf, '\n')) {
            std::string l = util::trim(line);
            if (util::lower(l.substr(0, 9)) != "location:") continue;
            std::string loc = util::trim(l.substr(9));
            if (!loc.empty() && std::find(found.begin(), found.end(), loc) == found.end()) found.push_back(loc);
        }
    }
    close(fd);
    return found;
}

// Finds the device (possibly an embedded one) that offers a ContentDirectory.
const XMLElement* find_media_device(const XMLElement* device, const XMLElement** service) {
    for (; device; device = device->NextSiblingElement()) {
        if (strcmp(local_name(device), "device") != 0) continue;
        if (const XMLElement* list = child(device, "serviceList")) {
            for (const XMLElement* s = list->FirstChildElement(); s; s = s->NextSiblingElement()) {
                if (text(s, "serviceType").find("ContentDirectory") != std::string::npos) {
                    *service = s;
                    return device;
                }
            }
        }
        if (const XMLElement* sub = child(device, "deviceList")) {
            if (const XMLElement* d = find_media_device(sub->FirstChildElement(), service)) return d;
        }
    }
    return nullptr;
}

bool describe(const std::string& location, Server& out) {
    http::Response r = http::get(location, {{"User-Agent", "CoffeeFlix/2.0 UPnP/1.1"}}, 6);
    if (!r.ok()) return false;
    XMLDocument doc;
    if (doc.Parse(r.body.c_str(), r.body.size()) != tinyxml2::XML_SUCCESS) return false;
    const XMLElement* root = doc.RootElement();
    const XMLElement* service = nullptr;
    const XMLElement* dev = root ? find_media_device(child(root, "device"), &service) : nullptr;
    if (!dev || !service) return false;

    std::string base = text(root, "URLBase");
    if (base.empty()) base = location;
    out.location = location;
    out.name = text(dev, "friendlyName");
    out.model = text(dev, "modelName");
    // Some servers pose as Windows Media Connect for old clients; the
    // description says what they really are.
    if (out.model.find("Windows Media") != std::string::npos && !text(dev, "modelDescription").empty())
        out.model = text(dev, "modelDescription");
    out.udn = text(dev, "UDN");
    out.service_type = text(service, "serviceType");
    out.control_url = hls::resolve_url(base, text(service, "controlURL"));
    if (out.name.empty()) out.name = out.model.empty() ? tr("Media server") : out.model;
    if (out.udn.empty()) out.udn = location;

    // Largest PNG/JPEG icon up to 256 px.
    int best = 0;
    if (const XMLElement* icons = child(dev, "iconList")) {
        for (const XMLElement* ic = icons->FirstChildElement(); ic; ic = ic->NextSiblingElement()) {
            std::string mime = text(ic, "mimetype");
            int w = atoi(text(ic, "width").c_str());
            if (mime.find("png") == std::string::npos && mime.find("jpeg") == std::string::npos) continue;
            if (w > 256 || w <= best) continue;
            best = w;
            out.icon = hls::resolve_url(base, text(ic, "url"));
        }
    }
    return !out.control_url.empty();
}

// --- browsing --------------------------------------------------------------------------

std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

double parse_duration(const std::string& d) {  // H:MM:SS(.mmm)
    int h = 0, m = 0;
    double s = 0;
    if (sscanf(d.c_str(), "%d:%d:%lf", &h, &m, &s) == 3) return h * 3600 + m * 60 + s;
    return 0;
}

Item parse_item(const XMLElement* e) {
    Item it;
    it.id = attr(e, "id");
    it.title = text(e, "title");
    it.art = text(e, "albumArtURI");
    it.artist = text(e, "artist");
    if (it.artist.empty()) it.artist = text(e, "creator");
    it.album = text(e, "album");
    if (strcmp(local_name(e), "container") == 0) {
        it.kind = CONTAINER;
        const char* cc = e->Attribute("childCount");
        if (cc) it.child_count = atoi(cc);
        return it;
    }
    std::string cls = text(e, "class");
    const char* want = nullptr;
    if (cls.find("videoItem") != std::string::npos) {
        it.kind = VIDEO;
        want = "video/";
    } else if (cls.find("audioItem") != std::string::npos) {
        it.kind = AUDIO;
        want = "audio/";
    } else if (cls.find("imageItem") != std::string::npos) {
        it.kind = IMAGE;
        want = "image/";
    } else {
        return Item{};  // not playable (playlists, text): dropped by the caller
    }

    // The first matching resource is the original file; thumbnails are marked
    // with a *_TN profile.
    for (const XMLElement* r = e->FirstChildElement(); r; r = r->NextSiblingElement()) {
        if (strcmp(local_name(r), "res") != 0 || !r->GetText()) continue;
        std::string info = attr(r, "protocolInfo");
        std::vector<std::string> parts = util::split(info, ':');
        std::string mime = parts.size() > 2 ? util::lower(parts[2]) : "";
        std::string url = util::trim(r->GetText());
        if (mime == "text/srt" || mime == "application/x-subrip") {
            if (it.subtitles.empty()) it.subtitles = url;
            continue;
        }
        if (mime == "smi/caption") continue;
        bool thumb = info.find("_TN") != std::string::npos;
        if (thumb) {
            if (it.art.empty()) it.art = url;
            continue;
        }
        if (!it.url.empty() || (!mime.empty() && mime.compare(0, strlen(want), want) != 0)) continue;
        it.url = url;
        it.mime = mime;
        it.size = strtoull(attr(r, "size").c_str(), nullptr, 10);
        it.duration = parse_duration(attr(r, "duration"));
    }
    // Samsung's caption extension (minidlna, Serviio, Plex).
    if (it.subtitles.empty()) it.subtitles = text(e, "CaptionInfoEx");
    if (it.subtitles.empty()) it.subtitles = text(e, "CaptionInfo");
    if (it.kind == IMAGE && it.art.empty()) it.art = it.url;
    return it;
}

}  // namespace

Servers discover(int wait_ms) {
    Servers out;
    std::vector<std::string> locations = ssdp_search(wait_ms, out.error);
    for (const std::string& loc : locations) {
        Server s;
        if (!describe(loc, s)) continue;
        bool dup = false;
        for (const Server& o : out.items) dup |= o.udn == s.udn;
        if (!dup) out.items.push_back(std::move(s));
    }
    std::sort(out.items.begin(), out.items.end(), [](const Server& a, const Server& b) {
        return util::lower(a.name) < util::lower(b.name);
    });
    log_message(LOG_OK, "DLNA", "%zu media server(s) found", out.items.size());
    std::lock_guard<std::mutex> lk(g_m);
    g_known = out.items;
    return out;
}

std::vector<Server> known_servers() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_known;
}

Listing browse(const Server& server, const std::string& object_id) {
    Listing out;
    const int page = 200, limit = 3000;
    int start = 0;
    for (;;) {
        http::Request req;
        req.method = "POST";
        req.url = server.control_url;
        req.timeout = 25;
        req.headers = {{"Content-Type", "text/xml; charset=\"utf-8\""},
                       {"SOAPACTION", "\"" + server.service_type + "#Browse\""},
                       {"User-Agent", "CoffeeFlix/2.0 UPnP/1.1 DLNADOC/1.50"}};
        req.body = util::fmt(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
            "<u:Browse xmlns:u=\"%s\"><ObjectID>%s</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
            "<Filter>*</Filter><StartingIndex>%d</StartingIndex><RequestedCount>%d</RequestedCount>"
            "<SortCriteria></SortCriteria></u:Browse></s:Body></s:Envelope>",
            server.service_type.c_str(), xml_escape(object_id).c_str(), start, page);
        http::Response r = http::perform(req);
        if (!r.ok()) {
            if (out.items.empty()) {
                out.error = r.status ? util::fmt(tr("The server refused the request (%ld)"), r.status)
                                     : util::fmt(tr("Can't reach %s"), server.name.c_str());
                return out;
            }
            break;  // keep what arrived
        }
        XMLDocument env;
        env.Parse(r.body.c_str(), r.body.size());
        const XMLElement* body = child(env.RootElement(), "Body");
        const XMLElement* resp = body ? body->FirstChildElement() : nullptr;
        const char* result = resp ? (child(resp, "Result") ? child(resp, "Result")->GetText() : nullptr) : nullptr;
        int returned = atoi(text(resp, "NumberReturned").c_str());
        int total = atoi(text(resp, "TotalMatches").c_str());
        if (!result) {
            if (out.items.empty()) {
                out.error = tr("The server sent an answer CoffeeFlix doesn't understand");
                return out;
            }
            break;
        }
        XMLDocument didl;
        didl.Parse(result);
        for (const XMLElement* e = didl.RootElement() ? didl.RootElement()->FirstChildElement() : nullptr; e;
             e = e->NextSiblingElement()) {
            Item it = parse_item(e);
            if (it.id.empty() || it.title.empty()) continue;
            if (it.kind != CONTAINER && it.url.empty()) continue;
            if (it.kind == CONTAINER && it.child_count == 0) continue;  // empty genre/playlist folders
            out.items.push_back(std::move(it));
        }
        start += returned;
        if (returned <= 0 || start >= total || start >= limit) break;
    }
    out.ok = true;
    return out;
}

}  // namespace dlna
