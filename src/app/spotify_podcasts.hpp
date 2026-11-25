// The podcasts followed on Spotify, subscribed in CoffeeFlix's Podcasts section (their RSS feeds), so all their
// episodes are there: Spotify lists a show's episodes only to its own apps. One way only, Spotify to CoffeeFlix:
//   - a show followed on Spotify since the last sync is subscribed here (when its feed is found);
//   - one no longer followed is unsubscribed here, if the sync subscribed it (never the viewer's own);
//   - one unsubscribed here stays so: only shows newly followed are added.
// Shows only on Spotify (no feed) stay in the Spotify section's Podcasts tab.
#pragma once

#include <set>
#include <string>

namespace spotify_podcasts {

enum Choice { UNASKED, ON, OFF };
Choice choice();

// On: syncs now. Off: forgets what was synced, after unsubscribing it here too with `remove_synced`.
void turn_on();
void turn_off(bool remove_synced);

// Main thread: syncs when it's on, signed in and a while since the last time (`force`: now), in the background.
// Says what changed when it's done.
void maybe_sync(bool force = false);
bool syncing();

// The shows followed on Spotify that the syncs took over (found their feeds): the rest are only on Spotify.
std::set<std::string> taken_over();

}  // namespace spotify_podcasts
