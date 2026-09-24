// EPUB books laid out as portrait pages: chapters in reading order, headings, paragraphs,
// list items and pictures, rendered with the app's Inter font. Styling (CSS) is ignored, which
// suits novels and most non-fiction; the goal is comfortable reading on a TV, not fidelity.
#pragma once

#include <SDL2/SDL.h>

#include <memory>
#include <string>

namespace epub {

class Book {
public:
    Book();
    ~Book();
    Book(const Book&) = delete;
    Book& operator=(const Book&) = delete;

    // Reads and lays out the whole book. Returns a user-facing error, or "" on success.
    std::string open(const std::string& path);
    int page_count() const;
    std::string title() const;
    // RGBA page scaled to fit max_w x max_h; nullptr on failure. One thread at a time.
    SDL_Surface* render(int index, int max_w, int max_h);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace epub
