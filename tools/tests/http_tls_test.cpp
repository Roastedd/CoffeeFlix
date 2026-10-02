// Real HTTPS fixtures distinguish trust failures, bad CA files and interrupted
// handshakes, while keeping personal URL paths out of the log.
#include <cassert>
#include <iostream>
#include "core/http.hpp"

static http::Response request(const char* url) {
    http::Request r;
    r.url = url; r.private_url = true; r.timeout = 5; r.fresh_connection = true;
    return http::perform(r);
}
int main(int argc, char** argv) {
    assert(argc == 7);
    http::init(argv[1]);
    auto good = request(argv[3]);
    assert(good.ok() && good.body == "trusted");
    auto handshake = request(argv[4]);
    assert(!handshake.ok() && handshake.error.find("secure handshake failed") != std::string::npos);
    assert(handshake.error.find("date and time") == std::string::npos);
    auto untrusted = request(argv[5]);
    assert(!untrusted.ok() && untrusted.error.find("certificate could not be verified") != std::string::npos);
    http::shutdown();
    http::init(argv[2]);
    auto bundle = request(argv[6]);
    assert(!bundle.ok() && bundle.error.find("certificate bundle could not be loaded") != std::string::npos);
    assert(bundle.error.find("date and time") == std::string::npos);
    assert(http::verify_tls());
    http::shutdown();
    std::cout << "PASS trusted HTTPS, untrusted certificate, failed handshake, and invalid CA bundle" << std::endl;
}
