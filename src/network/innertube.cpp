#include "network/innertube.hpp"
#include "network/http_client.hpp"
#include "logger/logger.hpp"

#include <jansson.h>
#include <regex>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace InnerTube {

// Public API key from YouTube mobile web client
const char* API_KEY = "AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
const char* API_BASE_URL = "https://www.youtube.com/youtubei/v1";

// ==================== Helper Functions ====================

/**
 * Extract video ID from various YouTube URL formats
 * Supports:
 * - https://www.youtube.com/watch?v=VIDEO_ID
 * - https://youtu.be/VIDEO_ID
 * - https://m.youtube.com/watch?v=VIDEO_ID
 * - https://youtube.com/shorts/VIDEO_ID
 */
std::string extract_video_id(const std::string& url) {
    std::string video_id;
    
    // Pattern 1: youtube.com/watch?v=VIDEO_ID
    std::regex watch_regex(R"([?&]v=([a-zA-Z0-9_-]{11}))");
    std::smatch match;
    if (std::regex_search(url, match, watch_regex)) {
        return match[1].str();
    }
    
    // Pattern 2: youtu.be/VIDEO_ID
    std::regex youtu_be_regex(R"(youtu\.be/([a-zA-Z0-9_-]{11}))");
    if (std::regex_search(url, match, youtu_be_regex)) {
        return match[1].str();
    }
    
    // Pattern 3: youtube.com/shorts/VIDEO_ID
    std::regex shorts_regex(R"(youtube\.com/shorts/([a-zA-Z0-9_-]{11}))");
    if (std::regex_search(url, match, shorts_regex)) {
        return match[1].str();
    }
    
    // If URL is already just the video ID (11 chars)
    if (url.length() == 11) {
        return url;
    }
    
    log_message(LOG_WARNING, "InnerTube", "Could not extract video ID from URL: %s", url.c_str());
    return "";
}

/**
 * Get visitor data by making request to YouTube homepage
 * This is used for some API requests to simulate a browser session
 */
std::string get_visitor_data() {
    // For now, return empty string - visitor data is optional for basic requests
    // Future enhancement: Parse from YouTube homepage response
    return "";
}

/**
 * Build API URL with key parameter
 */
std::string build_api_url(const std::string& endpoint) {
    return std::string(API_BASE_URL) + endpoint + "?key=" + API_KEY;
}

// ==================== Client Configuration ====================

ClientConfig get_android_vr_config() {
    ClientConfig config;
    config.client_name = "ANDROID_VR";
    config.client_version = "1.57.29";
    config.device_make = "Meta";
    config.device_model = "Quest 3";
    config.os_name = "Android";
    config.os_version = "12";
    config.user_agent = "com.google.android.apps.youtube.vr.oculus/1.57.29 (Linux; U; Android 12; GB) gzip";
    return config;
}

ClientConfig get_android_config() {
    ClientConfig config;
    config.client_name = "ANDROID";
    config.client_version = "19.09.37";
    config.device_make = "Apple";
    config.device_model = "iPhone16,2";  // iPhone 15 Pro Max
    config.os_name = "Android";
    config.os_version = "14";
    config.user_agent = "com.google.android.youtube/19.09.37 (Linux; U; Android 14) gzip";
    return config;
}

ClientConfig get_mweb_config() {
    ClientConfig config;
    config.client_name = "MWEB";
    config.client_version = "2.20241202.07.00";
    config.device_make = "";
    config.device_model = "";
    config.os_name = "Android";
    config.os_version = "14";
    config.user_agent = "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.6099.230 Mobile Safari/537.36";
    return config;
}

ClientConfig get_tvhtml5_config() {
    ClientConfig config;
    config.client_name = "TVHTML5_SIMPLY_EMBEDDED_PLAYER";
    config.client_version = "2.0";
    config.device_make = "";
    config.device_model = "";
    config.os_name = "";
    config.os_version = "";
    config.user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36";
    return config;
}

// ==================== Helper Functions ====================

