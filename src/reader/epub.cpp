#include "reader/epub.hpp"

#include <SDL2/SDL_ttf.h>
#include <tinyxml2.h>
#include <zip.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/util.hpp"
#include "gfx/images.hpp"
#include "logger/logger.hpp"
#include "platform/platform.hpp"

namespace epub {

namespace {

// Pages are laid out on this reference sheet and scaled when rendered.
constexpr float PAGE_W = 600, PAGE_H = 800;
constexpr float MARGIN_X = 50, MARGIN_TOP = 56, MARGIN_BOTTOM = 56;
constexpr float CONTENT_W = PAGE_W - 2 * MARGIN_X, CONTENT_H = PAGE_H - MARGIN_TOP - MARGIN_BOTTOM;

enum Style { BODY, H1, H2, H3, SMALL, STYLE_COUNT };

struct StyleSpec {
    bool bold;
    float size;         // px on the reference page
    float line;         // line height, x size
    float before, after;  // paragraph spacing, x size
};

const StyleSpec STYLES[STYLE_COUNT] = {
    {false, 25, 1.45f, 0.0f, 0.55f},  // BODY
    {true, 38, 1.2f, 0.6f, 0.6f},     // H1
    {true, 32, 1.2f, 0.6f, 0.5f},     // H2
    {true, 28, 1.25f, 0.5f, 0.4f},    // H3..H6
    {false, 21, 1.4f, 0.0f, 0.5f},    // SMALL (captions)
};

const SDL_Color PAPER = {250, 247, 240, 255};
const SDL_Color INK = {33, 30, 36, 255};

// --- fonts ------------------------------------------------------------------------------

std::mutex g_font_mutex;
std::vector<uint8_t> g_font_data[2];  // regular, bold

const std::vector<uint8_t>& font_data(bool bold) {
    std::lock_guard<std::mutex> lock(g_font_mutex);
    std::vector<uint8_t>& d = g_font_data[bold ? 1 : 0];
    if (d.empty()) {
        std::string path = platform::content_dir() + (bold ? "/fonts/Inter-SemiBold.otf" : "/fonts/Inter-Regular.otf");
        std::string bytes;
        if (util::read_file(path, bytes)) d.assign(bytes.begin(), bytes.end());
        else log_message(LOG_ERROR, "EPUB", "Can't read %s", path.c_str());
    }
    return d;
}

// Fonts opened at the sizes a book needs; owned by one book and used by one thread at a time.
class Fonts {
public:
    ~Fonts() {
        for (auto& [key, f] : fonts_) TTF_CloseFont(f);
    }
    TTF_Font* get(bool bold, int px) {
        int key = (bold ? 1 << 16 : 0) | px;
        auto it = fonts_.find(key);
        if (it != fonts_.end()) return it->second;
        const std::vector<uint8_t>& data = font_data(bold);
        TTF_Font* f = nullptr;
        if (!data.empty()) f = TTF_OpenFontRW(SDL_RWFromConstMem(data.data(), (int)data.size()), 1, px);
        fonts_[key] = f;
        return f;
    }

private:
    std::map<int, TTF_Font*> fonts_;
};

// --- zip ----------------------------------------------------------------------------------------

bool read_entry(zip_t* z, const std::string& name, std::string& out) {
    zip_int64_t i = zip_name_locate(z, name.c_str(), ZIP_FL_ENC_GUESS);
    if (i < 0) i = zip_name_locate(z, name.c_str(), ZIP_FL_ENC_GUESS | ZIP_FL_NOCASE);
    if (i < 0) return false;
    zip_stat_t st;
    if (zip_stat_index(z, (zip_uint64_t)i, 0, &st) != 0 || !(st.valid & ZIP_STAT_SIZE) || st.size > (48u << 20)) return false;
    zip_file_t* f = zip_fopen_index(z, (zip_uint64_t)i, 0);
    if (!f) return false;
    out.assign(st.size, '\0');
    zip_int64_t got = zip_fread(f, out.data(), st.size);
    zip_fclose(f);
    return got == (zip_int64_t)st.size;
}

// "OEBPS/Text/../Images/a%20b.jpg#x" relative to "OEBPS/Text" -> "OEBPS/Images/a b.jpg"
std::string resolve(const std::string& base_dir, std::string href) {
    href = href.substr(0, href.find('#'));
    href = util::url_decode(href);
    if (href.empty()) return "";
    std::string joined = href[0] == '/' ? href.substr(1) : (base_dir.empty() ? href : base_dir + "/" + href);
    std::vector<std::string> parts;
    for (const std::string& p : util::split(joined, '/')) {
        if (p.empty() || p == ".") continue;
        if (p == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(p);
        }
    }
    std::string out;
    for (const std::string& p : parts) out += (out.empty() ? "" : "/") + p;
    return out;
}

std::string dir_of(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
}

// --- XML ----------------------------------------------------------------------------------------

// Element name without its namespace prefix, lower case ("dc:title" -> "title").
std::string local_name(const tinyxml2::XMLElement* e) {
    const char* n = e->Name();
    const char* colon = std::strrchr(n, ':');
    return util::lower(colon ? colon + 1 : n);
}

const tinyxml2::XMLElement* find_element(const tinyxml2::XMLElement* e, const char* name) {
    for (; e; e = e->NextSiblingElement()) {
        if (local_name(e) == name) return e;
        if (const tinyxml2::XMLElement* r = find_element(e->FirstChildElement(), name)) return r;
    }
    return nullptr;
}

const char* attr(const tinyxml2::XMLElement* e, const char* name) {
    if (const char* v = e->Attribute(name)) return v;
    // Namespaced variants such as xlink:href or opf:role.
    for (const tinyxml2::XMLAttribute* a = e->FirstAttribute(); a; a = a->Next()) {
        const char* colon = std::strchr(a->Name(), ':');
        if (colon && std::strcmp(colon + 1, name) == 0) return a->Value();
    }
    return nullptr;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// HTML's named entities aren't XML: turn the common ones into characters before parsing (and
// escape unknown ones) so books written as loose XHTML still parse.
std::string xmlify_entities(const std::string& in) {
    static const std::unordered_map<std::string, uint32_t> NAMED = [] {
        std::unordered_map<std::string, uint32_t> m = {
            {"nbsp", 160}, {"iexcl", 161}, {"cent", 162}, {"pound", 163}, {"curren", 164}, {"yen", 165},
            {"brvbar", 166}, {"sect", 167}, {"uml", 168}, {"copy", 169}, {"ordf", 170}, {"laquo", 171},
            {"not", 172}, {"shy", 173}, {"reg", 174}, {"macr", 175}, {"deg", 176}, {"plusmn", 177},
            {"sup2", 178}, {"sup3", 179}, {"acute", 180}, {"micro", 181}, {"para", 182}, {"middot", 183},
            {"cedil", 184}, {"sup1", 185}, {"ordm", 186}, {"raquo", 187}, {"frac14", 188}, {"frac12", 189},
            {"frac34", 190}, {"iquest", 191}, {"OElig", 338}, {"oelig", 339}, {"Scaron", 352},
            {"scaron", 353}, {"Yuml", 376}, {"fnof", 402}, {"circ", 710}, {"tilde", 732}, {"ensp", 8194},
            {"emsp", 8195}, {"thinsp", 8201}, {"zwnj", 8204}, {"zwj", 8205}, {"lrm", 8206}, {"rlm", 8207},
            {"ndash", 8211}, {"mdash", 8212}, {"lsquo", 8216}, {"rsquo", 8217}, {"sbquo", 8218},
            {"ldquo", 8220}, {"rdquo", 8221}, {"bdquo", 8222}, {"dagger", 8224}, {"Dagger", 8225},
            {"bull", 8226}, {"hellip", 8230}, {"permil", 8240}, {"prime", 8242}, {"Prime", 8243},
            {"lsaquo", 8249}, {"rsaquo", 8250}, {"oline", 8254}, {"frasl", 8260}, {"euro", 8364},
            {"trade", 8482}, {"larr", 8592}, {"uarr", 8593}, {"rarr", 8594}, {"darr", 8595}, {"harr", 8596},
            {"minus", 8722}, {"infin", 8734}, {"ne", 8800}, {"le", 8804}, {"ge", 8805}, {"loz", 9674},
            {"spades", 9824}, {"clubs", 9827}, {"hearts", 9829}, {"diams", 9830},
        };
        // Latin-1 letters 192..255 in code point order.
        static const char* const LATIN[] = {
            "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute",
            "Ecirc", "Euml", "Igrave", "Iacute", "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute",
            "Ocirc", "Otilde", "Ouml", "times", "Oslash", "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute",
            "THORN", "szlig", "agrave", "aacute", "acirc", "atilde", "auml", "aring", "aelig", "ccedil",
            "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml", "eth", "ntilde",
            "ograve", "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc",
            "uuml", "yacute", "thorn", "yuml"};
        for (int i = 0; i < 64; i++) m[LATIN[i]] = 192 + i;
        return m;
    }();
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] != '&') {
            out += in[i];
            continue;
        }
        size_t semi = in.find(';', i + 1);
        if (semi != std::string::npos && semi - i <= 10) {
            std::string name = in.substr(i + 1, semi - i - 1);
            if (!name.empty() && (name[0] == '#' || name == "amp" || name == "lt" || name == "gt" || name == "quot" ||
                                  name == "apos")) {
                out.append(in, i, semi - i + 1);
                i = semi;
                continue;
            }
            auto it = NAMED.find(name);
            if (it != NAMED.end()) {
                append_utf8(out, it->second);
                i = semi;
                continue;
            }
        }
        out += "&amp;";  // a stray '&' or an entity we don't know: keep it as text
    }
    return out;
}

