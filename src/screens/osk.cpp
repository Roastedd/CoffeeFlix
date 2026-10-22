#include "screens/osk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "app/pad_views.hpp"
#include "audio/mixer.hpp"
#include "core/i18n.hpp"
#include "core/util.hpp"
#include "gfx/anim.hpp"
#include "gfx/gfx.hpp"
#include "gfx/text.hpp"
#include "platform/platform.hpp"
#include "ui/ui.hpp"

namespace screens::osk {

namespace {

using ui::Rect;

enum Page { LOWER, UPPER, SYMBOLS };
constexpr const char* ROWS[3][4] = {
    {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm,.?"},
    {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM;:!"},
    {"!@#$%^&*()", "-_=+[]{};:", "\"'/\\|<>~`,", ".?!@#&*:;/"},
};
constexpr int MAX_LENGTH = 120;

enum Kind { CHAR, SHIFT, SYMBOLS_KEY, SPACE, DELETE_KEY, CANCEL, DONE };

struct Key {
    Kind kind = CHAR;
    std::string text;  // the character, for CHAR
    Rect r;
};

struct State {
    bool active = false;
    std::string title, text;
    bool password = false;
    std::function<void(std::string)> done;
    int page = LOWER;
    int row = 1, col = 0;  // the cursor: a row of `keys`, a key in it
    std::vector<std::vector<Key>> keys;
    double opened = 0;
    float flash = 0;  // a key just pressed
    int flash_row = -1, flash_col = -1;
} S;

// Lays the keys out in 1000 x 560 card at (140, 80): ten columns of 82 and gaps of 8, then the row of controls.
void layout() {
    S.keys.assign(5, {});
    const float x0 = 140 + 40, y0 = 80 + 150, kw = 82, kh = 62, gap = 8;
    for (int r = 0; r < 4; r++) {
        const std::string row = ROWS[S.page][r];
        for (size_t c = 0; c < row.size(); c++) {
            Key k;
            k.text = std::string(1, row[c]);
            k.r = Rect(x0 + c * (kw + gap), y0 + r * (kh + gap), kw, kh);
            S.keys[r].push_back(k);
        }
    }
    const float y = y0 + 4 * (kh + gap), unit = kw + gap;
    struct Spec { Kind kind; float units; };
    const Spec controls[] = {{SHIFT, 1.5f}, {SYMBOLS_KEY, 1.5f}, {SPACE, 3.0f}, {DELETE_KEY, 1.5f}, {CANCEL, 1.0f}, {DONE, 1.5f}};
    float x = x0;
    for (const Spec& c : controls) {
        Key k;
        k.kind = c.kind;
        k.r = Rect(x, y, c.units * unit - gap, kh);
        S.keys[4].push_back(k);
        x += c.units * unit;
    }
    S.row = std::clamp(S.row, 0, 4);
    S.col = std::clamp(S.col, 0, (int)S.keys[S.row].size() - 1);
}

void backspace() {
    while (!S.text.empty() && ((unsigned char)S.text.back() & 0xC0) == 0x80) S.text.pop_back();  // a character is more than one byte
    if (!S.text.empty()) S.text.pop_back();
}

void finish(bool ok) {
    S.active = false;
    auto done = std::move(S.done);
    S.done = nullptr;
    audio::play(ok ? audio::SFX_SELECT : audio::SFX_BACK);
    if (ok && done) done(util::trim(S.text));
}

void press_key(int row, int col) {
    const Key& k = S.keys[row][col];
    S.flash = 1;
    S.flash_row = row;
    S.flash_col = col;
    switch (k.kind) {
        case CHAR:
            if ((int)S.text.size() < MAX_LENGTH) S.text += k.text;
            if (S.page == UPPER) S.page = LOWER;  // one capital, then back
            layout();
            break;
        case SHIFT:
            S.page = S.page == LOWER ? UPPER : LOWER;
            layout();
            break;
        case SYMBOLS_KEY:
            S.page = S.page == SYMBOLS ? LOWER : SYMBOLS;
            layout();
            break;
        case SPACE:
            if ((int)S.text.size() < MAX_LENGTH) S.text += ' ';
            break;
        case DELETE_KEY: backspace(); break;
        case CANCEL: finish(false); return;
        case DONE: finish(true); return;
    }
    audio::play(audio::SFX_TOGGLE, 0.5f);
}

// The key in `row` nearest across to where the cursor is now.
int nearest(int row, float cx) {
    int best = 0;
    float bd = 1e9f;
    for (size_t i = 0; i < S.keys[row].size(); i++) {
        const float d = std::fabs(S.keys[row][i].r.cx() - cx);
        if (d < bd) {
            bd = d;
            best = (int)i;
        }
    }
    return best;
}

bool hit(float x, float y, int& row, int& col) {
    for (int r = 0; r < (int)S.keys.size(); r++)
        for (int c = 0; c < (int)S.keys[r].size(); c++)
            if (S.keys[r][c].r.inset(-4, -4).contains(x, y)) {
                row = r;
                col = c;
                return true;
            }
    return false;
}

const char* label_of(const Key& k) {
    switch (k.kind) {
        case SHIFT: return S.page == UPPER ? "abc" : "ABC";
        case SYMBOLS_KEY: return S.page == SYMBOLS ? "abc" : "#+=";
        case SPACE: return tr("space");
        case CANCEL: return tr("Cancel");
        case DONE: return tr("OK");
        default: return "";
    }
}

}  // namespace

bool wanted() {
    if (getenv("COFFEEFLIX_OSK")) return true;
    if (!platform::is_wiiu()) return false;
    // The system keyboard is drawn on the GamePad: not for a dark GamePad, or for someone using another controller.
    return pad_views::pad_screen_off() || !platform::last_input_was_pad();
}

void open(const std::string& title, const std::string& initial, bool password, std::function<void(std::string)> done) {
    S = State();
    S.active = true;
    S.title = title;
    S.text = initial;
    S.password = password;
    S.done = std::move(done);
    S.opened = ui::time();
    layout();
    audio::play(audio::SFX_OPEN, 0.6f);
}

bool active() { return S.active; }

void input(Input& in) {
    if (!S.active) return;
    if (in.pressed_(BTN_B)) {
        finish(false);
        return;
    }
    if (in.pressed_(BTN_PLUS)) {
        finish(true);
        return;
    }
    if (in.pressed_(BTN_Y)) { backspace(); S.flash = 1; }
    if (in.pressed_(BTN_X) && (int)S.text.size() < MAX_LENGTH) { S.text += ' '; S.flash = 1; }
    if (in.pressed_(BTN_L) || in.pressed_(BTN_R)) {
        S.page = S.page == LOWER ? UPPER : LOWER;
        layout();
    }
    const float cx = S.keys[S.row][S.col].r.cx();
    int dr = 0, dc = 0;
    if (in.rep(BTN_UP)) dr = -1;
    if (in.rep(BTN_DOWN)) dr = 1;
    if (in.rep(BTN_LEFT)) dc = -1;
    if (in.rep(BTN_RIGHT)) dc = 1;
    if (dr || dc) {
        if (dr) {
            S.row = (S.row + dr + (int)S.keys.size()) % (int)S.keys.size();
            S.col = nearest(S.row, cx);
        } else {
            const int n = (int)S.keys[S.row].size();
            S.col = (S.col + dc + n) % n;
        }
        audio::play(audio::SFX_MOVE, 0.4f);
    }
    int r, c;
    if (in.tap && hit(in.tx, in.ty, r, c)) {
        S.row = r;
        S.col = c;
        press_key(r, c);
    } else if (in.pressed_(BTN_A)) {
        if (in.pointer && hit(in.px, in.py, r, c)) {
            S.row = r;
            S.col = c;
        }
        press_key(S.row, S.col);
    } else if (in.pointer_moved && hit(in.px, in.py, r, c)) {
        S.row = r;
        S.col = c;
    }
    in.eat_all();
    in.eat(BTN_B);
}

void draw() {
    if (!S.active) return;
    const ui::Theme& t = ui::theme();
    const float dt = ui::dt();
    S.flash = std::max(0.0f, S.flash - dt * 6);
    const float a = anim::ease_out_cubic((float)(ui::time() - S.opened) / 0.2f);
    gfx::fill_rect(Rect(0, 0, ui::W, ui::H), gfx::Color(4, 3, 6, (uint8_t)(215 * a)));
    gfx::push_alpha(a);
    const Rect card(140, 80 + (1 - a) * 24, 1000, 560);
    gfx::shadow(card, 34, gfx::Color(0, 0, 0, 180));
    gfx::fill_rrect(card, 26, gfx::Color(26, 22, 32, 250));
    text::draw(ui::font::title, card.x + 40, card.y + 24, S.title, t.text);
    const Rect field(card.x + 40, card.y + 70, card.w - 80, 56);
    gfx::fill_rrect(field, 16, gfx::Color(255, 255, 255, 22));
    gfx::stroke_rrect(field, 16, 2, t.accent);
    std::string v = S.text;
    if (S.password) v = std::string(util::utf8_length(v), '*');
    const float tw = text::measure(ui::font::label, v);
    const float maxw = field.w - 40;
    gfx::push_clip(field.inset(12, 4));
    const float tx = field.x + 20 - std::max(0.0f, tw - maxw);
    text::draw(ui::font::label, tx, field.cy() - 13, v, t.text);
    if (std::fmod((float)ui::time(), 1.0f) < 0.55f) gfx::fill_rect(Rect(tx + tw + 3, field.cy() - 14, 2.5f, 28), t.accent);
    gfx::pop_clip();

    // keys are laid out against the card's resting place; the slide-in moves them with it
    const float dy = card.y - 80;
    for (int r = 0; r < (int)S.keys.size(); r++) {
        for (int c = 0; c < (int)S.keys[r].size(); c++) {
            const Key& k = S.keys[r][c];
            const Rect kr = k.r.offset(0, dy);
            const bool on = r == S.row && c == S.col;
            const bool flash = S.flash > 0 && r == S.flash_row && c == S.flash_col;
            if (on) gfx::fill_rrect_hgrad(kr, 14, t.accent, t.accent2);
            else if (k.kind == DONE) gfx::fill_rrect(kr, 14, t.accent.alpha(0.35f));
            else gfx::fill_rrect(kr, 14, gfx::Color(255, 255, 255, k.kind == CHAR ? 26 : 38));
            if (flash) gfx::fill_rrect(kr, 14, gfx::Color(255, 255, 255, (uint8_t)(70 * S.flash)));
            const gfx::Color fg = on ? gfx::rgb(0x15121A) : t.text;
            if (k.kind == CHAR) text::draw(ui::font::title, kr.cx(), kr.cy() - text::line_height(ui::font::title) * 0.5f, k.text, fg, text::CENTER);
            else if (k.kind == DELETE_KEY) text::icon(ic::BACKSPACE, 30, kr.cx(), kr.cy(), fg);
            else text::draw(ui::font::body_bold, kr.cx(), kr.cy() - text::line_height(ui::font::body_bold) * 0.5f, label_of(k), fg, text::CENTER);
        }
    }
    ui::hint_bar({{"A", tr("Type")}, {"Y", tr("Delete")}, {"X", tr("Space")}, {"+", tr("Done")}, {"B", tr("Cancel")}}, card.b() - 38);
    gfx::pop_alpha();
}

std::string page_chars(int page) {
    std::string out;
    for (int r = 0; r < 4; r++) out += ROWS[page][r];
    return out;
}

std::string press(std::string text, const std::string& key) {
    if (key == BACKSPACE) {
        while (!text.empty() && ((unsigned char)text.back() & 0xC0) == 0x80) text.pop_back();
        if (!text.empty()) text.pop_back();
        return text;
    }
    return text + key;
}

}  // namespace screens::osk
