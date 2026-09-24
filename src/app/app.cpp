#include "app/app.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <vector>

#include "audio/mixer.hpp"
#include "core/http.hpp"
#include "core/input.hpp"
#include "core/store.hpp"
#include "core/tasks.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/gfx.hpp"
#include "gfx/images.hpp"
#include "gfx/text.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"
#include "screens/screens.hpp"
#include "screens/widgets.hpp"
#include "ui/ui.hpp"
#include "app/mini_player.hpp"
#include "player/player.hpp"

namespace app {

namespace {

constexpr float RAIL_W = 84.0f;
constexpr float RAIL_OPEN_W = 272.0f;

std::vector<std::unique_ptr<Screen>> g_stack;
std::vector<std::unique_ptr<Screen>> g_dead;  // destroyed at end of frame
Section g_section = SEC_HOME;
double g_trans_start = -10;
int g_trans_dir = 1;
bool g_quit = false;
bool g_focus_rail_request = false;
bool g_mini_visible = false;
ui::Id g_rail_group = 0;

struct RailEntry { Section s; int icon; const char* label; };
const RailEntry RAIL[] = {
    {SEC_HOME, ic::HOME, "Home"},
    {SEC_SEARCH, ic::SEARCH, "Search"},
    {SEC_YOUTUBE, ic::SMART_DISPLAY, "YouTube"},
    {SEC_JELLYFIN, ic::VIDEO_LIBRARY, "Jellyfin"},
    {SEC_TWITCH, ic::LIVE_TV, "Twitch"},
    {SEC_RADIO, ic::RADIO, "Radio"},
    {SEC_PODCASTS, ic::PODCASTS, "Podcasts"},
    {SEC_MEDIA, ic::FOLDER, "My Media"},
    {SEC_SETTINGS, ic::SETTINGS, "Settings"},
};

ui::Id rail_id(Section s) { return ui::id(g_rail_group, (int64_t)s); }

void start_transition(int dir) {
    g_trans_start = ui::time();
    g_trans_dir = dir;
}

void draw_rail() {
    using namespace ui;
    const Theme& t = theme();
    bool rail_has_focus = focus_in_group(g_rail_group);
    float open = spring(id("rail_open"), rail_has_focus ? 1.0f : 0.0f, 260, 26);
    float w = RAIL_W + (RAIL_OPEN_W - RAIL_W) * open;

    // Scrim over the content while the rail is open.
    if (open > 0.01f) gfx::fill_rect_hgrad(Rect(0, 0, W, H), Color(5, 4, 8, (uint8_t)(200 * open)), Color(5, 4, 8, (uint8_t)(90 * open)));
    gfx::fill_rect_hgrad(Rect(0, 0, w + 60, H), Color(10, 8, 14, (uint8_t)(170 + 70 * open)), Color(10, 8, 14, 0));

    // Logo
    float lx = 42, ly = 58;
    gfx::fill_circle(lx, ly, 22, t.accent);
    gfx::fill_rrect_hgrad(Rect(lx - 22, ly - 22, 44, 44), 22, t.accent, t.accent2);
    text::icon(ic::LOCAL_CAFE, 26, lx, ly, gfx::rgb(0x1A1016));
    if (open > 0.02f) {
        gfx::push_alpha(anim::smoothstep((open - 0.3f) / 0.7f));
        text::draw(text::font(text::BOLD, 24), lx + 36, ly - 15, "CoffeeFlix", t.text);
        gfx::pop_alpha();
    }

    set_group_entry(g_rail_group, rail_id(g_section));
    int n = (int)(sizeof(RAIL) / sizeof(RAIL[0]));
    float y = 132;
    for (int i = 0; i < n; i++) {
        const RailEntry& e = RAIL[i];
        if (e.s == SEC_SETTINGS) y = H - 84;
        Rect r(14, y, w - 28, 50);
        Item it = focusable(rail_id(e.s), r, g_rail_group, F_NO_MEMORY | F_SIDE_ENTRY);
        if (it.focused) g_focus_rail_request = false;
        if (it.clicked) {
            open_section(e.s);
        }
        bool active = e.s == g_section;
        float f = it.f;
        Rect br = r.offset(bump_x(rail_id(e.s)), bump_y(rail_id(e.s)));
        if (f > 0.01f) gfx::fill_rrect(br, 25, t.surface_focus.alpha(f));
        if (active && f < 0.99f) {
            float a = 1 - f;
            gfx::fill_rrect_hgrad(Rect(4, br.cy() - 14, 5, 28), 2.5f, t.accent.alpha(a), t.accent2.alpha(a));
            if (open > 0.01f) gfx::fill_rrect(br, 25, t.surface.alpha(open * a));
        }
        Color fg = active ? t.accent : t.text2;
        fg = gfx::lerp(fg, gfx::rgb(0x15121A), f);
        text::icon(e.icon, 28, 42 + bump_x(rail_id(e.s)), br.cy(), fg);
        if (open > 0.02f) {
            gfx::push_alpha(anim::smoothstep((open - 0.25f) / 0.75f));
            Color lc = gfx::lerp(active ? t.text : t.text2, gfx::rgb(0x15121A), f);
            text::draw(active ? font::body_bold : font::body, 76 + bump_x(rail_id(e.s)),
                       br.cy() - text::line_height(font::body) * 0.5f, e.label, lc);
            gfx::pop_alpha();
        }
        y += 58;
    }
}

void draw_status() {
    using namespace ui;
    const Theme& t = theme();
    std::string clock = util::clock_hhmm();
    float x = W - 40;
    text::draw(font::label, x, 30, clock, t.text, text::RIGHT);
    x -= text::measure(font::label, clock) + 18;
    bool net = platform::network_connected();
    text::icon(net ? ic::WIFI : ic::WIFI_OFF, 22, x - 8, 30 + text::line_height(font::label) * 0.5f, net ? t.text2 : t.bad);
}

void handle_back() {
    Input& in = ui::input();
    if (!in.pressed_(BTN_B)) return;
    in.eat(BTN_B);
    Screen* s = top();
    if (s && s->on_back()) return;
    audio::play(audio::SFX_BACK);
    if (g_stack.size() > 1) {
        pop();
    } else if (!ui::focus_in_group(g_rail_group)) {
        focus_rail();
    } else if (g_section != SEC_HOME) {
        open_section(SEC_HOME);
        focus_rail();
    }
}

void frame(float dt) {
    using namespace ui;
    Screen* s = top();
    bool full = s && s->fullscreen();

    if (!s || !s->draws_background()) draw_background();
    bool prompting = screens::prompt_active();
    if (prompting) {
        ui::input().eat_all();
        ui::input().eat(BTN_B);
        suspend_nav();
    }

    // Screen content with a slide/fade transition.
    float tt = (float)(time() - g_trans_start) / 0.32f;
    float e = anim::ease_out_cubic(tt);
    bool transitioning = tt < 1.0f;
    if (transitioning) {
        gfx::push_alpha(std::min(1.0f, tt * 1.6f));
        gfx::push_transform(1.0f, 0, 0, (1 - e) * 48.0f * g_trans_dir, 0);
    }
    if (s) s->frame();
    if (transitioning) {
        gfx::pop_transform();
        gfx::pop_alpha();
    }

    if (!full) {
        mini_player::draw();
        draw_rail();
        if (!focus_in_group(g_rail_group)) draw_status();
    } else {
        mini_player::update_hidden();
    }
    if (g_focus_rail_request && !full) {
        set_focus(rail_id(g_section));
        g_focus_rail_request = false;
    }
    if (!prompting) handle_back();
    screens::draw_prompt();
    draw_overlays();
    (void)dt;
}

}  // namespace

float Screen::content_x() { return RAIL_W + 36.0f; }

void push(std::unique_ptr<Screen> s) {
    if (!s) return;
    g_stack.push_back(std::move(s));
    ui::reset_focus();
    start_transition(1);
    g_stack.back()->on_enter();
}

void pop() {
    if (g_stack.size() <= 1) return;
    g_dead.push_back(std::move(g_stack.back()));
    g_stack.pop_back();
    ui::reset_focus();
    start_transition(-1);
    g_stack.back()->on_enter();
}

void open_section(Section s) {
    for (auto& sc : g_stack) g_dead.push_back(std::move(sc));
    g_stack.clear();
    g_section = s;
    g_stack.push_back(screens::make_section_root(s));
    ui::reset_focus();
    start_transition(1);
    g_stack.back()->on_enter();
}

Screen* top() { return g_stack.empty() ? nullptr : g_stack.back().get(); }
bool rail_focused() { return ui::focus_in_group(g_rail_group); }
void focus_rail() { g_focus_rail_request = true; }
void quit() { g_quit = true; }
void set_mini_player_visible(bool v) { g_mini_visible = v; }

int run(int, char**) {
    if (!platform::init()) return 1;
    log_message(LOG_OK, "App", "CoffeeFlix starting on %s", platform::name());

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        log_message(LOG_ERROR, "App", "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
    SDL_Window* window = SDL_CreateWindow("CoffeeFlix", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                          platform::is_wiiu() ? 0 : SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, 0);
    if (!window || !renderer) {
        log_message(LOG_ERROR, "App", "Window/renderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, 1280, 720);
    SDL_RendererInfo info;
    if (SDL_GetRendererInfo(renderer, &info) == 0) log_message(LOG_OK, "App", "Renderer: %s", info.name);
    platform::post_video_init(window, renderer);

    std::string content = platform::content_dir();
    gfx::init(renderer);
    if (!text::init(content)) log_message(LOG_ERROR, "App", "Fonts missing in %s", content.c_str());
    store::load(platform::data_dir() + "/coffeeflix.json");
    http::init(content + "/cacert.pem");
    http::set_verify_tls(store::get_bool("verify_tls", true));
    tasks::init();
    images::init(platform::is_wiiu() ? (80u << 20) : (256u << 20));
    audio::init();
    player::init();
    audio::set_sfx_enabled(store::get_bool("ui_sounds", true));
    ui::init();
    ui::set_accent((int)store::get_int("accent", 0));
    g_rail_group = ui::id("rail");

    open_section(SEC_HOME);

    Input in;
    RawInput raw;
    Uint64 last = SDL_GetPerformanceCounter();
    while (platform::running() && !g_quit) {
        Uint64 now = SDL_GetPerformanceCounter();
        float dt = (float)((now - last) / (double)SDL_GetPerformanceFrequency());
        last = now;
        if (platform::fixed_dt() > 0) dt = platform::fixed_dt();

        platform::poll(raw);
        input_update(in, raw, dt);
        tasks::pump();
        images::begin_frame();
        player::update();

        gfx::begin_frame(gfx::BLACK);
        ui::begin_frame(in, dt);
        frame(dt);
        ui::end_frame();
        gfx::end_frame();

        if (const char* shot = platform::screenshot_request()) platform::save_screenshot(renderer, shot);
        SDL_RenderPresent(renderer);
        g_dead.clear();
        store::tick();
    }

    log_message(LOG_OK, "App", "Shutting down");
    mini_player::shutdown();
    g_dead.clear();
    g_stack.clear();
    player::shutdown();
    g_stack.clear();
    store::save_now();
    tasks::shutdown();
    audio::shutdown();
    images::shutdown();
    text::shutdown();
    gfx::shutdown();
    http::shutdown();
    IMG_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    platform::shutdown();
    return 0;
}

}  // namespace app