// --- content -------------------------------------------------------------------------------

struct Block {
    enum Kind { TEXT, IMAGE, RULE, CHAPTER } kind = TEXT;
    Style style = BODY;
    std::string text;   // TEXT
    std::string image;  // IMAGE: zip entry
};

// Collects a chapter's blocks from its XHTML.
class Chapter {
public:
    Chapter(std::string dir, std::vector<Block>& out) : dir_(std::move(dir)), out_(out) {}

    void walk(const tinyxml2::XMLNode* n) {
        for (; n; n = n->NextSibling()) {
            if (const tinyxml2::XMLText* t = n->ToText()) {
                add_text(t->Value());
                continue;
            }
            const tinyxml2::XMLElement* e = n->ToElement();
            if (!e) continue;
            std::string name = local_name(e);
            if (name == "head" || name == "script" || name == "style" || name == "title") continue;
            if (name == "br") {
                flush();
                continue;
            }
            if (name == "hr") {
                flush();
                out_.push_back(Block{Block::RULE, BODY, "", ""});
                continue;
            }
            if (name == "img" || name == "image") {
                const char* src = attr(e, name == "img" ? "src" : "href");
                if (src) {
                    flush();
                    out_.push_back(Block{Block::IMAGE, BODY, "", resolve(dir_, src)});
                }
                continue;
            }
            Style style = style_;
            bool block = is_block(name);
            if (block) flush();
            if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6')
                style_ = name[1] == '1' ? H1 : name[1] == '2' ? H2 : H3;
            else if (name == "figcaption" || name == "caption" || name == "small")
                style_ = block ? SMALL : style_;
            if (name == "li") text_ = "\xE2\x80\xA2 ";  // bullet
            walk(e->FirstChild());
            if (block) flush();
            style_ = style;
        }
    }

