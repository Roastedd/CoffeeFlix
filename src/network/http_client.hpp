#ifndef HTTP_CLIENT_HPP
#define HTTP_CLIENT_HPP

#include <string>
#include <map>

namespace HttpClient {
    struct HttpResponse {
        int status_code;
        std::string body;
        std::map<std::string, std::string> headers;
        std::string error;
        bool success;
    };

    /**
     * Perform HTTP/HTTPS GET request using libcurl
     * @param url Full URL (http:// or https://)
     * @param timeout_seconds Timeout in seconds (default 10)
     * @return HttpResponse with status, body, and headers
     */
    HttpResponse get(const std::string& url, int timeout_seconds = 10);
    
    /**
     * Perform HTTP/HTTPS POST request with JSON body
     * @param url Full URL (http:// or https://)
     * @param body Request body (typically JSON string)
     * @param headers Custom headers (e.g., Content-Type, Authorization)
     * @param timeout_seconds Timeout in seconds (default 10)
     * @return HttpResponse with status, body, and headers
     */
    HttpResponse post(const std::string& url,
                      const std::string& body,
                      const std::map<std::string, std::string>& headers = {},
                      int timeout_seconds = 10);
}

#endif // HTTP_CLIENT_HPP
