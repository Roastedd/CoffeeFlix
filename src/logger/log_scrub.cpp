#include "logger/log_scrub.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

constexpr std::string_view REMOVED = "[removed]";
constexpr size_t npos = std::string_view::npos;

bool is_alpha(char c) { return std::isalpha((unsigned char)c); }
bool is_alnum(char c) { return std::isalnum((unsigned char)c); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || c == '\t'; }
// Tokens and IDs: base64 (plain, URL-safe or percent-encoded), with dots.
bool token_char(char c) { return is_alnum(c) || (c && std::strchr("-_.~+/=%", c)); }
bool b64url_char(char c) { return is_alnum(c) || c == '-' || c == '_'; }
bool name_char(char c) { return is_alnum(c) || c == '_'; }
// An unquoted value ends at a space or a separator.
bool value_char(char c) { return (unsigned char)c > ' ' && !std::strchr(",;&\"')]}<>", c); }
bool scheme_char(char c) { return is_alnum(c) || c == '+' || c == '-' || c == '.'; }
// A link in the log ends at a space, a quote or a bracket around it.
bool link_char(char c) { return (unsigned char)c > ' ' && !std::strchr("\"'<>`)]}", c); }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

// Where the run of characters that pass `ok` from `i` ends.
template <typename Ok>
size_t run(std::string_view s, size_t i, Ok ok) {
    while (i < s.size() && ok(s[i])) i++;
    return i;
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

std::string remove_secrets(std::string_view text, std::vector<std::string> secrets) {
    // Longest first, so one that contains another goes whole.
    std::sort(secrets.begin(), secrets.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    std::string out(text);
    for (const std::string& s : secrets) {
        if (s.size() < 6) continue;
        std::string next;
        size_t from = 0, at;
        while ((at = out.find(s, from)) != npos) {
            next.append(out, from, at - from);
            next += REMOVED;
            from = at + s.size();
        }
        if (from == 0) continue;
        next.append(out, from);
        out = std::move(next);
    }
    return out;
}

// --- links ---------------------------------------------------------------------------------

// Hosts whose links are signed for one viewer and carry their IP address, in the path too.
const std::string_view SIGNED_HOSTS[] = {"googlevideo.com", "ttvnw.net", "jtvnw.net"};

// Link parameters whose values are private: these names, or any with one of PRIVATE_PARTS in it.
const char* const PRIVATE_NAMES[] = {"ip", "expire", "expires", "sparams", "code", "policy", "email", "user", "username"};
const char* const PRIVATE_PARTS[] = {"token", "sig", "key", "auth", "pass", "secret", "session", "visitor", "device", "cookie"};

bool signed_host(std::string_view host) {
    for (std::string_view h : SIGNED_HOSTS)
        if (host == h || (host.size() > h.size() && host.substr(host.size() - h.size()) == h &&
                          host[host.size() - h.size() - 1] == '.'))
            return true;
    return false;
}

bool private_name(std::string_view name) {
    std::string n = lower(name);
    for (const char* p : PRIVATE_NAMES)
        if (n == p) return true;
    for (const char* p : PRIVATE_PARTS)
        if (n.find(p) != npos) return true;
    return false;
}

// A query string or fragment, without its ? or #.
bool private_query(std::string_view q) {
    for (size_t i = 0; i <= q.size();) {
        size_t end = std::min(q.find_first_of("&;?#", i), q.size());
        std::string_view part = q.substr(i, end - i);
        if (private_name(part.substr(0, part.find('=')))) return true;
        i = end + 1;
    }
    return false;
}

// One link: user names and passwords go, and so does the query string when it has a private
// parameter. On signed hosts only the first part of the path stays.
std::string clean_link(std::string_view url) {
    size_t auth = url.find("://") + 3;
    size_t path = std::min(url.find_first_of("/?#", auth), url.size());
    std::string_view authority = url.substr(auth, path - auth);
    std::string out(url.substr(0, auth));
    size_t at = authority.rfind('@');
    if (at != npos) {
        out += REMOVED;
        out += '@';
        authority.remove_prefix(at + 1);
    }
    out += authority;
    bool sig = signed_host(lower(authority.substr(0, authority.find(':'))));
    size_t query = std::min(url.find_first_of("?#", path), url.size());
    std::string_view p = url.substr(path, query - path), q = url.substr(query);
    size_t second = sig ? p.find('/', 1) : npos;
    out += p.substr(0, second);
    if (second != npos) {
        out += '/';
        out += REMOVED;
    }
    if (!q.empty() && (sig || private_query(q.substr(1)))) {
        out += q[0];
        out += REMOVED;
    } else {
        out += q;
    }
    return out;
}

std::string clean_links(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t done = 0;
    for (size_t at = text.find("://"); at != npos; at = text.find("://", at)) {
        size_t start = at;
        while (start > done && scheme_char(text[start - 1])) start--;
        while (start < at && !is_alpha(text[start])) start++;
        size_t end = run(text, at + 3, link_char);
        if (start == at || end == at + 3) {  // no scheme, or nothing after it
            at += 3;
            continue;
        }
        out.append(text.substr(done, start - done));
        out += clean_link(text.substr(start, end - start));
        done = at = end;
    }
    out.append(text.substr(done));
    return out;
}

// --- fields ----------------------------------------------------------------------------------

// Names of fields whose values are private, as headers, JSON or key=value; also matched after a
// "-" (X-Goog-Visitor-Id, X-Emby-Token, Set-Cookie).
const std::string_view FIELDS[] = {"authorization", "cookie",       "access_token", "refresh_token", "id_token",
                                   "token",         "device_code",  "api_key",      "apikey",        "api-key",
                                   "password",      "passwd",       "client_secret", "secret",       "visitordata",
                                   "visitor_data",  "visitor-id",   "deviceid",     "device_id",     "device-id",
                                   "sig",           "signature"};

std::string clean_fields(std::string_view text) {
    std::string low = lower(text), out;
    out.reserve(text.size());
    size_t done = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (!is_alpha(text[i]) || (i > 0 && name_char(text[i - 1]))) continue;
        for (std::string_view f : FIELDS) {
            if (low.compare(i, f.size(), f) != 0) continue;
            size_t j = i + f.size();
            if (j < text.size() && (name_char(text[j]) || text[j] == '-')) continue;
            if (j < text.size() && (text[j] == '"' || text[j] == '\'')) j++;  // the end of a JSON key
            j = run(text, j, is_space);
            if (j >= text.size() || (text[j] != ':' && text[j] != '=')) continue;
            j = run(text, j + 1, is_space);
            size_t from = j, to;
            if (j < text.size() && (text[j] == '"' || text[j] == '\'')) {
                char quote = text[j];
                from = j + 1;
                to = run(text, from, [quote](char c) { return c != quote && c != '\n'; });
            } else if (f == "authorization" || f == "cookie") {
                to = run(text, j, [](char c) { return c != '\n' && c != '\r'; });  // "Bearer ...", "a=1; b=2"
            } else {
                to = run(text, j, value_char);
            }
            if (to > from && text.substr(from, to - from) != REMOVED) {
                out.append(text.substr(done, from - done));
                out += REMOVED;
                done = to;
                i = to - 1;
            }
            break;
        }
    }
    out.append(text.substr(done));
    return out;
}

// --- shapes ----------------------------------------------------------------------------------

// Where a secret known by its shape ends when one starts at s[i], else 0; `from` is where the
// part to take out starts.
size_t shaped_secret(std::string_view s, size_t i, size_t& from) {
    std::string_view t = s.substr(i);
    from = i;
    if (lower(t.substr(0, 7)) == "bearer ") {
        from = run(s, i + 7, is_space);
        return run(s, from, token_char);
    }
    size_t end = 0, min = 0;
    if (starts_with(t, "ya29.")) {  // Google access token
        end = run(s, i + 5, token_char);
        min = 20;
    } else if (starts_with(t, "1//")) {  // Google refresh token
        end = run(s, i + 3, b64url_char);
        min = 24;
    } else if (starts_with(t, "AIza")) {  // Google API key
        end = run(s, i + 4, b64url_char);
        min = 30;
    } else if (starts_with(t, "eyJ")) {  // JWT
        end = run(s, i, [](char c) { return b64url_char(c) || c == '.'; });
        if (s.substr(i, end - i).find('.') == npos) end = 0;
        min = 30;
    } else if (starts_with(t, "Cgt")) {  // YouTube visitor data
        end = run(s, i + 3, [](char c) { return b64url_char(c) || c == '%' || c == '='; });
        min = 20;
    }
    return end >= i + min ? end : 0;
}

std::string clean_shapes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t done = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (!is_alnum(text[i]) || (i > 0 && is_alnum(text[i - 1]))) continue;
        size_t from = i, end = shaped_secret(text, i, from);
        if (end <= from) continue;
        out.append(text.substr(done, from - done));
        out += REMOVED;
        done = end;
        i = end - 1;
    }
    out.append(text.substr(done));
    return out;
}