std::string get_thumbnail_url(const std::string& video_id, const std::string& quality) {
    // quality options: "default" (120x90), "mqdefault" (320x180), "hqdefault" (480x360), "maxresdefault" (1280x720)
    return "https://i.ytimg.com/vi/" + video_id + "/" + quality + ".jpg";
}

bool load_more_results(SearchResults& results) {
    if (results.continuation_token.empty()) {
        log_message(LOG_WARNING, "InnerTube", "No continuation token available");
        return false;
    }
    
    ClientConfig config = get_mweb_config();
    std::string url = build_api_url("/search");
    std::string body = create_search_request("", config, results.continuation_token);
    
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["User-Agent"] = config.user_agent;
    
    log_message(LOG_OK, "InnerTube", "Loading more results with continuation token");
    
    HttpClient::HttpResponse response = HttpClient::post(url, body, headers, 30);
    
    if (!response.success) {
        log_message(LOG_ERROR, "InnerTube", "Load more failed: %s", response.error.c_str());
        return false;
    }
    
    // Parse new results
    SearchResults new_results = parse_search_response(response.body);
    
    // Check if parsing was successful
    if (!new_results.success || new_results.results.empty()) {
        log_message(LOG_WARNING, "InnerTube", "No new results from continuation token");
        return false;
    }
    
    // Append to existing results
    results.results.insert(results.results.end(), new_results.results.begin(), new_results.results.end());
    results.continuation_token = new_results.continuation_token;
    
    log_message(LOG_OK, "InnerTube", "Loaded %zu more results, total now %zu", 
                new_results.results.size(), results.results.size());
    
    return true;
}

// ==================== JSON Request Builders ====================

/**
 * Create JSON request body for player endpoint
 */
std::string create_player_request(const std::string& video_id, const ClientConfig& config) {
    json_t* root = json_object();
    json_t* context = json_object();
    json_t* client = json_object();
    
    // Build client object
    json_object_set_new(client, "clientName", json_string(config.client_name.c_str()));
    json_object_set_new(client, "clientVersion", json_string(config.client_version.c_str()));
    json_object_set_new(client, "osName", json_string(config.os_name.c_str()));
    json_object_set_new(client, "osVersion", json_string(config.os_version.c_str()));
    
    if (!config.device_make.empty()) {
        json_object_set_new(client, "deviceMake", json_string(config.device_make.c_str()));
    }
    if (!config.device_model.empty()) {
        json_object_set_new(client, "deviceModel", json_string(config.device_model.c_str()));
    }
    
    json_object_set_new(client, "hl", json_string("en"));
    json_object_set_new(client, "gl", json_string("US"));
    
    // Build context
    json_object_set_new(context, "client", client);
    
    // Build root
    json_object_set_new(root, "context", context);
    json_object_set_new(root, "videoId", json_string(video_id.c_str()));
    
    // Convert to string
    char* json_str = json_dumps(root, JSON_COMPACT);
    std::string result(json_str);
    
    free(json_str);
    json_decref(root);
    
    return result;
}

/**
 * Create JSON request body for search endpoint
 */
std::string create_search_request(const std::string& query, const ClientConfig& config, const std::string& continuation_token) {
    json_t* root = json_object();
    json_t* context = json_object();
    json_t* client = json_object();
    
    // Build client object
    json_object_set_new(client, "clientName", json_string(config.client_name.c_str()));
    json_object_set_new(client, "clientVersion", json_string(config.client_version.c_str()));
    json_object_set_new(client, "hl", json_string("en"));
    json_object_set_new(client, "gl", json_string("US"));
    
    json_object_set_new(context, "client", client);
    json_object_set_new(root, "context", context);
    
    if (!continuation_token.empty()) {
        json_object_set_new(root, "continuation", json_string(continuation_token.c_str()));
    } else {
        json_object_set_new(root, "query", json_string(query.c_str()));
    }
    
    char* json_str = json_dumps(root, JSON_COMPACT);
    std::string result(json_str);
    
    free(json_str);
    json_decref(root);
    
    return result;
}

/**
 * Create JSON request body for browse endpoint (trending/popular)
 */
