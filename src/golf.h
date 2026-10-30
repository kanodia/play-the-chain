#pragma once

// Chain Golf rounds, one file each, /golf_round_<id>.txt, in stable memory
// like the chess games. Each shot keeps its exact inputs, so anyone can have
// the canister simulate it again and check it lands where it did.

#include <cstdint>
#include <string>
#include <vector>

#include "golf_sim.h"

struct GolfShot {
  float angle = 0, power = 0;
  GolfPoint end;
  ShotEnd how = ShotEnd::Rest;
  uint64_t instructions = 0;
};

struct GolfRound {
  uint64_t id = 0;
  std::string key; // needed to shoot; the page keeps it in the browser
  uint32_t day = 0;
  uint64_t created_ns = 0;
  std::string name;
  int level = GOLF_NORMAL;
  std::vector<GolfShot> shots;

  GolfPoint ball(const GolfCourse &course) const; // where the next shot starts
  int strokes() const;                            // an out of bounds costs one more
  bool holed() const;
  bool finished() const; // holed, or out of shots
  bool drilled() const;  // played on a hole with its cup cut
};

// The course a round was played on.
GolfCourse golf_round_course(const GolfRound &round);

// Called after every upgrade: the first upgrade with the fix records which
// round is the first one played on a drilled hole.
void golf_mark_drilled_from();

constexpr size_t GOLF_MAX_SHOTS = 12;

GolfRound golf_round_new(uint32_t day, const std::string &name, uint64_t now_ns,
                         int level = GOLF_NORMAL);
bool golf_round_load(uint64_t id, GolfRound &round);
void golf_round_save(const GolfRound &round);
std::vector<uint64_t> golf_day_rounds(uint32_t day);

// A nickname for the leaderboard: letters, digits, space, - _ . ; at most 16.
std::string golf_clean_name(const std::string &raw);

std::string golf_round_json(const GolfRound &round, const GolfCourse &course,
                            bool with_key);
std::string golf_frames_json(const std::vector<GolfPoint> &frames);
std::string golf_leaderboard_json(uint32_t day, int level = GOLF_NORMAL);
// "Easy", "Pro", or "" for Normal.
const char *golf_level_name(int level);