// --- email and IP addresses --------------------------------------------------------------

bool local_part_char(char c) { return is_alnum(c) || (c && std::strchr("._%+-", c)); }
bool domain_char(char c) { return is_alnum(c) || c == '-' || c == '.'; }

std::string clean_emails(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t done = 0;
    for (size_t at = text.find('@'); at != npos; at = text.find('@', at + 1)) {
        size_t start = at;
        while (start > done && local_part_char(text[start - 1])) start--;
        size_t end = run(text, at + 1, domain_char);
        while (end > at + 1 && text[end - 1] == '.') end--;  // a full stop after it
        std::string_view domain = text.substr(at + 1, end - at - 1);
        size_t dot = domain.rfind('.');
        if (start == at || dot == npos || domain.size() - dot < 3) continue;
        if (!std::all_of(domain.begin() + dot + 1, domain.end(), is_alpha)) continue;
        out.append(text.substr(done, start - done));
        out += REMOVED;
        done = end;
    }
    out.append(text.substr(done));
    return out;
}

// Private, loopback, link-local, shared (carrier NAT) and multicast addresses say nothing about
// where the console is.
bool local_ip(const int a[4]) {
    return a[0] == 0 || a[0] == 10 || a[0] == 127 || a[0] >= 224 || (a[0] == 169 && a[1] == 254) ||
           (a[0] == 172 && a[1] >= 16 && a[1] < 32) || (a[0] == 192 && a[1] == 168) || (a[0] == 100 && a[1] >= 64 && a[1] < 128);
}

