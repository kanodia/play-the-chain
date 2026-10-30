#pragma once

// Chain Mini-Golf rounds, one file each, /mg_round_<id>.txt, in stable memory,
// like Chain Golf's. Each shot keeps its exact inputs, so anyone can have
// the canister simulate it again and check it lands where it did.

#include <cstdint>
#include <string>
#include <vector>

#include "minigolf_sim.h"

struct MgStroke {
  float yaw = 0, power = 0;
  MgPoint end;
  MgEnd how = MgEnd::Rest;
  uint64_t instructions = 0;
};

struct MgRound {
  uint64_t id = 0;
  std::string key; // needed to shoot; the page keeps it in the browser
  uint32_t day = 0;
  uint64_t created_ns = 0;
  std::string name;
  int level = MG_CLASSIC;
  std::vector<MgStroke> shots;

  MgPoint ball(const MgCourse &course) const; // where the next shot starts
  int strokes() const;                        // an out of bounds costs one more
  bool holed() const;
  bool finished() const; // holed, or out of shots
  bool varied() const;   // played on its level's own green, not the classic one
};

constexpr size_t MG_MAX_SHOTS = 10;

MgRound mg_round_new(uint32_t day, const std::string &name, uint64_t now_ns,
                     int level = MG_CLASSIC);
// The course a round was played on: its day, at its level.
MgCourse mg_round_course(const MgRound &round);

// Called after every upgrade: the first one records which round is the first
// played on a varied green.
void mg_mark_varied_from();
bool mg_round_load(uint64_t id, MgRound &round);
void mg_round_save(const MgRound &round);
std::vector<uint64_t> mg_day_rounds(uint32_t day);

std::string mg_round_json(const MgRound &round, const MgCourse &course,
                          bool with_key);
std::string mg_frames_json(const std::vector<MgPoint> &frames);
// As golf_leaderboard_json: the highest level first, then the fewest strokes.
std::string mg_leaderboard_json(uint32_t day);
