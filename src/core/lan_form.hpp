// A web page on the home network for typing on a phone what's long to type on the GamePad
// (playlist links, logins). It runs while a screen holds it, at a link with a random part, so
// only whoever sees the screen (its QR code) can send anything; what's sent goes to the screen,
// which shows it before anything is saved.
#pragma once

#include <map>
#include <memory>
#include <string>

namespace lan_form {

class Server {
public:
    // `page`: the form, which posts to its own address. `sent`: the answer after a post.
    Server(std::string page, std::string sent);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // "http://192.168.1.42:8080/3fa9c1e07b52", "" when there's no network (or no free port).
    const std::string& url() const { return url_; }
    // Main thread: the fields of the form sent last, once.
    bool take(std::map<std::string, std::string>& fields);

    struct State;

private:
    std::shared_ptr<State> s_;
    std::string url_;
};

}  // namespace lan_form