    void flush() {
        while (!text_.empty() && text_.back() == ' ') text_.pop_back();
        if (!text_.empty() && text_ != "\xE2\x80\xA2") out_.push_back(Block{Block::TEXT, style_, text_, ""});
        text_.clear();
    }

private:
    static bool is_block(const std::string& n) {
        static const char* const BLOCKS[] = {"p", "div", "h1", "h2", "h3", "h4", "h5", "h6", "li", "ul", "ol",
                                             "blockquote", "section", "article", "header", "footer", "figure",
                                             "figcaption", "caption", "pre", "table", "tr", "dt", "dd", "dl",
                                             "aside", "body", "main", "address", "center", "nav", "td", "th"};
        for (const char* b : BLOCKS)
            if (n == b) return true;
        return false;
    }

    // Whitespace collapses to single spaces, as in HTML.
    void add_text(const char* s) {
        for (const char* p = s; *p; p++) {
            unsigned char c = (unsigned char)*p;
            bool space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
            // A no-break space (U+00A0) counts as a space too.
            if (c == 0xC2 && (unsigned char)p[1] == 0xA0) {
                space = true;
                p++;
            }
            if (space) {
                if (!text_.empty() && text_.back() != ' ') text_ += ' ';
            } else {
                text_ += (char)c;
            }
        }
    }

