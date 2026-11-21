// Keeps spotify::index (the songs of the account's playlists, for the search) up to date, in the background:
// the list of playlists, then one playlist a step, only those whose revision changed. A step is a task of its
// own, so the workers stay free for everything else in between.
#pragma once

namespace spotify_search_index {

// Main thread: starts an update when signed in and none ran for a while (`force`: now).
void maybe_run(bool force = false);
// Signing out: stops, and the index goes.
void stop();

bool running();
int done();   // playlists read in this update
int total();  // playlists this update reads (those that changed)

}  // namespace spotify_search_index
