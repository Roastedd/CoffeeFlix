#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace util {

std::string url_encode(std::string_view s);
std::string url_decode(std::string_view s);
std::string html_to_text(std::string_view html);  // strips tags, decodes entities
std::string trim(std::string_view s);
std::string lower(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
bool ends_with(std::string_view s, std::string_view suffix);
bool icontains(std::string_view haystack, std::string_view needle);
std::vector<std::string> split(std::string_view s, char sep);
std::string replace_all(std::string s, std::string_view from, std::string_view to);
std::string file_extension(std::string_view path);  // lower-case, without dot
std::string file_name(std::string_view path);
std::string parent_dir(std::string_view path);
std::string join_path(std::string_view a, std::string_view b);

std::string format_duration(double seconds);          // 1:02:03 / 4:05
std::string format_count(int64_t n);                  // 1.2K, 3.4M
std::string format_bytes(uint64_t n);
std::string format_ticks_duration(int64_t ticks);     // .NET ticks (100ns) -> "1h 42m"
std::string fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));

uint64_t hash64(std::string_view s, uint64_t seed = 1469598103934665603ull);
std::string random_hex(int bytes);
double now_seconds();          // monotonic
int64_t unix_time();           // wall clock seconds
std::string clock_hhmm();      // local time for the status bar

bool file_exists(const std::string& path);
bool dir_exists(const std::string& path);
bool make_dirs(const std::string& path);
bool read_file(const std::string& path, std::string& out);
bool write_file_atomic(const std::string& path, std::string_view data);

}  // namespace util