    std::string dir_;
    std::vector<Block>& out_;
    std::string text_;
    Style style_ = BODY;
};

// Picture size from the file header, without decoding it.
bool image_size(const std::string& d, int& w, int& h) {
    auto u8 = [&](size_t i) { return i < d.size() ? (unsigned char)d[i] : 0u; };
    auto be16 = [&](size_t i) { return (int)(u8(i) << 8 | u8(i + 1)); };
    auto le16 = [&](size_t i) { return (int)(u8(i) | u8(i + 1) << 8); };
    if (d.size() > 24 && std::memcmp(d.data(), "\x89PNG", 4) == 0) {
        w = (int)(u8(16) << 24 | u8(17) << 16 | u8(18) << 8 | u8(19));
        h = (int)(u8(20) << 24 | u8(21) << 16 | u8(22) << 8 | u8(23));
        return w > 0 && h > 0;
    }
    if (d.size() > 10 && std::memcmp(d.data(), "GIF8", 4) == 0) {
        w = le16(6);
        h = le16(8);
        return w > 0 && h > 0;
    }
    if (d.size() > 4 && u8(0) == 0xFF && u8(1) == 0xD8) {
        size_t i = 2;
        while (i + 9 < d.size()) {
            if (u8(i) != 0xFF) {
                i++;
                continue;
            }
            unsigned m = u8(i + 1);
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = be16(i + 5);
                w = be16(i + 7);
                return w > 0 && h > 0;
            }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
                i += 2;
                continue;
            }
            i += 2 + be16(i + 2);
        }
        return false;
    }
    if (d.size() > 30 && std::memcmp(d.data(), "RIFF", 4) == 0 && std::memcmp(d.data() + 8, "WEBP", 4) == 0) {
        if (std::memcmp(d.data() + 12, "VP8X", 4) == 0) {
            w = 1 + (int)(u8(24) | u8(25) << 8 | u8(26) << 16);
            h = 1 + (int)(u8(27) | u8(28) << 8 | u8(29) << 16);
        } else if (std::memcmp(d.data() + 12, "VP8 ", 4) == 0) {
            w = le16(26) & 0x3FFF;
            h = le16(28) & 0x3FFF;
        } else if (std::memcmp(d.data() + 12, "VP8L", 4) == 0) {
            uint32_t b = u8(21) | u8(22) << 8 | u8(23) << 16 | u8(24) << 24;
            w = 1 + (int)(b & 0x3FFF);
            h = 1 + (int)((b >> 14) & 0x3FFF);
        }
        return w > 0 && h > 0;
    }
    return false;
}

// --- layout ---------------------------------------------------------------------------------

struct Item {
    enum Kind { LINE, IMAGE, RULE } kind = LINE;
    Style style = BODY;
    std::string text;   // LINE
    std::string image;  // IMAGE
    float x = 0, y = 0, w = 0, h = 0;
};
using Page = std::vector<Item>;

// Splits one UTF-8 string into its code point boundaries.
std::vector<size_t> char_starts(const std::string& s) {
    std::vector<size_t> out;
    for (size_t i = 0; i < s.size(); i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) out.push_back(i);
    return out;
}

class Layout {
public:
    Layout(Fonts& fonts, zip_t* zip) : fonts_(fonts), zip_(zip) {}

