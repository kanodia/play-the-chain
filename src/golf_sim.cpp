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

GolfCourse golf_course(uint32_t day) {
  Rng rng(day);
  GolfCourse c;
  c.day = day;

  // Two waves and a tilt, flattened at the tee and around the cup.
  float a1 = rng.uniform(0.6f, 1.8f), k1 = rng.uniform(0.12f, 0.28f), p1 = rng.uniform(0, 6.28f);
  float a2 = rng.uniform(0.2f, 0.7f), k2 = rng.uniform(0.4f, 0.8f), p2 = rng.uniform(0, 6.28f);
  float tilt = rng.uniform(-0.06f, 0.06f);
  c.cup_x = std::round(rng.uniform(38.0f, 54.0f));
  auto raw = [&](float x) { return a1 * std::sin(k1 * x + p1) + a2 * std::sin(k2 * x + p2) + tilt * x; };
  float tee_h = raw(3.0f), cup_h = raw(c.cup_x);
  auto height = [&](float x) {
    float h = raw(x);
    float dt = std::fabs(x - 3.0f), dc = std::fabs(x - c.cup_x);
    if (dt < 2.0f) h = tee_h + (h - tee_h) * (dt / 2.0f);
    if (dc < 2.5f) h = cup_h + (h - cup_h) * (dc / 2.5f);
    return h;
  };

  for (float x = COURSE_LEFT; x <= COURSE_RIGHT + 0.01f; x += 1.0f) {
    if (std::fabs(x - c.cup_x) < 1.0f) { // the cup, cut into the flat green
      if (x < c.cup_x) {
        c.ground.push_back({c.cup_x - CUP_HALF_W, cup_h});
        c.ground.push_back({c.cup_x - CUP_HALF_W, cup_h - CUP_DEPTH});
        c.ground.push_back({c.cup_x + CUP_HALF_W, cup_h - CUP_DEPTH});
        c.ground.push_back({c.cup_x + CUP_HALF_W, cup_h});
      }
      continue;
    }
    c.ground.push_back({x, height(x)});
  }
  c.cup_y = cup_h;
  c.tee = {3.0f, tee_h + GOLF_BALL_RADIUS};

  // Up to two obstacles between the tee and the cup: a wall or a tilted plank.
  int n = int(rng.next() % 3);
  for (int i = 0; i < n; ++i) {
    GolfBox b;
    b.x = std::round(rng.uniform(10.0f, c.cup_x - 6.0f));
    bool wall = rng.next() % 2;
    b.half_w = wall ? rng.uniform(0.2f, 0.4f) : rng.uniform(1.0f, 2.2f);
    b.half_h = wall ? rng.uniform(0.6f, 1.6f) : 0.15f;
    b.angle = wall ? 0.0f : rng.uniform(-0.5f, 0.5f);
    b.y = height(b.x) + (wall ? b.half_h : rng.uniform(1.2f, 2.5f));
    c.boxes.push_back(b);
  }
  c.par = n == 2 ? 4 : 3;
  return c;
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
    world.Step(STEP, 8, 3);
    b2Vec2 p = ball->GetPosition();
    if (r.steps % 2 == 0) r.frames.push_back({p.x, p.y});
    if (p.x < COURSE_LEFT - 1.0f || p.x > COURSE_RIGHT || p.y < COURSE_FLOOR) {
      r.how = ShotEnd::Out;
      break;
    }
    bool in_cup = std::fabs(p.x - c.cup_x) < CUP_HALF_W &&
                  p.y < c.cup_y - CUP_DEPTH + GOLF_BALL_RADIUS * 1.6f;
    float v = ball->GetLinearVelocity().Length();
    if (in_cup && v < 1.5f) {
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
  std::string out = "{\"day\":" + std::to_string(c.day) + ",\"par\":" + std::to_string(c.par);
  std::snprintf(buf, sizeof buf, ",\"cup\":{\"x\":%.3f,\"y\":%.3f,\"half_w\":%.3f,\"depth\":%.3f}",
                c.cup_x, c.cup_y, CUP_HALF_W, CUP_DEPTH);
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
