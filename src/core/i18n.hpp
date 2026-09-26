// The interface in other languages: the code has it in English, wrapped in tr(), and
// content/lang/<code>.json has each language's text for those English strings.
// tools/i18n.py lists what's missing and checks the translations.
#pragma once

#include <string>

namespace i18n {

struct Language {
    const char* code;     // "es"
    const char* name;     // in the language itself: "Español"
    const char* english;  // "Spanish" (a tr() string too)
};
extern const Language LANGUAGES[];
extern const int LANGUAGE_COUNT;

// Loads the language chosen in Settings, else the console's.
void init(const std::string& content_dir);

// The code chosen in Settings; empty to follow the console.
const std::string& chosen();
void choose(const std::string& code);
// The language showing now, and the console's (English when it isn't one of LANGUAGES).
const Language& current();
const Language& system();
// Changes each time the language does.
int generation();

// english in the current language, else english itself. Safe from any thread; the text stays
// valid for as long as the app runs.
const char* tr(const char* english) __attribute__((format_arg(1)));

}  // namespace i18n

using i18n::tr;

// Marks English text for tools/i18n.py where it can't be translated yet (a table made before the
// language is known): tr() it where it's shown.
#define N_(s) s
