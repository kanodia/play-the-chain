#pragma once

// Chain Mini-Golf: one 3D hole a day, every putt simulated by Bullet Physics
// inside the canister. Nothing here touches the IC, so it also builds natively.
// Coordinates: x along the green, z across it, y up; metres.

#include <cstdint>
#include <string>
#include <vector>

struct MgPoint {
  float x = 0, y = 0, z = 0;
};

enum class MgKind { Block, Bumper, Windmill };

struct MgObstacle {
  MgKind kind = MgKind::Block;
  float x = 0, z = 0;
  float half_x = 0, half_z = 0, half_y = 0; // Block, Windmill blade; Bumper: radius in half_x
  float yaw = 0;   // Block
  float omega = 0; // Windmill, rad/s about y
};

struct MgCourse {
  uint32_t day = 0;
  int nx = 0, nz = 0;  // grid cells
  float cell = 0.25f;  // metres per cell
  std::vector<float> heights; // (nx + 1) * (nz + 1), row by row along x
  int cup_i = 0, cup_k = 0;   // the cell cut out for the cup
  MgPoint tee;
  std::vector<MgObstacle> obstacles;
  int par = 2;

  float height(int i, int k) const { return heights[size_t(k) * (nx + 1) + i]; }
  float cup_x() const { return (cup_i + 0.5f) * cell; }
  float cup_z() const { return (cup_k + 0.5f) * cell; }
  float cup_rim() const; // the green's height at the cup
};

enum class MgEnd { Rest, Holed, Out };

struct MgShot {
  std::vector<MgPoint> frames; // the ball, 30 per simulated second
  MgPoint end;
  MgEnd how = MgEnd::Rest;
  int steps = 0;
};

constexpr float MG_BALL_RADIUS = 0.06f;
constexpr float MG_MAX_POWER = 100.0f;
constexpr int MG_STEPS_PER_FRAME = 4; // 120 Hz physics, 30 frames a second

MgCourse mg_course(uint32_t day);

// The windmills' angle at the start of a shot: fixed by the day and the
// stroke, so a replay sees exactly the same blades.
float mg_phase(uint32_t day, size_t stroke);

// One putt from `from`: yaw in degrees (0 = along +x, 90 = +z), power in percent.
MgShot mg_shoot(const MgCourse &course, MgPoint from, float yaw, float power,
                float phase);

std::string mg_course_json(const MgCourse &course);