std::string create_browse_request(const std::string& browse_id, const std::string& params, const ClientConfig& config) {
    json_t* root = json_object();
    json_t* context = json_object();
    json_t* client = json_object();
    
    // Build client object
    json_object_set_new(client, "clientName", json_string(config.client_name.c_str()));
    json_object_set_new(client, "clientVersion", json_string(config.client_version.c_str()));
    json_object_set_new(client, "hl", json_string("en"));
    json_object_set_new(client, "gl", json_string("US"));
    
    json_object_set_new(context, "client", client);
    json_object_set_new(root, "context", context);
    json_object_set_new(root, "browseId", json_string(browse_id.c_str()));
    
    if (!params.empty()) {
        json_object_set_new(root, "params", json_string(params.c_str()));
    }
    
    char* json_str = json_dumps(root, JSON_COMPACT);
    std::string result(json_str);
    
    free(json_str);
    json_decref(root);
    
    return result;
}

// ==================== JSON Response Parsers ====================

/**
 * Parse player response and extract video info including streams
 */
VideoInfo parse_player_response(const std::string& json_response) {
    VideoInfo info;
    info.success = false;
    
    json_error_t error;
    json_t* root = json_loads(json_response.c_str(), 0, &error);
    if (!root) {
        log_message(LOG_ERROR, "InnerTube", "Failed to parse JSON: %s", error.text);
        return info;
    }
    
    // Check playability status
    json_t* playability = json_object_get(root, "playabilityStatus");
    if (playability) {
        json_t* status = json_object_get(playability, "status");
        if (status && json_is_string(status)) {
            const char* status_str = json_string_value(status);
            info.playability_status = status_str;
            if (strcmp(status_str, "OK") != 0) {
                json_t* reason = json_object_get(playability, "reason");
                if (reason && json_is_string(reason)) {
                    info.playability_reason = json_string_value(reason);
                    log_message(LOG_ERROR, "InnerTube", "Video not playable: %s", json_string_value(reason));
                }
                json_decref(root);
                return info;
            }
        }
    }
    
    // Extract video details
    json_t* video_details = json_object_get(root, "videoDetails");
    if (video_details) {
        json_t* video_id = json_object_get(video_details, "videoId");
        if (video_id && json_is_string(video_id)) {
            info.video_id = json_string_value(video_id);
        }
        
        json_t* title = json_object_get(video_details, "title");
        if (title && json_is_string(title)) {
            info.title = json_string_value(title);
        }
        
        json_t* author = json_object_get(video_details, "author");
        if (author && json_is_string(author)) {
            info.author = json_string_value(author);
        }
        
        json_t* length_seconds = json_object_get(video_details, "lengthSeconds");
        if (length_seconds && json_is_string(length_seconds)) {
            info.duration_seconds = std::stoi(json_string_value(length_seconds));
        }
        
        json_t* view_count = json_object_get(video_details, "viewCount");
        if (view_count && json_is_string(view_count)) {
            info.view_count = std::stoull(json_string_value(view_count));
        }
    }
    
    // Extract streaming data
    json_t* streaming_data = json_object_get(root, "streamingData");
    if (!streaming_data) {
        log_message(LOG_ERROR, "InnerTube", "No streaming data available");
        json_decref(root);
        return info;
    }
    
    // Parse formats (combined audio+video streams)
    json_t* formats = json_object_get(streaming_data, "formats");
    if (formats && json_is_array(formats)) {
        size_t index;
        json_t* format;
        json_array_foreach(formats, index, format) {
            VideoStream stream;
            
            json_t* itag = json_object_get(format, "itag");
            if (itag && json_is_integer(itag)) {
                stream.itag = json_integer_value(itag);
            }
            
            json_t* url = json_object_get(format, "url");
            if (url && json_is_string(url)) {
                stream.url = json_string_value(url);
            }
            
            json_t* mime_type = json_object_get(format, "mimeType");
            if (mime_type && json_is_string(mime_type)) {
                stream.mime_type = json_string_value(mime_type);
            }
            
            json_t* width = json_object_get(format, "width");
            if (width && json_is_integer(width)) {
                stream.width = json_integer_value(width);
            }
            
            json_t* height = json_object_get(format, "height");
            if (height && json_is_integer(height)) {
                stream.height = json_integer_value(height);
            }
            
            json_t* bitrate = json_object_get(format, "bitrate");
            if (bitrate && json_is_integer(bitrate)) {
                stream.bitrate = json_integer_value(bitrate);
            }
            
            // Only include H.264 + AAC streams
            if (stream.mime_type.find("video/mp4") != std::string::npos &&
                stream.mime_type.find("avc1") != std::string::npos) {
                stream.has_audio = true;
                stream.has_video = true;
                info.video_streams.push_back(stream);
                // Store itag 18 as preferred combined stream
                if (stream.itag == 18) {
                    info.combined_stream_url = stream.url;
                }
            }
        }
    }
    
    // Parse adaptive formats (separate audio/video streams)
    json_t* adaptive_formats = json_object_get(streaming_data, "adaptiveFormats");
    if (adaptive_formats && json_is_array(adaptive_formats)) {
        size_t index;
        json_t* format;
        json_array_foreach(adaptive_formats, index, format) {
            VideoStream stream;
            
            json_t* itag = json_object_get(format, "itag");
            if (itag && json_is_integer(itag)) {
                stream.itag = json_integer_value(itag);
            }
            
            json_t* url = json_object_get(format, "url");
            if (url && json_is_string(url)) {
                stream.url = json_string_value(url);
            }
            
            json_t* mime_type = json_object_get(format, "mimeType");
            if (mime_type && json_is_string(mime_type)) {
                stream.mime_type = json_string_value(mime_type);
            }
            
            json_t* width = json_object_get(format, "width");
            if (width && json_is_integer(width)) {
                stream.width = json_integer_value(width);
            }
            
            json_t* height = json_object_get(format, "height");
            if (height && json_is_integer(height)) {
                stream.height = json_integer_value(height);
            }
            
            json_t* bitrate = json_object_get(format, "bitrate");
            if (bitrate && json_is_integer(bitrate)) {
                stream.bitrate = json_integer_value(bitrate);
            }
            
            // Include H.264 video or AAC audio streams
            bool is_h264_video = stream.mime_type.find("video/mp4") != std::string::npos &&
                                 stream.mime_type.find("avc1") != std::string::npos;
            bool is_aac_audio = stream.mime_type.find("audio/mp4") != std::string::npos &&
                                stream.mime_type.find("mp4a") != std::string::npos;
            
            if (is_h264_video) {
                stream.has_video = true;
                stream.has_audio = false;
                info.video_streams.push_back(stream);
            } else if (is_aac_audio) {
                stream.has_video = false;
                stream.has_audio = true;
                info.audio_streams.push_back(stream);
            }
        }
    }
    
    info.success = !info.video_streams.empty() || !info.audio_streams.empty();
    
    if (info.success) {
        log_message(LOG_OK, "InnerTube", "Parsed video: %s (%zu video, %zu audio streams)",
                   info.title.c_str(), info.video_streams.size(), info.audio_streams.size());
    }
    
    json_decref(root);
    return info;
}

