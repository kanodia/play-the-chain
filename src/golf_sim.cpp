#include "golf_sim.h"

#include <cmath>
#include <cstdio>

#include "box2d/box2d.h"

// The course is 64 m long. The ball is out past either end or below the
// floor; a shot is also over after 20 s, wherever the ball is.
constexpr float COURSE_LEFT = -2.0f, COURSE_RIGHT = 62.0f, COURSE_FLOOR = -15.0f;
constexpr float CUP_HALF_W = 0.32f, CUP_DEPTH = 0.45f;
constexpr float STEP = 1.0f / 60.0f;
constexpr int MAX_STEPS = 20 * 60;
constexpr float MAX_SPEED = 26.0f; // m/s at full power

// A small deterministic generator, so a day always gets the same hole.
namespace {
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) {}
  uint32_t next() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return uint32_t(s >> 32);
  }
  float uniform(float lo, float hi) { return lo + (hi - lo) * float(next() % 1000001) / 1000000.0f; }
};
} // namespace

// The cup and the wind for a level. Level 10 keeps the classic numbers
// exactly, so older rounds recompute.
static void set_level(GolfCourse &c, uint32_t day, int level) {
  c.level = level;
  if (level < GOLF_CLASSIC) {
    float easy = float(GOLF_CLASSIC - level) / 9.0f; // 1 at level 1
    c.cup_half_w = CUP_HALF_W + (0.70f - CUP_HALF_W) * easy;
    c.catch_speed = 1.5f + 2.0f * easy;
  } else if (level > GOLF_CLASSIC) {
    float hard = float(level - GOLF_CLASSIC) / 10.0f; // 1 at level 20
    c.cup_half_w = CUP_HALF_W - 0.10f * hard;
    c.catch_speed = 1.5f - 0.6f * hard;
    // A headwind (towards the tee), from its own generator so the wind
    // doesn't move the hole.
    Rng w(uint64_t(day) ^ 0x57494E44ULL);
    c.wind = -3.0f * hard * w.uniform(0.85f, 1.15f);
  }
}

// Two waves and a tilt, flattened at the tee and around the cup.
struct Hills {
  float a1, k1, p1, a2, k2, p2, tilt, cup_x, tee_h = 0, cup_h = 0;
  Hills(Rng &rng, float cup_lo, float cup_hi) {
    a1 = rng.uniform(0.6f, 1.8f), k1 = rng.uniform(0.12f, 0.28f), p1 = rng.uniform(0, 6.28f);
    a2 = rng.uniform(0.2f, 0.7f), k2 = rng.uniform(0.4f, 0.8f), p2 = rng.uniform(0, 6.28f);
    tilt = rng.uniform(-0.06f, 0.06f);
    cup_x = std::round(rng.uniform(cup_lo, cup_hi));
  }
  void settle() { tee_h = raw(3.0f), cup_h = raw(cup_x); }
  float raw(float x) const { return a1 * std::sin(k1 * x + p1) + a2 * std::sin(k2 * x + p2) + tilt * x; }
  float height(float x) const {
    float h = raw(x);
    float dt = std::fabs(x - 3.0f), dc = std::fabs(x - cup_x);
    if (dt < 2.0f) h = tee_h + (h - tee_h) * (dt / 2.0f);
    if (dc < 2.5f) h = cup_h + (h - cup_h) * (dc / 2.5f);
    return h;
  }
};

// The ground, sampled every metre, with the cup cut into the flat green.
static void lay_ground(GolfCourse &c, const Hills &hills, bool drilled) {
  const float half = c.cup_half_w;
  c.cup_x = hills.cup_x;
  // Before the fix the cup was never cut: it sits on a whole metre, like the
  // samples, and the cut only happened at a sample short of it.
  bool cut = !drilled;
  for (float x = COURSE_LEFT; x <= COURSE_RIGHT + 0.01f; x += 1.0f) {
    if (!cut && x >= c.cup_x - 0.5f) {
      c.ground.push_back({c.cup_x - half, hills.cup_h});
      c.ground.push_back({c.cup_x - half, hills.cup_h - CUP_DEPTH});
      c.ground.push_back({c.cup_x + half, hills.cup_h - CUP_DEPTH});
      c.ground.push_back({c.cup_x + half, hills.cup_h});
      cut = true;
    }
    if (std::fabs(x - c.cup_x) < 1.0f) continue;
    c.ground.push_back({x, hills.height(x)});
  }
  c.cup_y = hills.cup_h;
  c.tee = {3.0f, hills.tee_h + GOLF_BALL_RADIUS};
}

