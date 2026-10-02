// Include the real updater to exercise its internal parser and file-swap logic. Production
// builds retain the compiled developer key; only this isolated test process substitutes a key.
#include "../../src/app/updater.cpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include "app/app.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--ui") {
        std::string pub;
        assert(util::read_file(argv[2], pub));
        updater::DEV_KEY = pub.c_str();
        tasks::on_main([] { app::open_section(app::SEC_SETTINGS); app::push(screens::make_update()); });
        return app::run(0, nullptr);
    }
    assert(argc == 4);
    using namespace updater;
    std::string server = argv[1], folder = argv[2], pub;
    assert(util::read_file(argv[3], pub));
    DEV_KEY = pub.c_str();
    assert(valid_build_id("otherbuild") && !valid_build_id("../escape"));
    setenv("COFFEEFLIX_DATA", folder.c_str(), 1);
    setenv("COFFEEFLIX_DEV_SERVER", server.c_str(), 1);
    assert(platform::init());
    http::init("content/cacert.pem");
    store::load(folder + "/coffeeflix.json");
    store::set_bool("update_dev_unlocked", true);
    store::set_bool("update_dev", true);
    store::set_str("update_dev_build", "coffeeflix");
    tasks::init();
    std::string bundle = folder + "/running.wuhb";
    g_bundle = bundle;
    auto response = http::get("http://" + server + "/builds.json");
    assert(response.ok());
    auto current = parse_dev_catalog(response.body, server, bundle, "coffeeflix");
    assert(current.error.empty() && current.catalog_valid && current.running && current.builds.size() == 2);
    auto other = parse_dev_catalog(response.body, server, bundle, "otherbuild");
    assert(other.error.empty() && !other.running && other.rel.build_name == "Other build");
    assert(other.rel.url.find("/builds/otherbuild/" + other.rel.sha256 + "/") != std::string::npos);
    auto missing = parse_dev_catalog(response.body, server, bundle, "missing");
    assert(missing.catalog_valid && missing.builds.size() == 2 && !missing.error.empty());
    auto tampered = json::Doc::parse(response.body);
    std::string payload = json::str(tampered.get(), {"catalog"});
    payload = util::replace_all(payload, "Other build", "Wrong build");
    json_object_set_new(tampered.get(), "catalog", json_string(payload.c_str()));
    assert(!parse_dev_catalog(json::dump(tampered.get()), server, bundle, "coffeeflix").catalog_valid);
    std::string bad;
    assert(util::read_file(folder + "/bad-schema.json", bad));
    assert(!parse_dev_catalog(bad, server, bundle, "coffeeflix").catalog_valid);
    assert(util::read_file(folder + "/duplicate-id.json", bad));
    assert(!parse_dev_catalog(bad, server, bundle, "coffeeflix").catalog_valid);
    assert(util::read_file(folder + "/bad-binary-signature.json", bad));
    assert(!parse_dev_catalog(bad, server, bundle, "coffeeflix").catalog_valid);
    auto legacy = http::get("http://" + server + "/dev.json");
    auto legacy_doc = json::Doc::parse(legacy.body);
    assert(json::str(legacy_doc.get(), {"sha256"}) == current.rel.sha256);
    auto legacy_binary = http::get("http://" + server + "/coffeeflix.wuhb");
    assert(legacy_binary.ok() && (int64_t)legacy_binary.body.size() == current.rel.size);

    on_checked(other, false);
    g_supported = false;
    assert(select_build("otherbuild"));
    assert(selected_build() == "otherbuild");
    assert(!select_build("not-in-catalog"));
    g_state = DOWNLOADING;
    assert(!select_build("coffeeflix"));
    g_state = READY;
    assert(!select_build("coffeeflix"));
    g_state = AVAILABLE;
    g_release = std::string(APP_BUILD_ID) == "coffeeflix" ? other.rel : current.rel;
    g_supported = true;
    g_started = util::now_seconds();
    g_declined = false;
    set_automatic(true);
    tick();
    assert(g_state == AVAILABLE && !g_job);  // automatic updates cannot switch builds
    set_automatic(false);

    // A response for a previous choice cannot win after switching again.
    start_check(false);
    assert(select_build("coffeeflix"));
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (g_state == CHECKING && std::chrono::steady_clock::now() < until) {
        tasks::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(g_state == UP_TO_DATE && g_release.build_id == "coffeeflix");

    Job job;
    std::string target = bundle + ".download";
    assert(fetch(other.rel, target, job).empty());
    assert(verify(target, other.rel, job).empty());
    { std::fstream f(target, std::ios::in | std::ios::out | std::ios::binary); f.seekp(128); f.put('X'); }
    assert(!verify(target, other.rel, job).empty());
    assert(fetch(other.rel, target, job).empty());
    g_release = other.rel;
    remember_ready(&g_release);
    assert(store::get_str("update_ready_build") == "otherbuild");
    g_state = READY;
    finish_on_exit();  // swaps only the temporary fake bundles in this test folder
    FileHash installed;
    assert(hash_file(bundle, nullptr, installed) && installed.sha256 == other.rel.sha256);
    assert(is_bundle(bundle + ".bak"));
    swap_back();
    FileHash restored;
    assert(hash_file(bundle, nullptr, restored) && restored.sha256 == current.rel.sha256);
    // A staged image from a different remembered choice must not survive startup.
    setenv("COFFEEFLIX_BUNDLE", bundle.c_str(), 1);
    assert(fetch(other.rel, target, job).empty());
    remember_ready(&other.rel);
    store::set_str("update_dev_build", "coffeeflix");
    g_state = IDLE;
    init();
    assert(!util::file_exists(target) && store::get_str("update_ready").empty());
    stop();
    tasks::shutdown();
    http::shutdown();
    std::cout << "Signed catalog, selection, download, tampering, install and rollback tests passed\n";
}