/**
 * Parse search response and extract video results
 */
SearchResults parse_search_response(const std::string& json_response) {
    SearchResults results;
    results.success = false;
    
    json_error_t error;
    json_t* root = json_loads(json_response.c_str(), 0, &error);
    if (!root) {
        log_message(LOG_ERROR, "InnerTube", "Failed to parse search JSON: %s", error.text);
        return results;
    }
    
    // Navigate to contents array
    json_t* contents = json_object_get(root, "contents");
    if (!contents) {
        json_decref(root);
        return results;
    }
    
    json_t* section_list = json_object_get(contents, "twoColumnSearchResultsRenderer");
    if (!section_list) {
        section_list = json_object_get(contents, "sectionListRenderer");
    }
    
    if (!section_list) {
        json_decref(root);
        return results;
    }
    
    json_t* primary_contents = json_object_get(section_list, "primaryContents");
    if (!primary_contents) {
        json_decref(root);
        return results;
    }
    
    json_t* section_contents = json_object_get(primary_contents, "sectionListRenderer");
    if (!section_contents) {
        json_decref(root);
        return results;
    }
    
    json_t* content_array = json_object_get(section_contents, "contents");
    if (!content_array || !json_is_array(content_array)) {
        json_decref(root);
        return results;
    }
    
    // Parse video items
    size_t index;
    json_t* section;
    json_array_foreach(content_array, index, section) {
        json_t* item_section = json_object_get(section, "itemSectionRenderer");
        if (!item_section) continue;
        
        json_t* items = json_object_get(item_section, "contents");
        if (!items || !json_is_array(items)) continue;
        
        size_t item_index;
        json_t* item;
        json_array_foreach(items, item_index, item) {
            json_t* video_renderer = json_object_get(item, "videoRenderer");
            if (!video_renderer) continue;
            
            SearchResultItem result_item;
            
            json_t* video_id = json_object_get(video_renderer, "videoId");
            if (video_id && json_is_string(video_id)) {
                result_item.video_id = json_string_value(video_id);
            }
            
            json_t* title_obj = json_object_get(video_renderer, "title");
            if (title_obj) {
                json_t* runs = json_object_get(title_obj, "runs");
                if (runs && json_is_array(runs) && json_array_size(runs) > 0) {
                    json_t* first_run = json_array_get(runs, 0);
                    json_t* text = json_object_get(first_run, "text");
                    if (text && json_is_string(text)) {
                        result_item.title = json_string_value(text);
                    }
                }
            }
            
            json_t* owner_text = json_object_get(video_renderer, "ownerText");
            if (owner_text) {
                json_t* runs = json_object_get(owner_text, "runs");
                if (runs && json_is_array(runs) && json_array_size(runs) > 0) {
                    json_t* first_run = json_array_get(runs, 0);
                    json_t* text = json_object_get(first_run, "text");
                    if (text && json_is_string(text)) {
                        result_item.author = json_string_value(text);
                    }
                }
            }
            
            // Get thumbnail
            json_t* thumbnail_obj = json_object_get(video_renderer, "thumbnail");
            if (thumbnail_obj) {
                json_t* thumbnails = json_object_get(thumbnail_obj, "thumbnails");
                if (thumbnails && json_is_array(thumbnails) && json_array_size(thumbnails) > 0) {
                    // Get highest quality thumbnail (last in array)
                    json_t* best_thumb = json_array_get(thumbnails, json_array_size(thumbnails) - 1);
                    json_t* url = json_object_get(best_thumb, "url");
                    if (url && json_is_string(url)) {
                        result_item.thumbnail_url = json_string_value(url);
                    }
                }
            }
            
            // Get duration
            json_t* length_text_obj = json_object_get(video_renderer, "lengthText");
            if (length_text_obj) {
                json_t* simple_text = json_object_get(length_text_obj, "simpleText");
                if (simple_text && json_is_string(simple_text)) {
                    result_item.duration_text = json_string_value(simple_text);
                }
            }
            
            // Get view count
            json_t* view_count_obj = json_object_get(video_renderer, "viewCountText");
            if (view_count_obj) {
                json_t* simple_text = json_object_get(view_count_obj, "simpleText");
                if (simple_text && json_is_string(simple_text)) {
                    result_item.view_count_text = json_string_value(simple_text);
                }
            }
            
            // Get published date
            json_t* published_obj = json_object_get(video_renderer, "publishedTimeText");
            if (published_obj) {
                json_t* simple_text = json_object_get(published_obj, "simpleText");
                if (simple_text && json_is_string(simple_text)) {
                    result_item.published_text = json_string_value(simple_text);
                }
            }
            
            if (!result_item.video_id.empty()) {
                results.results.push_back(result_item);
            }
        }
    }
    
    // Extract continuation token for pagination
    size_t continuation_index;
    json_t* continuation_section;
    json_array_foreach(content_array, continuation_index, continuation_section) {
        json_t* continuation_item = json_object_get(continuation_section, "continuationItemRenderer");
        if (continuation_item) {
            json_t* continuation_endpoint = json_object_get(continuation_item, "continuationEndpoint");
            if (continuation_endpoint) {
                json_t* continuation_command = json_object_get(continuation_endpoint, "continuationCommand");
                if (continuation_command) {
                    json_t* token = json_object_get(continuation_command, "token");
                    if (token && json_is_string(token)) {
                        results.continuation_token = json_string_value(token);
                    }
                }
            }
        }
    }
    
    results.success = !results.results.empty();
    log_message(LOG_OK, "InnerTube", "Parsed %zu search results", results.results.size());
    
    json_decref(root);
    return results;
}