// The first holes: one per day, the same at every level, up to two obstacles.
static GolfCourse classic_course(uint32_t day, bool drilled, int level) {
  Rng rng(day);
  GolfCourse c;
  c.day = day;
  set_level(c, day, level);
  Hills hills(rng, 38.0f, 54.0f);
  hills.settle();
  lay_ground(c, hills, drilled);

  // Up to two obstacles between the tee and the cup: a wall or a tilted plank.
  int n = int(rng.next() % 3);
  for (int i = 0; i < n; ++i) {
    GolfBox b;
    b.x = std::round(rng.uniform(10.0f, c.cup_x - 6.0f));
    bool wall = rng.next() % 2;
    b.half_w = wall ? rng.uniform(0.2f, 0.4f) : rng.uniform(1.0f, 2.2f);
    b.half_h = wall ? rng.uniform(0.6f, 1.6f) : 0.15f;
    b.angle = wall ? 0.0f : rng.uniform(-0.5f, 0.5f);
    b.y = hills.height(b.x) + (wall ? b.half_h : rng.uniform(1.2f, 2.5f));
    c.boxes.push_back(b);
  }
  c.par = n == 2 ? 4 : 3;
  return c;
}

// How many obstacles a level gets: one at the start, three or four at the top.
static int obstacle_count(Rng &rng, int level) {
  if (level <= 3) return 1;
  if (level <= 7) return 1 + int(rng.next() % 2);
  if (level <= 12) return 2 + int(rng.next() % 2);
  if (level <= 17) return 3;
  return 3 + int(rng.next() % 2);
}

// Every level of every day is its own hole. Higher levels get steeper hills
// and more, bigger obstacles: walls, tilted planks and floating blocks.
static GolfCourse varied_course(uint32_t day, int level) {
  Rng rng((uint64_t(day) << 8) ^ uint64_t(level) ^ 0x484F4C45ULL);
  GolfCourse c;
  c.day = day;
  set_level(c, day, level);
  const float t = float(level - 1) / float(GOLF_MAX_LEVEL - 1); // 0 at level 1, 1 at level 20
  Hills hills(rng, 34.0f, 54.0f);
  float steep = 0.7f + 0.7f * t;
  hills.a1 *= steep, hills.a2 *= steep;
  hills.settle();
  lay_ground(c, hills, true);

  // One obstacle in each stretch between the tee and the green, so they never
  // pile up on each other.
  int n = obstacle_count(rng, level);
  const float lo = 9.0f, hi = c.cup_x - 5.0f, span = (hi - lo) / float(n);
  for (int i = 0; i < n; ++i) {
    GolfBox b;
    b.x = std::round(rng.uniform(lo + span * i + 0.5f, lo + span * (i + 1) - 0.5f));
    int kind = int(rng.next() % 3); // 0 wall, 1 plank, 2 block
    // No wall just before the green: a ball that rolls back to its foot can't
    // climb out against the wind.
    if (i == n - 1 && kind == 0) kind = 1 + int(rng.next() % 2);
    float ground = hills.height(b.x);
    if (kind == 0) {
      b.half_w = rng.uniform(0.2f, 0.4f);
      b.half_h = rng.uniform(0.5f, 1.0f) + 0.9f * t;
      b.y = ground + b.half_h;
    } else if (kind == 1) {
      b.half_w = rng.uniform(1.0f, 2.2f);
      b.half_h = 0.15f;
      b.angle = rng.uniform(-0.5f, 0.5f);
      b.y = ground + rng.uniform(1.2f, 2.5f);
    } else {
      b.half_w = b.half_h = rng.uniform(0.4f, 0.7f) + 0.2f * t;
      b.angle = rng.uniform(-0.4f, 0.4f);
      b.y = ground + b.half_h + rng.uniform(1.5f, 3.5f);
    }
    c.boxes.push_back(b);
  }
  c.par = n <= 2 ? 3 : n == 3 ? 4 : 5;
  return c;
}

GolfCourse golf_course(uint32_t day, bool drilled, int level, bool varied) {
  return varied ? varied_course(day, level) : classic_course(day, drilled, level);
}

static void build_world(b2World &world, const GolfCourse &c) {
  b2BodyDef def;
  b2Body *ground = world.CreateBody(&def);
  for (size_t i = 0; i + 1 < c.ground.size(); ++i) {
    b2EdgeShape edge;
    edge.SetTwoSided({c.ground[i].x, c.ground[i].y}, {c.ground[i + 1].x, c.ground[i + 1].y});
    b2FixtureDef f;
    f.shape = &edge;
    f.friction = 0.6f;
    f.restitution = 0.25f;
    ground->CreateFixture(&f);
  }
  // A backstop behind the tee, so a shot can't roll off to the left.
  b2EdgeShape wall;
  wall.SetTwoSided({COURSE_LEFT, c.ground.front().y}, {COURSE_LEFT, c.ground.front().y + 30.0f});
  ground->CreateFixture(&wall, 0.0f);
  for (const GolfBox &b : c.boxes) {
    b2PolygonShape box;
    box.SetAsBox(b.half_w, b.half_h, {b.x, b.y}, b.angle);
    b2FixtureDef f;
    f.shape = &box;
    f.friction = 0.5f;
    f.restitution = 0.4f;
    ground->CreateFixture(&f);
  }
}

