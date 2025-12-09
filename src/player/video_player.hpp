#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include <SDL2/SDL.h>
extern "C" {
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libswresample/swresample.h>
    #include <libavutil/time.h>
}
#include "main.hpp"

int video_player_init(const char* filepath);
void video_player_play(bool play);
void video_player_seek(double seconds);
void video_player_update();
void video_player_cleanup();

double video_player_get_total_playback_time();
double video_player_get_current_playback_time();

// Performance statistics
struct video_stats {
    int frames_decoded;
    int frames_dropped;
    int width;
    int height;
};
void video_player_get_stats(video_stats* stats);

#endif
