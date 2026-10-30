// Native checks for Chain Mini-Golf: determinism, speed, and that a greedy
// putter can hole each of 20 days.
//   clang++ -std=c++20 -O2 -w -I src -I src/bullet native/minigolf_test.cpp src/minigolf_sim.cpp \
//     $(find src/bullet -name '*.cpp') -o build/minigolf_test && build/minigolf_test
#include <chrono>
#include <cmath>
#include <cstdio>

#include <cstdlib>

#include "minigolf_sim.h"

// Optional argument: the level, 1-20 (default 10, the classic hole).
int main(int argc, char **argv) {
  int level = argc > 1 ? std::atoi(argv[1]) : MG_CLASSIC;
  int days = argc > 2 ? std::atoi(argv[2]) : 20; // optional: how many days
  int unsolved = 0, shots = 0;
  long long steps = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (uint32_t day = 20750; day < 20750 + uint32_t(days); ++day) {
    MgCourse c = mg_course(day, level);
    MgShot a = mg_shoot(c, c.tee, 10, 50, mg_phase(day, 0)), b = mg_shoot(c, c.tee, 10, 50, mg_phase(day, 0));
    if (a.end.x != b.end.x || a.end.y != b.end.y || a.end.z != b.end.z) {
      std::printf("day %u: NOT deterministic\n", day);
      return 1;
    }
    MgPoint ball = c.tee;
    int strokes = 0;
    bool holed = false;
    while (!holed && strokes < 8) {
      float best = 1e9f;
      MgShot pick;
      for (float yaw = 0; yaw < 360; yaw += 5)
        for (float pw = 5; pw <= 100; pw += 5) {
          MgShot r = mg_shoot(c, ball, yaw, pw, mg_phase(day, strokes));
          ++shots; steps += r.steps;
          float d = r.how == MgEnd::Holed ? -1 : r.how == MgEnd::Out ? 1e8f
                    : std::hypot(r.end.x - c.cup_x(), r.end.z - c.cup_z());
          if (d < best) { best = d; pick = r; }
        }
      ++strokes;
      holed = pick.how == MgEnd::Holed;
      ball = pick.end;
    }
    if (!holed) ++unsolved;
    std::printf("day %u par %d obstacles %zu -> %s in %d\n", day, c.par, c.obstacles.size(), holed ? "holed" : "NOT holed", strokes);
  }
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("unsolved %d, %d putts, %.0f steps/putt, %.2f ms/putt native\n", unsolved, shots, double(steps) / shots, 1000 * s / shots);
  return unsolved;
}
