Here is a copy-and-paste guide to building a Wii U homebrew app with the same architecture as CafeMP (Native C++ + WUT + FFmpeg + SDL2/GX2).

This guide assumes you are on a Linux or macOS terminal (or WSL for Windows).

1. Environment Setup (The Foundation)

You need the devkitPro toolchain.[1][2][3] Do not try to use standard GCC; it will not work.[1][2]

Step 1.1: Install devkitPro (if you haven't)
Follow the official instructions here, then run:

code
Bash
download
content_copy
expand_less
# Update package database
dkp-pacman -Sy

# Install the core Wii U development group
dkp-pacman -S wiiu-dev

# Install essential port libraries (curl for networking, zlib, etc.)
dkp-pacman -S wiiu-curl wiiu-zlib wiiu-libpng wiiu-freetype

Step 1.2: Install SDL2 (The easy graphics backend)
While CafeMP uses raw GX2 (hard), most similar apps use SDL2 because it's easier.[1][2]

code
Bash
download
content_copy
expand_less
dkp-pacman -S wiiu-sdl2 wiiu-sdl2_image wiiu-sdl2_ttf
2. The "Hard Part": Building FFmpeg for Wii U

You cannot install FFmpeg via pacman.[1][2] You must cross-compile it manually using a specific configuration script that disables features the Wii U CPU can't handle.[1][2]

Run these commands one by one:

code
Bash
download
content_copy
expand_less
# 1. Create a working directory
mkdir wiiu-ffmpeg-build && cd wiiu-ffmpeg-build

# 2. Clone the official FFmpeg repo
git clone https://github.com/FFmpeg/FFmpeg.git
cd FFmpeg

# 3. Download the Wii U configure script (Code by GaryOderNichts)
wget https://raw.githubusercontent.com/GaryOderNichts/FFmpeg-wiiu/main/configure-wiiu
chmod +x configure-wiiu

# 4. Run the configure script (This sets up the cross-compiler)
./configure-wiiu

# 5. Compile (This will take a while!)
make -j4

# 6. Install to your devkitPro portlibs folder
make install

Note: If make install fails due to permissions, use sudo make install.[1][2]

3. Project Structure

Create a new folder for your project (MyMediaApp).[1][2]

File Tree:

code
Text
download
content_copy
expand_less
MyMediaApp/
├── CMakeLists.txt       # The build script
└── src/
    └── main.cpp         # Your source code

Copy-Paste this CMakeLists.txt:
This tells the compiler where to find WUT, SDL2, and FFmpeg.

code
Cmake
download
content_copy
expand_less
cmake_minimum_required(VERSION 3.13)

# Setup devkitPro toolchain
set(CMAKE_TOOLCHAIN_FILE "$ENV{DEVKITPRO}/wut/share/wut.toolchain.cmake")
project(MyMediaApp CXX)

set(CMAKE_CXX_STANDARD 17)

# Find Libraries
find_package(SDL2 REQUIRED)

# Add your source files
file(GLOB_RECURSE SOURCES "src/*.cpp")
add_executable(MyMediaApp ${SOURCES})

# Link Libraries (Order matters!)
# We link FFmpeg libraries (avformat, avcodec, etc.) and SDL2
target_link_libraries(MyMediaApp
    avformat
    avcodec
    avutil
    swresample
    swscale
    SDL2
    wut
    m
)

# Convert the compiled .elf to a runnable .rpx/.wuhb
include(wut_create_rpx)
wut_create_rpx(MyMediaApp)
wut_create_wuhb(MyMediaApp
    CONTENT_DIR "./content" # Optional: folder for assets
    NAME "My Media App"
    AUTHOR "You"
)
[1][2]
4. The Code Skeleton (main.cpp)

This is a minimal "Hello World" that initializes the system, sets up a window, and proves libraries are linked.[1][2]

code
C++
download
content_copy
expand_less
#include <whb/proc.h>       // WUT: Process management
#include <whb/log.h>        // WUT: Logging
#include <whb/log_console.h>// WUT: Console logging

#include <SDL2/SDL.h>       // SDL2: Graphics

// FFmpeg headers must be wrapped in extern "C"
extern "C" {
    #include <libavformat/avformat.h>
}

int main(int argc, char **argv) {
    // 1. Initialize WUT (Wii U OS wrapper)
    WHBProcInit();
    WHBLogConsoleInit();
    WHBLogPrintf("Initializing MyMediaApp...");

    // 2. Initialize SDL2 (Video System)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        WHBLogPrintf("SDL Init failed: %s", SDL_GetError());
    } else {
        WHBLogPrintf("SDL Init successful!");
    }

    // 3. Initialize FFmpeg (Just to prove it works)
    // av_register_all() is deprecated in newer FFmpeg, but good for old versions.
    // Modern FFmpeg initializes automatically, but we can print version to test.
    unsigned version = avformat_version();
    WHBLogPrintf("FFmpeg LibAvFormat Version: %d", version);

    // 4. Create a Window (720p is standard for Wii U GamePad/TV)
    SDL_Window *window = SDL_CreateWindow(
        "My Player", 
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
        1280, 720, 
        0
    );

    if (!window) {
        WHBLogPrintf("Window creation failed!");
    }

    // 5. Main Loop
    bool running = true;
    while (running && WHBProcIsRunning()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            }
            // Button input handling would go here
        }

        // Render Red Background
        SDL_Surface *screen = SDL_GetWindowSurface(window);
        SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 255, 0, 0));
        SDL_UpdateWindowSurface(window);
        
        // Let the CPU breathe
        SDL_Delay(16); 
    }

    // 6. Cleanup
    SDL_DestroyWindow(window);
    SDL_Quit();
    WHBLogConsoleFree();
    WHBProcShutdown();

    return 0;
}
5. How to Build & Run

