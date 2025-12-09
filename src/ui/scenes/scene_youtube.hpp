#ifndef SCENE_YOUTUBE_HPP
#define SCENE_YOUTUBE_HPP

struct nk_context;
struct InputState;

void scene_youtube_render(struct nk_context *ctx);
void scene_youtube_input(InputState& input);

#endif // SCENE_YOUTUBE_HPP
