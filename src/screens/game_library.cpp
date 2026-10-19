#include "screens/game_library.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <utility>

#include "core/json.hpp"
#include "core/util.hpp"
#include "gfx/text.hpp"
#include "platform/platform.hpp"
#include "ui/ui.hpp"

#ifdef __WIIU__
#include <coreinit/mcp.h>
#include <nn/acp/title.h>
#include <nn/cmpt/cmpt.h>
#include <sysapp/launch.h>
extern "C" int32_t CMPTAcctSetDrcCtrlEnabled(int32_t enabled);
#endif

namespace game_library {
namespace {

using Key = std::pair<uint64_t, bool>;
struct Play {
    Key key;
    int option = 1;
    int64_t timestamp = 0;
};

std::mutex gMutex;
bool gLoaded = false;
std::vector<Game> gGames;
std::vector<Play> gPlays;

std::string CachePath() { return platform::data_dir() + "/games-library.json"; }

std::string TitleId(uint64_t id) {
    char text[20];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(id));
    return text;
}

bool ParseId(const std::string& value, uint64_t& id) {
    if (value.size() != 16) return false;
    auto result = std::from_chars(value.data(), value.data() + value.size(), id, 16);
    return result.ec == std::errc() && result.ptr == value.data() + value.size();
}

std::string Field(const char* value, size_t limit) {
    size_t length = 0;
    while (length < limit && value[length]) ++length;
    std::string text(value, length);
    for (char& ch : text) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    return text;
}

Key GameKey(const Game& game) { return {game.titleId, game.usb}; }

void LoadLocked() {
    if (gLoaded) return;
    gLoaded = true;
    const std::string path = CachePath();
    for (const auto& candidate : {path, path + ".tmp"}) {
        struct stat st{};
        if (stat(candidate.c_str(), &st) != 0 || st.st_size < 0 || st.st_size > 2 * 1024 * 1024) continue;
        std::string data;
        if (!util::read_file(candidate, data)) continue;
        json::Doc doc = json::Doc::parse(data);
        if (!doc || json::num(doc.get(), {"version"}) != 1) continue;
        json_t* games = json::at(doc.get(), {"games"});
        for (size_t i = 0; i < std::min<size_t>(json::size(games), 2048); ++i) {
            json_t* item = json_array_get(games, i);
            Game game;
            if (!ParseId(json::str(item, {"id"}), game.titleId)) continue;
            game.name = json::str(item, {"name"});
            game.path = json::str(item, {"path"});
            game.icon = json::str(item, {"icon"});
            if (game.name.size() > 512 || game.path.size() > 1024 || game.icon.size() > 1024) continue;
            game.usb = json::boolean(item, {"usb"});
            game.wii = json::boolean(item, {"wii"});
            game.gamepad = json::boolean(item, {"gamepad"});
            gGames.push_back(std::move(game));
        }
        json_t* plays = json::at(doc.get(), {"recent"});
        for (size_t i = 0; i < std::min<size_t>(json::size(plays), 24); ++i) {
            json_t* item = json_array_get(plays, i);
            Play play;
            if (!ParseId(json::str(item, {"id"}), play.key.first)) continue;
            play.key.second = json::boolean(item, {"usb"});
            play.option = static_cast<int>(json::num(item, {"option"}, 1));
            if (play.option < 0 || play.option > 3) play.option = 1;
            play.timestamp = json::num(item, {"time"});
            gPlays.push_back(play);
        }
        break;
    }
}

void SaveLocked() {
    json_t* root = json_object();
    json_object_set_new(root, "version", json_integer(1));
    json_t* games = json_array();
    for (const Game& game : gGames) {
        json_t* entry = json_object();
        json_object_set_new(entry, "id", json_string(TitleId(game.titleId).c_str()));
        json_object_set_new(entry, "name", json_string(game.name.c_str()));
        json_object_set_new(entry, "path", json_string(game.path.c_str()));
        json_object_set_new(entry, "icon", json_string(game.icon.c_str()));
        json_object_set_new(entry, "usb", json_boolean(game.usb));
        json_object_set_new(entry, "wii", json_boolean(game.wii));
        json_object_set_new(entry, "gamepad", json_boolean(game.gamepad));
        json_array_append_new(games, entry);
    }
    json_object_set_new(root, "games", games);
    json_t* recent = json_array();
    for (const Play& play : gPlays) {
        json_t* entry = json_object();
        json_object_set_new(entry, "id", json_string(TitleId(play.key.first).c_str()));
        json_object_set_new(entry, "usb", json_boolean(play.key.second));
        json_object_set_new(entry, "option", json_integer(play.option));
        json_object_set_new(entry, "time", json_integer(play.timestamp));
        json_array_append_new(recent, entry);
    }
    json_object_set_new(root, "recent", recent);
    char* data = json_dumps(root, JSON_COMPACT);
    if (data) {
        util::write_file_atomic(CachePath(), data);
        free(data);
    }
    json_decref(root);
}

void Record(const Game& game, int option) {
    std::lock_guard<std::mutex> lock(gMutex);
    LoadLocked();
    Key key = GameKey(game);
    gPlays.erase(std::remove_if(gPlays.begin(), gPlays.end(), [key](const Play& p) {
        return p.key == key;
    }), gPlays.end());
    gPlays.insert(gPlays.begin(), Play{key, option, static_cast<int64_t>(time(nullptr))});
    if (gPlays.size() > 24) gPlays.resize(24);
    SaveLocked();
}

}

std::vector<Game> cached() {
    std::lock_guard<std::mutex> lock(gMutex);
    LoadLocked();
    return gGames;
}

