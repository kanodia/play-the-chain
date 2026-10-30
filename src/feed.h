#pragma once

#include <cstdint>
#include <string>

// The feed: results players chose to post, and refuels. A post stores only
// what it points at (a golf round, a chess game, a refuel) and who posted it;
// the headline is written from the canister's own record of that result when
// the feed is read, so a post can't claim anything the chain didn't compute.
// One file per post, /feed_<id>.txt, in stable memory.

constexpr int FEED_REACTIONS = 3; // 🔥 👏 😮
constexpr uint32_t FEED_REACTION_CAP = 99'999;

struct FeedPost {
  uint64_t id = 0;
  std::string kind; // "golf", "chess" or "fuel"
  uint64_t ref = 0; // the round, game or refuel (1-based) it shows
  uint64_t time_ns = 0;
  std::string name; // cleaned like a golf name; "anon" if none
  uint32_t react[FEED_REACTIONS] = {0, 0, 0};
};

uint64_t feed_count();
bool feed_load(uint64_t id, FeedPost &post);
void feed_save(const FeedPost &post);

// The post that shows kind/ref, or 0 if nobody posted it yet.
uint64_t feed_posted(const std::string &kind, uint64_t ref);

// A new post with the next id, saved, and marked as posted for kind/ref.
FeedPost feed_add(const std::string &kind, uint64_t ref, const std::string &name,
                  uint64_t now_ns);
