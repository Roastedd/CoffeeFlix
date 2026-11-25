// The songs of the account's playlists, kept for the search (Spotify's own search isn't open to this client):
// each playlist with its revision, so only the ones that changed are read again. Kept on the card, for the
// account it was made for. Thread-safe.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "services/spotify/spotify.hpp"

namespace spotify::index {

// The playlist's revision when it was indexed ("" when it isn't).
std::string revision(const std::string& playlist_uri);
// Its songs (and episodes) as kept, with its revision: false when it isn't kept.
bool get(const std::string& playlist_uri, std::string& revision, std::vector<Item>& out);
void put(const std::string& playlist_uri, const std::string& name, const std::string& revision, std::vector<Item> items);
// Drops playlists that are gone from the account.
void keep_only(const std::set<std::string>& playlist_uris);
void save();
// Signing out: the index goes, on the card too.
void forget();

size_t songs();     // indexed, all playlists together
size_t playlists();

// Songs of the playlists (not the liked songs, kept here too) whose name or artists hold every word (already
// folded to lower case), each once, as found in the first playlist that has it: the subtitle says which
// ("Artist · in Playlist").
std::vector<Item> search(const std::vector<std::string>& words, size_t max,
                         std::string (*fold)(const std::string&));

}  // namespace spotify::index
