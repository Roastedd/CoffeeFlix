#ifndef MAIN_H
#define MAIN_H

#include <SDL2/SDL.h>

// #define SCREEN_WIDTH 1920
// #define SCREEN_HEIGHT 1080
// #define UI_SCALE 1.5

// Optimized for Wii U gamepad (854x480) and TV (1280x720)
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720
#define UI_SCALE 1.35  // Clean layout that fits without overflow
#define BASE_FONT_SIZE 30  // Clear, readable text

#ifndef LEGACY
#define FONT_PATH "/vol/content/Roboto-Regular.ttf"
#define AMBIANCE_PATH "/vol/content/769925__lightmister__game-main-menu-fluids.mp3"
#else
#define FONT_PATH "/vol/external01/wiiu/apps/cafemp/content/Roboto-Regular.ttf"
#define AMBIANCE_PATH "/vol/external01/wiiu/apps/cafemp/content/769925__lightmister__game-main-menu-fluids.mp3"
#endif

#define BASE_PATH "/vol/external01/wiiu/apps/cafemp/"
#define MEDIA_PATH_AUDIO "/vol/external01/wiiu/apps/cafemp/Audio/"
#define MEDIA_PATH_VIDEO "/vol/external01/wiiu/apps/cafemp/Video/"
#define MEDIA_PATH_PHOTO "/vol/external01/wiiu/apps/cafemp/Photo/"
#define MEDIA_PATH_PDF "/vol/external01/wiiu/apps/cafemp/Library/"
#define SETTINGS_PATH "/vol/external01/wiiu/apps/cafemp/settings.json"

#define VERSION_STRING_NUMBER "v1.0.0"

#ifdef DEBUG
#define VERSION_STRING "CoffeeFlix " VERSION_STRING_NUMBER " (Build: " __DATE__ " " __TIME__ ")"
#elif LEGACY
#define VERSION_STRING "CoffeeFlix Legacy " VERSION_STRING_NUMBER
#else
#define VERSION_STRING "CoffeeFlix " VERSION_STRING_NUMBER
#endif

#define TOOLTIP_BAR_HEIGHT (48)

struct frame_info {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

#endif
