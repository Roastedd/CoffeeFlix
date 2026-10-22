#ifdef __ANDROID__
#include <SDL2/SDL.h>  // renames main to SDL_main, which SDLActivity starts
#endif

#include "app/app.hpp"

int main(int argc, char** argv) { return app::run(argc, argv); }
