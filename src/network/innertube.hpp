#ifndef INNERTUBE_HPP
#define INNERTUBE_HPP

#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace InnerTube {
    
    // YouTube InnerTube API Key (public, from mobile web client)
    extern const char* API_KEY;
    extern const char* API_BASE_URL;
    
    // Video quality levels
    enum VideoQuality {
        QUALITY_360P,
        QUALITY_480P,
        QUALITY_720P,
        QUALITY_AUTO
    };
    
    // Video stream information
    struct VideoStream {
        std::string url;
        int itag;                    // YouTube format identifier
        std::string mime_type;       // e.g., "video/mp4; codecs=\"avc1.4d401f\""
        std::string quality_label;   // e.g., "360p", "480p"
        int width;
        int height;
        int bitrate;
        int64_t content_length;
        bool has_audio;              // True for combined streams (itag 18)
        bool has_video;
    };
    
    // Complete video information
    struct VideoInfo {
        std::string video_id;
        std::string title;
        std::string author;
        std::string author_id;
        std::string thumbnail_url;
        int duration_seconds;
        int64_t view_count;
        std::string description;
        
        // Stream URLs
        std::vector<VideoStream> video_streams;
        std::vector<VideoStream> audio_streams;
        std::string combined_stream_url;    // itag 18 (360p H.264+AAC)
        
        // Status
        bool success;
        std::string error;
        std::string playability_status;     // "OK", "UNPLAYABLE", etc.
        std::string playability_reason;
    };
    
    // Search result item
    struct SearchResultItem {
        std::string video_id;
        std::string title;
        std::string author;
        std::string author_id;
        std::string thumbnail_url;          // URL to video thumbnail
        std::string duration_text;          // e.g., "3:45"
        std::string view_count_text;        // e.g., "1.2M views"
        std::string published_text;         // e.g., "2 days ago"
        int duration_seconds;
        int64_t view_count;
    };
    
    // Search results
    struct SearchResults {
        std::vector<SearchResultItem> results;
        std::string continuation_token;     // For pagination
        bool success;
        std::string error;
    };
    
    // Client configuration for spoofing
    struct ClientConfig {
        std::string client_name;
        std::string client_version;
        std::string device_make;
        std::string device_model;
        std::string os_name;
        std::string os_version;
        std::string android_sdk_version;
        std::string user_agent;
    };
    
    /**
     * Get predefined client configurations
     */
    ClientConfig get_android_vr_config();
    ClientConfig get_android_config();
    ClientConfig get_mweb_config();
    ClientConfig get_tvhtml5_config();
    
    /**
     * Extract YouTube video ID from various URL formats
     * Supports:
     * - youtube.com/watch?v=VIDEO_ID
     * - youtu.be/VIDEO_ID
     * - youtube.com/embed/VIDEO_ID
     * - m.youtube.com/watch?v=VIDEO_ID
     * 
     * @param url YouTube URL or video ID
     * @return 11-character video ID, or empty string if invalid
     */
    std::string extract_video_id(const std::string& url);
    
    /**
     * Get visitor data for session management
     * Helps avoid bot detection by YouTube
     * Caches result for reuse
     * 
     * @return visitor_data string
     */
    std::string get_visitor_data();
    
    /**
     * Fetch video information from InnerTube API
     * Uses /youtubei/v1/player endpoint
     * 
     * @param video_id 11-character YouTube video ID
     * @param use_android_vr Use ANDROID_VR client (better quality, may fail for some videos)
     * @return VideoInfo with streams and metadata
     */
    VideoInfo get_video_info(const std::string& video_id, bool use_android_vr = false);
    
    /**
     * Search for videos using InnerTube API
     * Uses /youtubei/v1/search endpoint
     * 
     * @param query Search query string
     * @param continuation_token Optional continuation token for pagination
     * @return SearchResults with video list
     */
    SearchResults search_videos(const std::string& query, const std::string& continuation_token = "");
    
    /**
     * Get trending videos
     * Uses /youtubei/v1/browse endpoint with trending browse ID
     * 
     * @return SearchResults with trending videos
     */
    SearchResults get_trending();
    
    /**
     * Get popular videos
     * Uses /youtubei/v1/browse endpoint
     * 
     * @return SearchResults with popular videos
     */
    SearchResults get_popular();
    
    /**
     * Generate thumbnail URL for a video
     * 
     * @param video_id YouTube video ID
     * @param quality "default", "mqdefault", "hqdefault", "maxresdefault"
     * @return Thumbnail URL
     */
    std::string get_thumbnail_url(const std::string& video_id, const std::string& quality = "mqdefault");
    
    /**
     * Load more search results using continuation token
     * Modifies the SearchResults object in place
     * 
     * @param results SearchResults object to append to
     * @return true if more results loaded, false on error
     */
    bool load_more_results(SearchResults& results);
    
    /**
     * Select best stream for Wii U playback
     * Priority:
     * 1. Combined stream (itag 18) - 360p H.264+AAC
     * 2. 360p video stream + audio stream
     * 3. 480p video stream + audio stream (if available)
     * 
     * @param video VideoInfo with available streams
     * @param preferred_quality Preferred quality level
     * @return VideoStream with best compatible stream
     */
    VideoStream select_best_stream(const VideoInfo& video, VideoQuality preferred_quality = QUALITY_360P);
    
    /**
     * Get best stream URL for playback
     * Returns the URL of the best available stream based on quality preference
     * 
     * @param info VideoInfo with available streams
     * @param quality Preferred quality level
     * @return Stream URL or empty string if no compatible streams
     */
    std::string get_best_stream_url(const VideoInfo& info, VideoQuality quality = QUALITY_360P);
    
    /**
     * Build InnerTube API URL for given endpoint
     * 
     * @param endpoint API endpoint name (e.g., "player", "search", "browse")
     * @return Complete URL with API key
     */
    std::string build_api_url(const std::string& endpoint);
    
    /**
     * Create player API request body
     * 
     * @param video_id YouTube video ID
     * @param visitor_data Session visitor data
     * @param config Client configuration
     * @return JSON request body as string
     */
    std::string create_player_request(const std::string& video_id, 
                                     const std::string& visitor_data,
                                     const ClientConfig& config);
    
    /**
     * Create search API request body
     * 
     * @param query Search query
     * @param config Client configuration
     * @param continuation_token Optional token for pagination
     * @return JSON request body as string
     */
    std::string create_search_request(const std::string& query,
                                     const ClientConfig& config,
                                     const std::string& continuation_token = "");
    
    /**
     * Create browse API request body
     * 
     * @param browse_id Browse ID (e.g., "FEtrending")
     * @param params Optional params (e.g., "4gIOGgxtb3N0X3BvcHVsYXI%3D" for trending)
     * @param config Client configuration
     * @return JSON request body as string
     */
    std::string create_browse_request(const std::string& browse_id,
                                     const std::string& params,
                                     const ClientConfig& config);
    
    /**
     * Parse player API response and extract video info
     * 
     * @param json_response Raw JSON response string
     * @return VideoInfo populated from response
     */
    VideoInfo parse_player_response(const std::string& json_response);
    
    /**
     * Parse search API response and extract results
     * 
     * @param json_response Raw JSON response string
     * @return SearchResults populated from response
     */
    SearchResults parse_search_response(const std::string& json_response);
    
    /**
     * Parse browse API response (trending/popular)
     * 
     * @param json_response Raw JSON response string
     * @return SearchResults populated from response
     */
    SearchResults parse_browse_response(const std::string& json_response);
    
} // namespace InnerTube

#endif // INNERTUBE_HPP