    std::vector<Page> run(const std::vector<Block>& blocks) {
        for (const Block& b : blocks) {
            switch (b.kind) {
                case Block::CHAPTER:
                    new_page();
                    break;
                case Block::TEXT:
                    paragraph(b);
                    break;
                case Block::IMAGE:
                    picture(b);
                    break;
                case Block::RULE:
                    space(12);
                    if (y_ + 14 > CONTENT_H) new_page();
                    page().push_back(Item{Item::RULE, BODY, "", "", MARGIN_X + CONTENT_W * 0.35f, MARGIN_TOP + y_ + 6,
                                          CONTENT_W * 0.3f, 2});
                    y_ += 14;
                    gap_ = 12;
                    break;
            }
        }
        if (!pages_.empty() && pages_.back().empty()) pages_.pop_back();
        return std::move(pages_);
    }

private:
    Page& page() {
        if (pages_.empty()) pages_.emplace_back();
        return pages_.back();
    }

    void new_page() {
        if (!pages_.empty() && pages_.back().empty()) return;
        pages_.emplace_back();
        y_ = 0;
        gap_ = 0;
    }

    // Spacing between blocks never starts a page.
    void space(float px) {
        if (y_ > 0) y_ += std::max(px, gap_);
        gap_ = 0;
    }

    float width(Style s, const std::string& word) {
        auto& cache = widths_[s];
        auto it = cache.find(word);
        if (it != cache.end()) return it->second;
        int w = 0, h = 0;
        TTF_Font* f = fonts_.get(STYLES[s].bold, (int)STYLES[s].size);
        if (f) TTF_SizeUTF8(f, word.c_str(), &w, &h);
        cache[word] = (float)w;
        return (float)w;
    }

    void paragraph(const Block& b) {
        const StyleSpec& spec = STYLES[b.style];
        float line_h = spec.size * spec.line;
        space(spec.size * spec.before);
        // Keep a heading with at least a couple of lines of what follows it.
        if (b.style != BODY && b.style != SMALL && y_ + line_h * 3 > CONTENT_H) new_page();

        float space_w = width(b.style, " ");
        std::string line;
        float line_w = 0;
        auto emit = [&] {
            if (line.empty()) return;
            if (y_ + line_h > CONTENT_H) new_page();
            page().push_back(Item{Item::LINE, b.style, line, "", MARGIN_X, MARGIN_TOP + y_, line_w, line_h});
            y_ += line_h;
            line.clear();
            line_w = 0;
        };
        for (std::string word : util::split(b.text, ' ')) {
            if (word.empty()) continue;
            float w = width(b.style, word);
            // A word wider than the page is broken wherever it has to be.
            while (w > CONTENT_W) {
                emit();
                std::vector<size_t> cs = char_starts(word);
                if (cs.size() < 2) break;  // a single character that's wider than the page
                size_t cut = 1;
                while (cut < cs.size() && width(b.style, word.substr(0, cs[cut])) <= CONTENT_W) cut++;
                size_t bytes = cs[std::max<size_t>(1, cut - 1)];
                line = word.substr(0, bytes);
                line_w = width(b.style, line);
                emit();
                word = word.substr(bytes);
                w = width(b.style, word);
            }
            float needed = line.empty() ? w : line_w + space_w + w;
            if (needed > CONTENT_W) {
                emit();
                needed = w;
            }
            line += (line.empty() ? "" : " ") + word;
            line_w = needed;
        }
        emit();
        gap_ = spec.size * spec.after;
    }

    void picture(const Block& b) {
        std::string data;
        int iw = 0, ih = 0;
        if (b.image.empty() || !read_entry(zip_, b.image, data) || !image_size(data, iw, ih)) return;
        // Big pictures fit the page; small ones (ornaments, icons) keep their size.
        float s = std::min({1.0f, CONTENT_W / iw, CONTENT_H / ih});
        if (iw > CONTENT_W * 0.5f) s = std::min(CONTENT_W / iw, CONTENT_H / ih);
        float w = iw * s, h = ih * s;
        space(12);
        if (y_ + h > CONTENT_H) new_page();
        page().push_back(Item{Item::IMAGE, BODY, "", b.image, MARGIN_X + (CONTENT_W - w) * 0.5f, MARGIN_TOP + y_, w, h});
        y_ += h;
        gap_ = 14;
    }

