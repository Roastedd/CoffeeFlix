// What spotify.cpp shares with the other parts of the Spotify service (spotify_library.cpp).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/http.hpp"

namespace spotify::detail {

std::string hex(const std::vector<uint8_t>& b);
// Spotify ids (22 characters, base 62) and the 16 bytes the services use for them.
std::vector<uint8_t> id_to_gid(const std::string& id);
std::string gid_to_id(const std::vector<uint8_t>& gid);
// A request to spclient with the account's tokens: a GET, or a POST of `post` as `content_type`.
http::Response sp_get(const std::string& path, const std::atomic<bool>* cancel, std::string& error,
                      const std::vector<uint8_t>* post = nullptr, const char* content_type = "application/x-protobuf");

// Drops what the library kept (signing out).
void forget_library();

}  // namespace spotify::detail
