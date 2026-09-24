// "Now playing" pill shown over browsing screens while audio keeps playing.
#pragma once

namespace mini_player {

void draw();           // browsing screens (rail visible)
void update_hidden();  // fullscreen screens: keep state ticking
void shutdown();

}  // namespace mini_player