Open your terminal in the MyMediaApp folder.[1][2]

Run these commands:

code
Bash
download
content_copy
expand_less
mkdir build && cd build
cmake ..
make
```[[1](https://www.google.com/url?sa=E&q=https%3A%2F%2Fvertexaisearch.cloud.google.com%2Fgrounding-api-redirect%2FAUZIYQHdyBznDNrzIr-JmShbxPVy6bQIHLEXGqaYWCwzGjxRQl3_nbmG1wIgFMvdzykCmCzfFqc-uuvbayoVlxa1DUuafHa3ef9U8P9RPFN5wXSY3rLsLvjZ-QzBD9k4iY0U_6YREKtoAg%3D%3D)][[2](https://www.google.com/url?sa=E&q=https%3A%2F%2Fvertexaisearch.cloud.google.com%2Fgrounding-api-redirect%2FAUZIYQGM8oGTSt81fg4jP3iHo-1ucw6h8AF3ku0f7ofjL0MYQwpekrNpf-5G7KyO-4J776lU4cQMZj3yTFmB94jF3FMZf0Kc1DwabBa2OBWyZ_uraFgHZyphRGGhLUH6hbgoJoyI_6UqAA%3D%3D)]

You will get a MyMediaApp.wuhb file.[1][2]

Copy this file to your SD card in wiiu/apps/.[1][2]

Launch it via the Wii U Menu (Aroma).[1][2]

How to add Streaming (YouTube/Jellyfin)?

To add streaming, you need to use libcurl to fetch the video URL before passing it to FFmpeg.[1][2]

In main.cpp:

code
C++
download
content_copy
expand_less
#include <curl/curl.h>

// Initialize curl globally
curl_global_init(CURL_GLOBAL_DEFAULT);

// Use standard curl logic to hit the Jellyfin API
// GET http://jellyfin-server/Items/{ID}/PlaybackInfo
// Parse the JSON (use cJSON library) to get the "DirectPlay" URL.
// Pass that URL to avformat_open_input() in FFmpeg.
Sources
help
youtube.com
youtube.com
wiiubrew.org
gbatemp.net
Google Search Suggestions
Display of Search Suggestions is required when using Grounding with Google Search. Learn more
how to compile cafemp wii u
wii u homebrew ffmpeg implementation
setting up wii u toolchain for media player development
cafemp source code structure analysis
build cafemp wii u documentation dependencies
building ffmpeg for wii u tutorial garyodernichts
devkitpro pacman list wiiu packages curl
wiiu-portlibs package list
wut sdl2 skeleton code wii u
simple wut gx2 hello world code snippet