    Fonts& fonts_;
    zip_t* zip_;
    std::vector<Page> pages_;
    float y_ = 0, gap_ = 0;
    std::unordered_map<std::string, float> widths_[STYLE_COUNT];
};

}  // namespace

struct Book::Impl {
    zip_t* zip = nullptr;
    std::string title;
    std::vector<Page> pages;
    Fonts fonts;

    ~Impl() {
        if (zip) zip_discard(zip);
    }

    // Reads the reading order and each chapter's content.
    std::string load(std::vector<Block>& blocks) {
        std::string container;
        if (!read_entry(zip, "META-INF/container.xml", container)) return "This isn't an EPUB book.";
        tinyxml2::XMLDocument cdoc;
        cdoc.Parse(container.data(), container.size());
        const tinyxml2::XMLElement* rootfile = find_element(cdoc.RootElement(), "rootfile");
        const char* opf_path = rootfile ? attr(rootfile, "full-path") : nullptr;
        std::string opf;
        if (!opf_path || !read_entry(zip, opf_path, opf)) return "The book's contents list is missing.";

        tinyxml2::XMLDocument odoc;
        if (odoc.Parse(opf.data(), opf.size()) != tinyxml2::XML_SUCCESS) return "The book's contents list is damaged.";
        std::string opf_dir = dir_of(opf_path);
        const tinyxml2::XMLElement* root = odoc.RootElement();
        if (const tinyxml2::XMLElement* t = find_element(root, "title"))
            if (t->GetText()) title = util::trim(t->GetText());

        struct Entry {
            std::string href, type;
        };
        std::map<std::string, Entry> manifest;
        std::string cover_id;
        if (const tinyxml2::XMLElement* m = find_element(root, "manifest")) {
            for (const tinyxml2::XMLElement* it = m->FirstChildElement(); it; it = it->NextSiblingElement()) {
                const char* id = attr(it, "id");
                const char* href = attr(it, "href");
                if (!id || !href) continue;
                const char* type = attr(it, "media-type");
                const char* props = attr(it, "properties");
                manifest[id] = Entry{resolve(opf_dir, href), type ? type : ""};
                if (props && std::strstr(props, "cover-image")) cover_id = id;
            }
        }
        if (cover_id.empty())  // EPUB 2: <meta name="cover" content="id"/>
            if (const tinyxml2::XMLElement* md = find_element(root, "metadata"))
                for (const tinyxml2::XMLElement* e = md->FirstChildElement(); e; e = e->NextSiblingElement())
                    if (local_name(e) == "meta" && attr(e, "name") && std::strcmp(attr(e, "name"), "cover") == 0 &&
                        attr(e, "content"))
                        cover_id = attr(e, "content");

        std::vector<std::string> spine;
        if (const tinyxml2::XMLElement* s = find_element(root, "spine"))
            for (const tinyxml2::XMLElement* r = s->FirstChildElement(); r; r = r->NextSiblingElement()) {
                const char* idref = attr(r, "idref");
                auto it = idref ? manifest.find(idref) : manifest.end();
                if (it != manifest.end() && it->second.type.find("html") != std::string::npos)
                    spine.push_back(it->second.href);
            }
        if (spine.empty()) return "This book has no chapters.";

        for (size_t i = 0; i < spine.size(); i++) {
            std::string html;
            if (!read_entry(zip, spine[i], html)) continue;
            size_t before = blocks.size();
            blocks.push_back(Block{Block::CHAPTER, BODY, "", ""});
            Chapter chapter(dir_of(spine[i]), blocks);
            tinyxml2::XMLDocument doc;
            std::string xml = xmlify_entities(html);
            if (doc.Parse(xml.data(), xml.size()) == tinyxml2::XML_SUCCESS) {
                chapter.walk(doc.FirstChild());
                chapter.flush();
            } else {
                log_message(LOG_WARNING, "EPUB", "%s isn't well-formed; showing its text only", spine[i].c_str());
                for (const std::string& para : util::split(util::html_to_text(html), '\n'))
                    if (!util::trim(para).empty()) blocks.push_back(Block{Block::TEXT, BODY, util::trim(para), ""});
            }
            // A book without a cover page gets its declared cover image first.
            if (i == 0 && !cover_id.empty() && manifest.count(cover_id)) {
                bool has_image = false;
                for (size_t k = before; k < blocks.size(); k++) has_image = has_image || blocks[k].kind == Block::IMAGE;
                if (!has_image) {
                    blocks.insert(blocks.begin() + before, Block{Block::IMAGE, BODY, "", manifest[cover_id].href});
                    blocks.insert(blocks.begin() + before, Block{Block::CHAPTER, BODY, "", ""});
                }
            }
        }
        return "";
    }
};