// ==================== Public API Functions ====================

/**
 * Get video information and stream URLs
 */
VideoInfo get_video_info(const std::string& video_id, bool use_android_vr) {
    VideoInfo info;
    info.success = false;
    
    if (video_id.empty()) {
        log_message(LOG_ERROR, "InnerTube", "Empty video ID");
        return info;
    }
    
    // Choose client config
    ClientConfig config = use_android_vr ? get_android_vr_config() : get_android_config();
    
    // Build request
    std::string url = build_api_url("/player");
    std::string body = create_player_request(video_id, config);
    
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["User-Agent"] = config.user_agent;
    
    log_message(LOG_OK, "InnerTube", "Requesting video info for: %s (client: %s)", 
               video_id.c_str(), config.client_name.c_str());
    
    // Make request
    HttpClient::HttpResponse response = HttpClient::post(url, body, headers, 30);
    
    if (!response.success) {
        log_message(LOG_ERROR, "InnerTube", "Failed to get video info: %s", response.error.c_str());
        return info;
    }
    
    // Parse response
    info = parse_player_response(response.body);
    return info;
}

/**
 * Search for videos
 */
SearchResults search_videos(const std::string& query, const std::string& continuation_token) {
    SearchResults results;
    results.success = false;
    
    if (query.empty() && continuation_token.empty()) {
        log_message(LOG_ERROR, "InnerTube", "Empty search query and continuation token");
        return results;
    }
    
    // Use MWEB client for search
    ClientConfig config = get_mweb_config();
    
    // Build request
    std::string url = build_api_url("/search");
    std::string body = create_search_request(query, config, continuation_token);
    
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["User-Agent"] = config.user_agent;
    
    log_message(LOG_OK, "InnerTube", "Searching for: %s", query.c_str());
    
    // Make request
    HttpClient::HttpResponse response = HttpClient::post(url, body, headers, 30);
    
    if (!response.success) {
        log_message(LOG_ERROR, "InnerTube", "Search failed: %s", response.error.c_str());
        return results;
    }
    
    // Parse response
    results = parse_search_response(response.body);
    return results;
}