// The ground's height at x, between its samples.
static float ground_at(const GolfCourse &c, float x) {
  for (size_t i = 0; i + 1 < c.ground.size(); ++i) {
    const GolfPoint &a = c.ground[i], &b = c.ground[i + 1];
    if (x >= a.x && x <= b.x) return b.x > a.x ? a.y + (b.y - a.y) * (x - a.x) / (b.x - a.x) : std::max(a.y, b.y);
  }
  return c.ground.empty() ? 0.0f : c.ground.back().y;
}

ShotResult golf_shoot(const GolfCourse &c, GolfPoint from, float angle, float power) {
  b2World world({0.0f, -9.81f});
  build_world(world, c);

  b2BodyDef def;
  def.type = b2_dynamicBody;
  def.position.Set(from.x, from.y);
  def.bullet = true; // no tunnelling through the ground at speed
  def.angularDamping = 2.5f; // rolling resistance, so it stops
  def.linearDamping = 0.15f;
  b2Body *ball = world.CreateBody(&def);
  b2CircleShape circle;
  circle.m_radius = GOLF_BALL_RADIUS;
  b2FixtureDef f;
  f.shape = &circle;
  f.density = 1.0f;
  f.friction = 0.5f;
  f.restitution = 0.35f;
  ball->CreateFixture(&f);

  float rad = angle * 3.14159265f / 180.0f;
  float speed = MAX_SPEED * power / 100.0f;
  ball->SetLinearVelocity({speed * std::cos(rad), speed * std::sin(rad)});

  ShotResult r;
  int still = 0;
  for (r.steps = 1; r.steps <= MAX_STEPS; ++r.steps) {
    // Pro's wind pushes the ball in the air; on the ground the grass wins.
    if (c.wind != 0.0f) {
      b2Vec2 at = ball->GetPosition();
      if (at.y > ground_at(c, at.x) + GOLF_BALL_RADIUS * 2.0f)
        ball->ApplyForceToCenter({ball->GetMass() * c.wind, 0.0f}, true);
    }
    world.Step(STEP, 8, 3);
    b2Vec2 p = ball->GetPosition();
    if (r.steps % 2 == 0) r.frames.push_back({p.x, p.y});
    if (p.x < COURSE_LEFT - 1.0f || p.x > COURSE_RIGHT || p.y < COURSE_FLOOR) {
      r.how = ShotEnd::Out;
      break;
    }
    bool in_cup = std::fabs(p.x - c.cup_x) < c.cup_half_w &&
                  p.y < c.cup_y - CUP_DEPTH + GOLF_BALL_RADIUS * 1.6f;
    float v = ball->GetLinearVelocity().Length();
    if (in_cup && v < c.catch_speed) {
      r.how = ShotEnd::Holed;
      break;
    }
    still = v < 0.05f ? still + 1 : 0;
    if (still >= 20) break; // at rest
  }
  b2Vec2 p = ball->GetPosition();
  r.end = r.how == ShotEnd::Out ? from : GolfPoint{p.x, p.y};
  if (r.frames.empty() || r.frames.back().x != p.x || r.frames.back().y != p.y)
    r.frames.push_back({p.x, p.y});
  return r;
}

std::string golf_course_json(const GolfCourse &c) {
  char buf[160];
  std::string out = "{\"day\":" + std::to_string(c.day) + ",\"par\":" + std::to_string(c.par) +
                    ",\"level\":" + std::to_string(c.level);
  std::snprintf(buf, sizeof buf, ",\"wind\":%.3f,\"cup\":{\"x\":%.3f,\"y\":%.3f,\"half_w\":%.3f,\"depth\":%.3f}",
                c.wind, c.cup_x, c.cup_y, c.cup_half_w, CUP_DEPTH);
  out += buf;
  std::snprintf(buf, sizeof buf, ",\"tee\":{\"x\":%.3f,\"y\":%.3f},\"ball_radius\":%.3f",
                c.tee.x, c.tee.y, GOLF_BALL_RADIUS);
  out += buf;
  out += ",\"ground\":[";
  for (size_t i = 0; i < c.ground.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%s[%.3f,%.3f]", i ? "," : "", c.ground[i].x, c.ground[i].y);
    out += buf;
  }
  out += "],\"boxes\":[";
  for (size_t i = 0; i < c.boxes.size(); ++i) {
    const GolfBox &b = c.boxes[i];
    std::snprintf(buf, sizeof buf, "%s{\"x\":%.3f,\"y\":%.3f,\"half_w\":%.3f,\"half_h\":%.3f,\"angle\":%.4f}",
                  i ? "," : "", b.x, b.y, b.half_w, b.half_h, b.angle);
    out += buf;
  }
  return out + "]}";
}
