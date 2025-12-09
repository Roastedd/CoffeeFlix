#include "network/http_client.hpp"
#include "logger/logger.hpp"

#include <curl/curl.h>
#include <cstring>
#include <sstream>

namespace HttpClient {

// Callback function for curl to write data
static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total_size = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), total_size);
    return total_size;
}

// Callback for headers
static size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total_size = size * nitems;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    
    std::string header_line(buffer, total_size);
    size_t colon_pos = header_line.find(':');
    
    if (colon_pos != std::string::npos) {
        std::string key = header_line.substr(0, colon_pos);
        std::string value = header_line.substr(colon_pos + 1);
        
        // Trim whitespace
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        
        headers->insert({key, value});
    }
    
    return total_size;
}

HttpResponse get(const std::string& url, int timeout_seconds) {
    HttpResponse response;
    response.status_code = 0;
    response.success = false;
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to initialize curl";
        log_message(LOG_ERROR, "HTTP", "curl_easy_init failed");
        return response;
    }
    
    log_message(LOG_OK, "HTTP", "Requesting: %s", url.c_str());
    
    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    
    // Set callbacks
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    
    // Set timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
    
    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    
    // Set user agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CoffeeFlix/1.0 (Nintendo Wii U)");
    
    // Enable SSL/TLS support (important for HTTPS!)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // Don't verify cert (Wii U may not have CA bundle)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    // Perform request
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        response.error = std::string("curl_easy_perform failed: ") + curl_easy_strerror(res);
        log_message(LOG_ERROR, "HTTP", "Request failed: %s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return response;
    }
    
    // Get response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    response.status_code = static_cast<int>(http_code);
    
    response.success = (response.status_code >= 200 && response.status_code < 300);
    
    if (response.success) {
        log_message(LOG_OK, "HTTP", "Request successful: %d, body size: %zu", 
                   response.status_code, response.body.size());
    } else {
        log_message(LOG_WARNING, "HTTP", "Request returned status: %d", response.status_code);
    }
    
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse post(const std::string& url,
                  const std::string& body,
                  const std::map<std::string, std::string>& headers,
                  int timeout_seconds) {
    HttpResponse response;
    response.status_code = 0;
    response.success = false;
    
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to initialize curl";
        log_message(LOG_ERROR, "HTTP", "curl_easy_init failed");
        return response;
    }
    
    log_message(LOG_OK, "HTTP", "POST Request: %s (body size: %zu)", url.c_str(), body.size());
    
    // Set URL
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    
    // Enable POST
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body.size());
    
    // Set callbacks
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response.headers);
    
    // Set timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
    
    // Follow redirects
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    
    // Set user agent
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CoffeeFlix/1.0 (Nintendo Wii U)");
    
    // Enable SSL/TLS support
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    
    // Set custom headers
    struct curl_slist* header_list = nullptr;
    for (const auto& header : headers) {
        std::string header_str = header.first + ": " + header.second;
        header_list = curl_slist_append(header_list, header_str.c_str());
    }
    if (header_list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }
    
    // Perform request
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        response.error = std::string("curl_easy_perform failed: ") + curl_easy_strerror(res);
        log_message(LOG_ERROR, "HTTP", "POST Request failed: %s", curl_easy_strerror(res));
        if (header_list) curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        return response;
    }
    
    // Get response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    response.status_code = static_cast<int>(http_code);
    
    response.success = (response.status_code >= 200 && response.status_code < 300);
    
    if (response.success) {
        log_message(LOG_OK, "HTTP", "POST Request successful: %d, response size: %zu", 
                   response.status_code, response.body.size());
    } else {
        log_message(LOG_WARNING, "HTTP", "POST Request returned status: %d", response.status_code);
    }
    
    if (header_list) curl_slist_free_all(header_list);
    curl_easy_cleanup(curl);
    return response;
}

} // namespace HttpClient
