// On-device YouTube recommendations, in the spirit of Flow's FlowNeuro: learns from what you
// watch (and how much of it), skip, search for and subscribe to, and builds a "For you" feed
// from videos related to ones you enjoyed, your subscriptions' uploads and searches for your
// interests. Everything stays in the local store; there is no account and nothing is sent anywhere.
#pragma once

#include <string>

#include "services/youtube.hpp"

namespace yt_recs {

// Signals. Safe to call from any thread.
void on_watch(const youtube::Video& v, double watched_seconds, bool finished);
void on_search(const std::string& query);
void on_subscribe(const std::string& channel_id, bool subscribed);
void not_interested(const youtube::Video& v);

// True once there's something to personalise with (watches, searches or subscriptions).
bool has_profile();
// Bumped by every signal, so screens know when to rebuild their feed.
int version();
void reset();

// Builds the feed. Blocks on the network: run it on a task thread.
youtube::Results for_you();

}  // namespace yt_recs