// Where an IPv4 address starting at s[i] ends, else 0.
size_t ipv4(std::string_view s, size_t i, int a[4]) {
    size_t j = i;
    for (int k = 0; k < 4; k++) {
        if (k > 0) {
            if (j >= s.size() || s[j] != '.') return 0;
            j++;
        }
        size_t start = j;
        a[k] = 0;
        while (j < s.size() && is_digit(s[j]) && j - start < 3) a[k] = a[k] * 10 + (s[j++] - '0');
        if (j == start || a[k] > 255) return 0;
    }
    // Not part of something longer (a version number)
    if (j < s.size() && (is_alnum(s[j]) || (s[j] == '.' && j + 1 < s.size() && is_digit(s[j + 1])))) return 0;
    return j;
}

std::string clean_ips(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t done = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (!is_digit(text[i]) || (i > 0 && (is_alnum(text[i - 1]) || text[i - 1] == '.'))) continue;
        int a[4] = {};
        size_t end = ipv4(text, i, a);
        if (!end) continue;
        if (!local_ip(a)) {
            out.append(text.substr(done, i - done));
            out += REMOVED;
            done = end;
        }
        i = end - 1;
    }
    out.append(text.substr(done));
    return out;
}

}  // namespace

std::string scrub_log(std::string_view text, const std::vector<std::string>& secrets) {
    std::string s = remove_secrets(text, secrets);
    s = clean_links(s);
    s = clean_fields(s);
    s = clean_shapes(s);
    s = clean_emails(s);
    return clean_ips(s);
}
