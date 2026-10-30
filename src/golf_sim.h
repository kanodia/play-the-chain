#pragma once

// Chain Golf: one hole a day, every shot simulated by Box2D inside the
// canister. Nothing here touches the IC, so it also builds natively.

#include <cstdint>
#include <string>
#include <vector>

struct GolfPoint {
  float x = 0, y = 0;
};

struct GolfBox { // a static obstacle, centred at (x, y)
  float x = 0, y = 0, half_w = 0, half_h = 0, angle = 0;
};

// Levels 1..20 of the same daily hole. Level 10 is the hole as it always was;
// below it the cup is wider and holds faster balls, above it the cup is
// tighter and a headwind grows.
constexpr int GOLF_CLASSIC = 10, GOLF_MAX_LEVEL = 20;

struct GolfCourse {
  uint32_t day = 0; // days since 1970-01-01, UTC
  int level = GOLF_CLASSIC;
  float cup_half_w = 0.32f;
  float wind = 0; // m/s², along x, on the ball while it flies (above level 10)
  float catch_speed = 1.5f; // the cup holds a ball slower than this, m/s
  std::vector<GolfPoint> ground; // left to right, the cup cut into it
  float cup_x = 0, cup_y = 0;    // the middle of the cup's rim
  GolfPoint tee;                 // where the ball starts
  std::vector<GolfBox> boxes;
  int par = 3;
};

enum class ShotEnd { Rest, Holed, Out };

struct ShotResult {
  std::vector<GolfPoint> frames; // the ball, every second physics step
  GolfPoint end;                 // where the ball stopped
  ShotEnd how = ShotEnd::Rest;
  int steps = 0;
};

constexpr float GOLF_BALL_RADIUS = 0.15f;
constexpr float GOLF_MAX_ANGLE = 180.0f; // degrees, 0 = straight right
constexpr float GOLF_MAX_POWER = 100.0f; // percent

// drilled: the cup cut into the ground. Rounds before the fix played holes
// without one, and keep them, so they still recompute exactly.
GolfCourse golf_course(uint32_t day, bool drilled = true, int level = GOLF_CLASSIC);

// One stroke from `from`. angle in degrees (0..180), power in percent.
ShotResult golf_shoot(const GolfCourse &course, GolfPoint from, float angle,
                      float power);

std::string golf_course_json(const GolfCourse &course);