/**
 * Get best stream URL for a video
 * Priority: itag 18 (360p combined) > higher quality combined > adaptive video streams
 */
std::string get_best_stream_url(const VideoInfo& info, VideoQuality quality) {
    if (!info.success) {
        return "";
    }
    
    // Priority 1: Use combined_stream_url if available (itag 18)
    if (!info.combined_stream_url.empty()) {
        log_message(LOG_OK, "InnerTube", "Selected itag 18 (360p combined stream)");
        return info.combined_stream_url;
    }
    
    // Priority 2: Find best video stream with audio
    if (!info.video_streams.empty()) {
        const VideoStream* best = nullptr;
        
        for (const auto& stream : info.video_streams) {
            if (stream.has_audio && stream.has_video) {
                if (!best || stream.height > best->height) {
                    best = &stream;
                }
            }
        }
        
        if (best) {
            log_message(LOG_OK, "InnerTube", "Selected itag %d (%dx%d combined stream)",
                       best->itag, best->width, best->height);
            return best->url;
        }
        
        // Priority 3: Fall back to video-only stream
        for (const auto& stream : info.video_streams) {
            if (stream.has_video) {
                log_message(LOG_WARNING, "InnerTube", "Selected video-only itag %d (no audio)",
                           stream.itag);
                return stream.url;
            }
        }
    }
    
    log_message(LOG_ERROR, "InnerTube", "No compatible streams found");
    return "";
}

/**
 * Parse browse response (for trending/popular)
 */
