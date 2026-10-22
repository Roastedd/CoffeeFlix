#include "core/lock.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/sha256.hpp"
#include "core/store.hpp"
#include "core/util.hpp"

namespace lock {

namespace {

constexpr int ROUNDS = 3000;
std::mutex g_m;
int64_t g_until = 0;  // kept in memory too, so changing the file's clock doesn't help

std::string digest(const std::string& text) {
    sha256::Hasher h;
    h.update(text.data(), text.size());
    return h.hex();
}

std::string stretch(const std::string& salt, const std::string& code) {
    std::string h = digest(salt + ":" + code);
    for (int i = 0; i < ROUNDS; i++) h = digest(h + salt + code);
    return h;
}

int wait_for_fails(int64_t fails) { return fails >= 7 ? 3600 : fails >= 5 ? 300 : fails >= 3 ? 30 : 0; }

int remaining() {
    const int64_t until = std::max(g_until, store::get_int("lock_until", 0));
    const int64_t left = until - util::unix_time();
    return left > 0 ? (int)left : 0;
}

}  // namespace

bool is_set() { return !store::get_str("lock_hash").empty(); }

Kind kind() {
    if (!is_set()) return Kind::NONE;
    return store::get_str("lock_kind") == "buttons" ? Kind::BUTTONS : Kind::PIN;
}

size_t length() { return is_set() ? (size_t)store::get_int("lock_length", 0) : 0; }

bool valid(Kind k, const std::string& code) {
    if (k == Kind::PIN) {
        if (code.size() != PIN_LENGTH) return false;
        for (char c : code)
            if (c < '0' || c > '9') return false;
        return true;
    }
    if (k == Kind::BUTTONS) {
        if (code.size() < MIN_BUTTONS || code.size() > MAX_BUTTONS) return false;
        for (char c : code)
            if (!strchr(BUTTON_SYMBOLS, c)) return false;
        return true;
    }
    return false;
}

bool set(Kind k, const std::string& code) {
    if (!valid(k, code)) return false;
    std::lock_guard<std::mutex> lk(g_m);
    const std::string salt = util::secure_random_hex(16);
    store::set_str("lock_kind", k == Kind::BUTTONS ? "buttons" : "pin");
    store::set_str("lock_salt", salt);
    store::set_int("lock_length", (int64_t)code.size());
    store::set_str("lock_hash", stretch(salt, code));
    store::set_int("lock_fails", 0);
    store::set_int("lock_until", 0);
    g_until = 0;
    return true;
}

void clear() {
    std::lock_guard<std::mutex> lk(g_m);
    for (const char* k : {"lock_kind", "lock_salt", "lock_hash"}) store::set_str(k, "");
    for (const char* k : {"lock_length", "lock_fails", "lock_until"}) store::set_int(k, 0);
    g_until = 0;
}

int wait_seconds() {
    std::lock_guard<std::mutex> lk(g_m);
    return remaining();
}

Result check(const std::string& code) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!is_set()) return Result::OK;
    if (remaining() > 0) return Result::LOCKED_OUT;
    const bool ok = stretch(store::get_str("lock_salt"), code) == store::get_str("lock_hash");
    if (ok) {
        store::set_int("lock_fails", 0);
        store::set_int("lock_until", 0);
        g_until = 0;
        return Result::OK;
    }
    const int64_t fails = store::get_int("lock_fails", 0) + 1;
    store::set_int("lock_fails", fails);
    const int wait = wait_for_fails(fails);
    if (wait > 0) {
        g_until = util::unix_time() + wait;
        store::set_int("lock_until", g_until);
    }
    return Result::WRONG;
}

void init(const std::string& data_dir) {
    const std::string flag = data_dir + "/reset-lock.txt";
    if (!util::file_exists(flag)) return;
    clear();
    std::remove(flag.c_str());
}

}  // namespace lock
