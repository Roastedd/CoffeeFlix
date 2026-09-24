// DLNA / UPnP media servers (Plex, Jellyfin, Emby, Serviio, minidlna, most NAS
// boxes, Windows Media Player sharing): found with an SSDP search on the local
// network and browsed through their ContentDirectory service. Media is
// streamed over plain HTTP, so the player and image loader handle it as-is.
// Everything here blocks on the network: call it from worker threads.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dlna {

struct Server {
    std::string name, model;
    std::string udn;           // unique device id
    std::string location;      // device description URL
    std::string control_url;   // ContentDirectory control URL
    std::string service_type;  // e.g. urn:schemas-upnp-org:service:ContentDirectory:1
    std::string icon;          // device icon URL, may be empty
};

struct Servers {
    std::vector<Server> items;
    std::string error;
};

// Sends an SSDP M-SEARCH for media servers, waits `wait_ms` for answers and
// reads each server's description.
Servers discover(int wait_ms = 2500);
// Servers found by the last discovery (for showing the list instantly).
std::vector<Server> known_servers();

enum Kind { CONTAINER, VIDEO, AUDIO, IMAGE };

struct Item {
    Kind kind = CONTAINER;
    std::string id, title;
    std::string url;           // stream URL (empty for containers)
    std::string art;           // album art / thumbnail URL
    std::string artist, album;
    std::string mime;
    std::string subtitles;     // .srt URL when the server offers one
    uint64_t size = 0;
    double duration = 0;       // seconds
    int child_count = -1;      // containers, when the server says
};

struct Listing {
    std::vector<Item> items;
    bool ok = false;
    std::string error;
};

// Children of a container ("0" is the root).
Listing browse(const Server& server, const std::string& object_id);

}  // namespace dlna