SearchResults parse_browse_response(const std::string& json_response) {
    SearchResults results;
    results.success = false;
    
    json_error_t error;
    json_t* root = json_loads(json_response.c_str(), 0, &error);
    if (!root) {
        log_message(LOG_ERROR, "InnerTube", "Failed to parse browse JSON: %s", error.text);
        return results;
    }
    
    // Navigate to contents
    json_t* contents = json_object_get(root, "contents");
    if (!contents) {
        json_decref(root);
        return results;
    }
    
    json_t* two_col = json_object_get(contents, "twoColumnBrowseResultsRenderer");
    if (!two_col) {
        json_decref(root);
        return results;
    }
    
    json_t* tabs = json_object_get(two_col, "tabs");
    if (!tabs || !json_is_array(tabs) || json_array_size(tabs) == 0) {
        json_decref(root);
        return results;
    }
    
    // Get first tab
    json_t* tab = json_array_get(tabs, 0);
    json_t* tab_renderer = json_object_get(tab, "tabRenderer");
    if (!tab_renderer) {
        json_decref(root);
        return results;
    }
    
    json_t* tab_content = json_object_get(tab_renderer, "content");
    if (!tab_content) {
        json_decref(root);
        return results;
    }
    
    json_t* section_list = json_object_get(tab_content, "sectionListRenderer");
    if (!section_list) {
        json_decref(root);
        return results;
    }
    
    json_t* section_contents = json_object_get(section_list, "contents");
    if (!section_contents || !json_is_array(section_contents)) {
        json_decref(root);
        return results;
    }
    
    // Parse video items
    size_t section_index;
    json_t* section;
    json_array_foreach(section_contents, section_index, section) {
        json_t* item_section = json_object_get(section, "itemSectionRenderer");
        if (!item_section) continue;
        
        json_t* section_items = json_object_get(item_section, "contents");
        if (!section_items || !json_is_array(section_items)) continue;
        
        size_t item_index;
        json_t* item;
        json_array_foreach(section_items, item_index, item) {
            // Check for shelfRenderer (trending with horizontal list)
            json_t* shelf_renderer = json_object_get(item, "shelfRenderer");
            if (shelf_renderer) {
                json_t* shelf_content = json_object_get(shelf_renderer, "content");
                if (shelf_content) {
                    json_t* horizontal_list = json_object_get(shelf_content, "horizontalListRenderer");
                    if (horizontal_list) {
                        json_t* horizontal_items = json_object_get(horizontal_list, "items");
                        if (horizontal_items && json_is_array(horizontal_items)) {
                            size_t h_index;
                            json_t* h_item;
                            json_array_foreach(horizontal_items, h_index, h_item) {
                                json_t* grid_video = json_object_get(h_item, "gridVideoRenderer");
                                if (grid_video) {
                                    SearchResultItem result_item;
                                    
                                    json_t* video_id = json_object_get(grid_video, "videoId");
                                    if (video_id && json_is_string(video_id)) {
                                        result_item.video_id = json_string_value(video_id);
                                    }
                                    
                                    json_t* title_obj = json_object_get(grid_video, "title");
                                    if (title_obj) {
                                        json_t* runs = json_object_get(title_obj, "runs");
                                        if (runs && json_is_array(runs) && json_array_size(runs) > 0) {
                                            json_t* first_run = json_array_get(runs, 0);
                                            json_t* text = json_object_get(first_run, "text");
                                            if (text && json_is_string(text)) {
                                                result_item.title = json_string_value(text);
                                            }
                                        }
                                    }
                                    
                                    // Get thumbnail
                                    json_t* thumbnail_obj = json_object_get(grid_video, "thumbnail");
                                    if (thumbnail_obj) {
                                        json_t* thumbnails = json_object_get(thumbnail_obj, "thumbnails");
                                        if (thumbnails && json_is_array(thumbnails) && json_array_size(thumbnails) > 0) {
                                            json_t* best_thumb = json_array_get(thumbnails, json_array_size(thumbnails) - 1);
                                            json_t* url = json_object_get(best_thumb, "url");
                                            if (url && json_is_string(url)) {
                                                result_item.thumbnail_url = json_string_value(url);
                                            }
                                        }
                                    }
                                    
                                    // Get author from shortBylineText
                                    json_t* owner_text = json_object_get(grid_video, "shortBylineText");
                                    if (owner_text) {
                                        json_t* runs = json_object_get(owner_text, "runs");
                                        if (runs && json_is_array(runs) && json_array_size(runs) > 0) {
                                            json_t* first_run = json_array_get(runs, 0);
                                            json_t* text = json_object_get(first_run, "text");
                                            if (text && json_is_string(text)) {
                                                result_item.author = json_string_value(text);
                                            }
                                        }
                                    }
                                    
                                    if (!result_item.video_id.empty()) {
                                        results.results.push_back(result_item);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            
            // Check for regular videoRenderer
            json_t* video_renderer = json_object_get(item, "videoRenderer");
            if (video_renderer) {
                SearchResultItem result_item;
                
                json_t* video_id = json_object_get(video_renderer, "videoId");
                if (video_id && json_is_string(video_id)) {
                    result_item.video_id = json_string_value(video_id);
                }
                
                json_t* title_obj = json_object_get(video_renderer, "title");
                if (title_obj) {
                    json_t* runs = json_object_get(title_obj, "runs");
                    if (runs && json_is_array(runs) && json_array_size(runs) > 0) {
                        json_t* first_run = json_array_get(runs, 0);
                        json_t* text = json_object_get(first_run, "text");
                        if (text && json_is_string(text)) {
                            result_item.title = json_string_value(text);
                        }
                    }
                }
                
                // Get thumbnail
                json_t* thumbnail_obj = json_object_get(video_renderer, "thumbnail");
                if (thumbnail_obj) {
                    json_t* thumbnails = json_object_get(thumbnail_obj, "thumbnails");
                    if (thumbnails && json_is_array(thumbnails) && json_array_size(thumbnails) > 0) {
                        json_t* best_thumb = json_array_get(thumbnails, json_array_size(thumbnails) - 1);
                        json_t* url = json_object_get(best_thumb, "url");
                        if (url && json_is_string(url)) {
                            result_item.thumbnail_url = json_string_value(url);
                        }
                    }
                }
                
                json_t* owner_text = json_object_get(video_renderer, "ownerText");
                if (owner_text) {
                    json_t* runs = json_object_get(owner_text, "runs");
                    if (runs && json_is_array(runs) && json_array_size(runs) > 0) {
                        json_t* first_run = json_array_get(runs, 0);
                        json_t* text = json_object_get(first_run, "text");
                        if (text && json_is_string(text)) {
                            result_item.author = json_string_value(text);
                        }
                    }
                }
                
                if (!result_item.video_id.empty()) {
                    results.results.push_back(result_item);
                }
            }
        }
    }
    
    results.success = !results.results.empty();
    log_message(LOG_OK, "InnerTube", "Parsed %zu browse results", results.results.size());
    
    json_decref(root);
    return results;
}

/**
 * Get trending videos
 */
SearchResults get_trending() {
    SearchResults results;
    results.success = false;
    
    ClientConfig config = get_mweb_config();
    
    // Build request for trending
    // browseId: FEtrending
    // params: 4gIOGgxtb3N0X3BvcHVsYXI%3D (protobuf: 44 { 3: "most_popular"})
    std::string url = build_api_url("/browse");
    std::string body = create_browse_request("FEtrending", "4gIOGgxtb3N0X3BvcHVsYXI%3D", config);
    
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/json";
    headers["User-Agent"] = config.user_agent;
    
    log_message(LOG_OK, "InnerTube", "Fetching trending videos");
    
    // Make request
    HttpClient::HttpResponse response = HttpClient::post(url, body, headers, 30);
    
    if (!response.success) {
        log_message(LOG_ERROR, "InnerTube", "Trending fetch failed: %s", response.error.c_str());
        return results;
    }
    
    // Parse response
    results = parse_browse_response(response.body);
    return results;
}

/**
 * Get popular videos (same as trending for now)
 */
SearchResults get_popular() {
    // Popular is the same as trending
    return get_trending();
}

} // namespace InnerTube
