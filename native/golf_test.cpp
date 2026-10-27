// Native checks for Chain Golf: determinism, and that every hole can be won.
//   clang++ -std=c++20 -O2 -w -I src -I src/box2d/include native/golf_test.cpp src/golf_sim.cpp \
//     src/box2d/collision/*.cpp src/box2d/common/*.cpp src/box2d/dynamics/*.cpp \
//     -o build/golf_test && build/golf_test
#include <chrono>
#include <cmath>
#include <cstdio>

#include "golf_sim.h"

static float dist(const GolfCourse &c, GolfPoint p) { return std::fabs(p.x - c.cup_x); }

int main() {
  int unsolved = 0, total_strokes = 0, shots = 0;
  long long steps = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (uint32_t day = 20750; day < 20780; ++day) {
    GolfCourse c = golf_course(day);
    ShotResult a = golf_shoot(c, c.tee, 45, 60), b = golf_shoot(c, c.tee, 45, 60);
    if (a.end.x != b.end.x || a.end.y != b.end.y || a.frames.size() != b.frames.size()) {
      std::printf("day %u: NOT deterministic\n", day);
      return 1;
    }
    // A greedy golfer: each stroke, the shot that ends nearest the cup.
    GolfPoint ball = c.tee;
    int strokes = 0;
    bool holed = false;
    while (!holed && strokes < 8) {
      ++strokes;
      float best = 1e9f; ShotResult pick;
      for (float ang = 2.5f; ang < 178; ang += 2.5f)
        for (float pw = 4; pw <= 100; pw += 4) {
          ShotResult r = golf_shoot(c, ball, ang, pw);
          ++shots; steps += r.steps;
          float d = r.how == ShotEnd::Holed ? -1 : r.how == ShotEnd::Out ? 1e8f : dist(c, r.end);
          if (d < best) { best = d; pick = r; }
        }
      holed = pick.how == ShotEnd::Holed;
      ball = pick.end;
    }
    total_strokes += strokes;
    if (!holed) ++unsolved;
    std::printf("day %u par %d cup %.0f boxes %zu -> %s in %d\n", day, c.par, c.cup_x, c.boxes.size(),
                holed ? "holed" : "NOT holed", strokes);
  }
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("unsolved %d, avg strokes %.2f, %d shots, %.0f steps/shot, %.2f ms/shot native\n",
              unsolved, total_strokes / 30.0, shots, double(steps) / shots, 1000 * s / shots);
  return unsolved;
}