Book::Book() : d_(std::make_unique<Impl>()) {}
Book::~Book() = default;

std::string Book::open(const std::string& path) {
    int err = 0;
    d_->zip = zip_open(path.c_str(), ZIP_RDONLY, &err);
    if (!d_->zip) return "The file may be damaged or isn't an EPUB book.";
    std::vector<Block> blocks;
    std::string error = d_->load(blocks);
    if (!error.empty()) return error;
    double t0 = util::now_seconds();
    d_->pages = Layout(d_->fonts, d_->zip).run(blocks);
    log_message(LOG_OK, "EPUB", "%s: %zu pages (%.0f ms)", util::file_name(path).c_str(), d_->pages.size(),
                (util::now_seconds() - t0) * 1000);
    return d_->pages.empty() ? "This book has no readable pages." : "";
}

int Book::page_count() const { return (int)d_->pages.size(); }
std::string Book::title() const { return d_->title; }

SDL_Surface* Book::render(int index, int max_w, int max_h) {
    if (index < 0 || index >= (int)d_->pages.size()) return nullptr;
    float s = std::min(max_w / PAGE_W, max_h / PAGE_H);
    int w = (int)std::lround(PAGE_W * s), h = (int)std::lround(PAGE_H * s);
    SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!out) return nullptr;
    SDL_FillRect(out, nullptr, SDL_MapRGBA(out->format, PAPER.r, PAPER.g, PAPER.b, 255));

    for (const Item& it : d_->pages[index]) {
        SDL_Rect r{(int)std::lround(it.x * s), (int)std::lround(it.y * s), (int)std::lround(it.w * s),
                   (int)std::lround(it.h * s)};
        if (it.kind == Item::RULE) {
            SDL_FillRect(out, &r, SDL_MapRGBA(out->format, 200, 194, 184, 255));
        } else if (it.kind == Item::LINE) {
            const StyleSpec& spec = STYLES[it.style];
            TTF_Font* f = d_->fonts.get(spec.bold, std::max(6, (int)std::lround(spec.size * s)));
            if (!f) continue;
            SDL_Surface* t = TTF_RenderUTF8_Blended(f, it.text.c_str(), INK);
            if (!t) continue;
            // Center the glyphs in the line box.
            SDL_Rect dst{r.x, r.y + (r.h - t->h) / 2, t->w, t->h};
            SDL_BlitSurface(t, nullptr, out, &dst);
            SDL_FreeSurface(t);
        } else {
            std::string data;
            if (!read_entry(d_->zip, it.image, data)) continue;
            SDL_Surface* img = images::decode(data, std::max(1, r.w), std::max(1, r.h));
            if (!img) continue;
            SDL_Rect dst{r.x + (r.w - img->w) / 2, r.y + (r.h - img->h) / 2, img->w, img->h};
            SDL_SetSurfaceBlendMode(img, SDL_BLENDMODE_BLEND);
            SDL_BlitSurface(img, nullptr, out, &dst);
            SDL_FreeSurface(img);
        }
    }
    return out;
}

}  // namespace epub
