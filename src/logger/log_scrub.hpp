// Takes private data out of log text before it leaves the console (Settings > Send logs).
#pragma once

#include <string>
#include <string_view>
#include <vector>

// Returns `text` with "[removed]" in place of:
// - each of `secrets` (values from the settings: sign-in tokens, device IDs...), word for word;
//   ones under 6 characters are left alone, as they could be ordinary text;
// - the query string of links that are signed for one viewer (googlevideo.com, Twitch) or that
//   have a private parameter (token, sig, key, ip, deviceId...), and user names and passwords
//   in links;
// - the values of Authorization and Cookie headers and of token, password, visitorData,
//   device ID and similar fields, as headers, JSON or key=value;
// - bearer tokens, Google sign-in tokens and API keys, JWTs and YouTube visitor data by their
//   shape, email addresses, and IPv4 addresses outside the private ranges.
std::string scrub_log(std::string_view text, const std::vector<std::string>& secrets);