std::vector<Recent> recent(size_t max) {
    std::lock_guard<std::mutex> lock(gMutex);
    LoadLocked();
    std::vector<Recent> out;
    for (const Play& play : gPlays) {
        auto it = std::find_if(gGames.begin(), gGames.end(), [&](const Game& game) {
            return GameKey(game) == play.key;
        });
        if (it == gGames.end()) continue;
        out.push_back(Recent{*it, play.option});
        if (out.size() == max) break;
    }
    return out;
}

Result scan(const std::shared_ptr<Progress>& progress, bool full) {
    Result result;
#ifdef __WIIU__
    const std::vector<Game> old = cached();
    std::map<Key, Game> existing;
    for (const Game& game : old) existing.emplace(GameKey(game), game);
    const int handle = MCP_Open();
    if (handle < 0) { result.failed = true; result.games = old; return result; }
    const MCPError count = MCP_TitleCount(handle);
    const uint32_t capacity = std::min<uint32_t>(2048, count > 0 ? static_cast<uint32_t>(count) + 32 : 512);
    bool scanned[2] = {};
    std::set<Key> seen;
    for (int disk = 0; disk < 2; ++disk) {
        progress->phase.store(disk + 1);
        std::vector<MCPTitleListType> list(capacity);
        uint32_t returned = 0;
        const MCPDeviceType device = disk ? MCP_DEVICE_TYPE_USB : MCP_DEVICE_TYPE_MLC;
        if (MCP_TitleListByDeviceType(handle, device, &returned, list.data(),
                static_cast<uint32_t>(list.size() * sizeof(MCPTitleListType))) != 0) continue;
        if (returned > capacity) continue;
        scanned[disk] = true;
        for (uint32_t i = 0; i < returned; ++i) {
            progress->checked.fetch_add(1);
            const auto& item = list[i];
            const bool wii = item.appType == MCP_APP_TYPE_GAME_WII || (item.titleId >> 32) == 0x00050002;
            if (!wii && item.appType != MCP_APP_TYPE_GAME) continue;
            const Key key{item.titleId, disk != 0};
            if (!seen.insert(key).second) continue;
            const std::string path = Field(item.path, sizeof(item.path));
            auto previous = existing.find(key);
            if (!full && previous != existing.end() && previous->second.path == path && previous->second.wii == wii) {
                result.games.push_back(previous->second);
                continue;
            }
            Game game;
            game.titleId = item.titleId;
            game.name = TitleId(item.titleId);
            game.path = path;
            game.usb = disk != 0;
            game.wii = wii;
            alignas(0x40) ACPMetaXml meta{};
            if (ACPGetTitleMetaXml(item.titleId, &meta) == 0) {
                game.gamepad = meta.drc_use == 65537;
                std::string name = Field(meta.longname_en, sizeof(meta.longname_en));
                if (name.empty()) name = Field(meta.shortname_en, sizeof(meta.shortname_en));
                if (!name.empty()) game.name = std::move(name);
            }
            char directory[1024] = {};
            if (ACPGetTitleMetaDirByDevice(item.titleId, directory, sizeof(directory),
                    disk ? ACP_DEVICE_TYPE_USB : ACP_DEVICE_TYPE_MLC) == 0 && directory[0]) {
                game.icon = std::string(directory) + "/iconTex.tga";
            } else if (!game.path.empty()) {
                game.icon = game.path + "/meta/iconTex.tga";
            }
            result.games.push_back(std::move(game));
        }
    }
    MCP_Close(handle);
    for (const Game& game : old) if (!scanned[game.usb]) result.games.push_back(game);
    if (!scanned[0] && !scanned[1]) { result.failed = true; result.games = old; return result; }
    std::sort(result.games.begin(), result.games.end(), [](const Game& a, const Game& b) {
        if (a.name == b.name) return a.usb < b.usb;
        return a.name < b.name;
    });
    {
        std::lock_guard<std::mutex> lock(gMutex);
        LoadLocked();
        const bool changed = gGames.size() != result.games.size() ||
            !std::equal(gGames.begin(), gGames.end(), result.games.begin(), [](const Game& a, const Game& b) {
                return a.titleId == b.titleId && a.usb == b.usb && a.wii == b.wii &&
                    a.gamepad == b.gamepad && a.name == b.name && a.path == b.path && a.icon == b.icon;
            });
        if (changed) {
            gGames = result.games;
            SaveLocked();
        }
    }
#else
    (void)progress;
    (void)full;
    result.games = cached();
#endif
    return result;
}

void launch(const Game& game, int option) {
#ifdef __WIIU__
    if (game.wii) {
        const bool useGamePad = option == 0 && game.gamepad;
        const CmptScreenType screen = useGamePad ? CMPT_SCREEN_TYPE_BOTH :
            option == 1 ? CMPT_SCREEN_TYPE_TV :
            option == 2 ? CMPT_SCREEN_TYPE_BOTH : CMPT_SCREEN_TYPE_DRC;
        if (CMPTAcctSetDrcCtrlEnabled(useGamePad ? 1 : 0) < 0 || CMPTAcctSetScreenType(screen) < 0) {
            ui::toast("Could not configure Wii VC controller mode", ic::ERROR_OUTLINE);
            return;
        }
        if (CMPTCheckScreenState() < 0 && screen != CMPT_SCREEN_TYPE_DRC) CMPTAcctSetScreenType(CMPT_SCREEN_TYPE_DRC);
        if (!game.path.empty()) {
            Record(game, option);
            _SYSLaunchTitleByPathFromLauncher(game.path.c_str(), static_cast<uint32_t>(game.path.size()));
            return;
        }
    }
    Record(game, option);
    SYSLaunchTitle(game.titleId);
#else
    (void)game;
    (void)option;
#endif
}

}
