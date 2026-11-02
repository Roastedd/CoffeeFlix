// Bringing the connections of another CoffeeFlix over: the sources and sign-ins in the coffeeflix.json that
// a Wii U keeps on its SD card (wiiu/apps/coffeeflix/). Copy that file to the Fire TV (the Receive files page takes it)
// or push it with adb, and it is read once: the file is deleted after a good import.
#pragma once
#include <string>

namespace settings_import {

struct Result {
    bool found = false;       // there was a file to read
    int imported = 0;         // sources added, and groups of sign-ins and viewing history taken
    std::string summary;      // what to tell the viewer
};

// Looks for coffeeflix.json in the media folder (and its Received folder). Nothing there: found stays false.
Result run();

// What is taken from a settings file's text; used by run() and by the tests.
Result import_text(const std::string& text);

}  // namespace settings_import
