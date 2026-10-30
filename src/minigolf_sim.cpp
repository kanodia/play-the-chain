#include "minigolf_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <utility>

#include "BulletCollision/BroadphaseCollision/btDbvtBroadphase.h"
#include "BulletCollision/CollisionDispatch/btCollisionDispatcher.h"
#include "BulletCollision/CollisionDispatch/btDefaultCollisionConfiguration.h"
#include "BulletCollision/CollisionShapes/btBoxShape.h"
#include "BulletCollision/CollisionShapes/btBvhTriangleMeshShape.h"
#include "BulletCollision/CollisionShapes/btCylinderShape.h"
#include "BulletCollision/CollisionShapes/btSphereShape.h"
#include "BulletCollision/CollisionShapes/btTriangleMesh.h"
#include "BulletDynamics/ConstraintSolver/btSequentialImpulseConstraintSolver.h"
#include "BulletDynamics/Dynamics/btDiscreteDynamicsWorld.h"
#include "BulletDynamics/Dynamics/btRigidBody.h"
#include "LinearMath/btDefaultMotionState.h"

constexpr float PI = 3.14159265f;
constexpr float STEP = 1.0f / 120.0f;
constexpr int MAX_STEPS = 20 * 120;
constexpr float MAX_SPEED = 6.5f; // m/s at full power
constexpr float CUP_DEPTH = 0.2f;
constexpr float WALL_TOP = 0.6f;   // walls reach from y = -0.6 to here, above any swell

// The same small deterministic generator as Chain Golf.
namespace {
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed * 0xD1B54A32D192ED03ULL + 0x8CB92BA72F3D8DD7ULL) {}
  uint32_t next() {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return uint32_t(s >> 32);
  }
  float uniform(float lo, float hi) { return lo + (hi - lo) * float(next() % 1000001) / 1000000.0f; }
};
} // namespace

float MgCourse::cup_rim() const {
  return height(cup_i, cup_k); // the cup sits on a flattened patch
}

MgCourse mg_course(uint32_t day, int level) {
  Rng rng(day);
  MgCourse c;
  c.day = day;
  c.level = level;
  if (level < MG_CLASSIC) c.catch_speed = 0.6f + 0.8f * float(MG_CLASSIC - level) / 9.0f; // 1.4 at level 1
  if (level > MG_CLASSIC) c.catch_speed = 0.6f - 0.3f * float(level - MG_CLASSIC) / 10.0f; // 0.3 at 20
  c.nx = 72; // 18 m
  c.nz = 24; // 6 m
  c.cup_i = int(rng.uniform(60, 66));
  c.cup_k = int(rng.uniform(5, 18));
  c.tee = {1.0f, 0, (c.nz * c.cell) * 0.5f};

  // Gentle swells, flat at the tee and around the cup.
  float a = rng.uniform(0.04f, 0.12f), kx = rng.uniform(0.25f, 0.6f), kz = rng.uniform(0.4f, 1.1f);
  float px = rng.uniform(0, 6.28f), pz = rng.uniform(0, 6.28f), tilt = rng.uniform(-0.012f, 0.012f);
  float cx = c.cup_x(), cz = c.cup_z();
  auto raw = [&](float x, float z) { return a * std::sin(kx * x + px) * std::cos(kz * z + pz) + tilt * x; };
  float tee_h = raw(c.tee.x, c.tee.z), cup_h = raw(cx, cz);
  c.heights.resize(size_t(c.nx + 1) * (c.nz + 1));
  for (int k = 0; k <= c.nz; ++k)
    for (int i = 0; i <= c.nx; ++i) {
      float x = i * c.cell, z = k * c.cell;
      float h = raw(x, z);
      float dt = std::hypot(x - c.tee.x, z - c.tee.z), dc = std::hypot(x - cx, z - cz);
      if (dt < 1.5f) h = tee_h + (h - tee_h) * (dt / 1.5f);
      if (dc < 1.2f) h = cup_h + (h - cup_h) * (dc / 1.2f);
      c.heights[size_t(k) * (c.nx + 1) + i] = h;
    }
  c.tee.y = tee_h + MG_BALL_RADIUS;

  // Two or three obstacles between 4 m and 2 m short of the cup.
  int n = 2 + int(rng.next() % 2);
  bool windmill = false;
  for (int i = 0; i < n; ++i) {
    MgObstacle o;
    o.x = rng.uniform(4.0f, cx - 2.0f);
    o.z = rng.uniform(1.2f, c.nz * c.cell - 1.2f);
    uint32_t pick = rng.next() % 3;
    if (pick == 2 && !windmill) {
      windmill = true;
      o.kind = MgKind::Windmill;
      o.z = c.nz * c.cell * 0.5f;
      o.half_x = 1.4f; o.half_y = 0.12f; o.half_z = 0.06f;
      o.omega = rng.uniform(1.0f, 2.0f) * (rng.next() % 2 ? 1.0f : -1.0f);
    } else if (pick == 1) {
      o.kind = MgKind::Bumper;
      o.half_x = rng.uniform(0.18f, 0.35f);
      o.half_y = 0.15f;
    } else {
      o.kind = MgKind::Block;
      o.half_x = rng.uniform(0.15f, 0.6f);
      o.half_z = rng.uniform(0.15f, 0.9f);
      o.half_y = 0.15f;
      o.yaw = rng.uniform(-0.8f, 0.8f);
    }
    c.obstacles.push_back(o);
  }
  c.par = windmill ? 3 : 2;
  if (level > MG_CLASSIC) {
    for (MgObstacle &o : c.obstacles)
      if (o.kind == MgKind::Windmill) o.omega *= 1.0f + 0.12f * float(level - MG_CLASSIC); // 2.2x at 20
    // Bumpers guarding the cup: one from level 14, two from 18. Their own
    // generator, so the rest of the hole is the same at every level.
    Rng p(uint64_t(day) ^ 0x50524FULL);
    int guards = level >= 18 ? 2 : level >= 14 ? 1 : 0;
    for (int g = 0; g < guards; ++g) {
      MgObstacle guard;
      guard.kind = MgKind::Bumper;
      guard.x = cx - p.uniform(0.9f, 1.4f);
      guard.z = cz + (g ? -1.0f : 1.0f) * p.uniform(0.15f, 0.45f);
      guard.half_x = 0.16f;
      guard.half_y = 0.15f;
      c.obstacles.push_back(guard);
    }
  }
  return c;
}

float mg_phase(uint32_t day, size_t stroke) {
  return std::fmod(day * 0.7f + float(stroke) * 1.9f, 2 * PI);
}

static float ground_at(const MgCourse &c, float x, float z) {
  int i = std::max(0, std::min(c.nx - 1, int(x / c.cell)));
  int k = std::max(0, std::min(c.nz - 1, int(z / c.cell)));
  return c.height(i, k);
}

namespace {
// A world per shot: everything owned here, freed in reverse.
struct World {
  btDefaultCollisionConfiguration config;
  btCollisionDispatcher dispatcher{&config};
  btDbvtBroadphase broadphase;
  btSequentialImpulseConstraintSolver solver;
  btDiscreteDynamicsWorld world{&dispatcher, &broadphase, &solver, &config};
  btTriangleMesh mesh;
  std::vector<std::unique_ptr<btCollisionShape>> shapes;
  std::vector<std::unique_ptr<btDefaultMotionState>> states;
  std::vector<std::unique_ptr<btRigidBody>> bodies;

  ~World() {
    for (auto &b : bodies) world.removeRigidBody(b.get());
  }

  btRigidBody *add(btCollisionShape *shape, const btTransform &at, float mass, float friction, float restitution) {
    shapes.emplace_back(shape);
    states.emplace_back(new btDefaultMotionState(at));
    btVector3 inertia(0, 0, 0);
    if (mass > 0) shape->calculateLocalInertia(mass, inertia);
    btRigidBody::btRigidBodyConstructionInfo info(mass, states.back().get(), shape, inertia);
    info.m_friction = friction;
    info.m_restitution = restitution;
    bodies.emplace_back(new btRigidBody(info));
    world.addRigidBody(bodies.back().get());
    return bodies.back().get();
  }
};
} // namespace

static btTransform at(float x, float y, float z, float yaw = 0) {
  btTransform t;
  t.setIdentity();
  t.setOrigin(btVector3(x, y, z));
  t.setRotation(btQuaternion(btVector3(0, 1, 0), yaw));
  return t;
}

MgShot mg_shoot(const MgCourse &c, MgPoint from, float yaw, float power, float phase) {
  World w;
  w.world.setGravity(btVector3(0, -9.81f, 0));

  // The green: two triangles a cell, the cup cell left open, with the cup's
  // four sides and floor below it.
  auto v = [&](int i, int k) { return btVector3(i * c.cell, c.height(i, k), k * c.cell); };
  for (int k = 0; k < c.nz; ++k)
    for (int i = 0; i < c.nx; ++i) {
      if (i == c.cup_i && k == c.cup_k) continue;
      w.mesh.addTriangle(v(i, k), v(i + 1, k), v(i + 1, k + 1));
      w.mesh.addTriangle(v(i, k), v(i + 1, k + 1), v(i, k + 1));
    }
  btVector3 a = v(c.cup_i, c.cup_k), b = v(c.cup_i + 1, c.cup_k), d = v(c.cup_i, c.cup_k + 1), e = v(c.cup_i + 1, c.cup_k + 1);
  btVector3 down(0, -CUP_DEPTH, 0);
  for (auto [p, q] : {std::pair{a, b}, std::pair{b, e}, std::pair{e, d}, std::pair{d, a}}) {
    w.mesh.addTriangle(p, q, q + down);
    w.mesh.addTriangle(p, q + down, p + down);
  }
  w.mesh.addTriangle(a + down, b + down, e + down);
  w.mesh.addTriangle(a + down, e + down, d + down);
  w.add(new btBvhTriangleMeshShape(&w.mesh, true), at(0, 0, 0), 0, 0.6f, 0.2f);

  // Border walls, tall enough to reach above the highest swell.
  float L = c.nx * c.cell, W = c.nz * c.cell;
  w.add(new btBoxShape(btVector3(L / 2 + 0.1f, WALL_TOP, 0.05f)), at(L / 2, 0, -0.05f), 0, 0.4f, 0.6f);
  w.add(new btBoxShape(btVector3(L / 2 + 0.1f, WALL_TOP, 0.05f)), at(L / 2, 0, W + 0.05f), 0, 0.4f, 0.6f);
  w.add(new btBoxShape(btVector3(0.05f, WALL_TOP, W / 2)), at(-0.05f, 0, W / 2), 0, 0.4f, 0.6f);
  w.add(new btBoxShape(btVector3(0.05f, WALL_TOP, W / 2)), at(L + 0.05f, 0, W / 2), 0, 0.4f, 0.6f);

  // Obstacles. Windmill blades are kinematic: they turn at a fixed rate
  // from `phase`, and push the ball rather than being pushed.
  std::vector<std::pair<btRigidBody *, const MgObstacle *>> mills;
  for (const MgObstacle &o : c.obstacles) {
    float g = ground_at(c, o.x, o.z);
    if (o.kind == MgKind::Block) {
      w.add(new btBoxShape(btVector3(o.half_x, o.half_y + 0.3f, o.half_z)), at(o.x, g - 0.3f + o.half_y, o.z, o.yaw), 0, 0.4f, 0.5f);
    } else if (o.kind == MgKind::Bumper) {
      w.add(new btCylinderShape(btVector3(o.half_x, o.half_y + 0.3f, o.half_x)), at(o.x, g - 0.3f + o.half_y, o.z), 0, 0.2f, 0.95f);
    } else {
      w.add(new btBoxShape(btVector3(0.15f, 0.2f, 0.15f)), at(o.x, g + 0.1f, o.z), 0, 0.4f, 0.5f); // the hub
      btRigidBody *blade = w.add(new btBoxShape(btVector3(o.half_x, o.half_y, o.half_z)), at(o.x, g + o.half_y + 0.01f, o.z, phase), 0, 0.3f, 0.6f);
      blade->setCollisionFlags(blade->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
      blade->setActivationState(DISABLE_DEACTIVATION);
      mills.push_back({blade, &o});
    }
  }

  btRigidBody *ball = w.add(new btSphereShape(MG_BALL_RADIUS), at(from.x, from.y, from.z), 0.046f, 0.4f, 0.5f);
  ball->setRollingFriction(0.0006f);
  ball->setSpinningFriction(0.0006f);
  ball->setDamping(0.25f, 0.3f);
  ball->setCcdMotionThreshold(MG_BALL_RADIUS * 0.5f);
  ball->setCcdSweptSphereRadius(MG_BALL_RADIUS * 0.9f);
  ball->setActivationState(DISABLE_DEACTIVATION);
  float rad = yaw * PI / 180.0f, speed = MAX_SPEED * power / 100.0f;
  ball->setLinearVelocity(btVector3(speed * std::cos(rad), 0, speed * std::sin(rad)));

  MgShot r;
  int still = 0;
  float cx = c.cup_x(), cz = c.cup_z(), half = c.cell / 2, rim = c.cup_rim();
  for (r.steps = 1; r.steps <= MAX_STEPS; ++r.steps) {
    float t = r.steps * STEP;
    for (auto [blade, o] : mills) {
      float g = ground_at(c, o->x, o->z);
      blade->getMotionState()->setWorldTransform(at(o->x, g + o->half_y + 0.01f, o->z, phase + o->omega * t));
    }
    w.world.stepSimulation(STEP, 0, STEP); // exactly one fixed step
    btVector3 p = ball->getWorldTransform().getOrigin();
    if (r.steps % MG_STEPS_PER_FRAME == 0) r.frames.push_back({p.x(), p.y(), p.z()});
    if (p.y() < rim - 2.0f || p.x() < -0.5f || p.x() > L + 0.5f || p.z() < -0.5f || p.z() > W + 0.5f) {
      r.how = MgEnd::Out;
      break;
    }
    bool in_cup = std::fabs(p.x() - cx) < half && std::fabs(p.z() - cz) < half &&
                  p.y() < rim - CUP_DEPTH + MG_BALL_RADIUS * 1.8f;
    float vel = ball->getLinearVelocity().length();
    if (in_cup && vel < c.catch_speed) {
      r.how = MgEnd::Holed;
      break;
    }
    still = vel < 0.02f ? still + 1 : 0;
    if (still >= 30) break; // at rest for a quarter of a second
  }
  btVector3 p = ball->getWorldTransform().getOrigin();
  r.end = r.how == MgEnd::Out ? from : MgPoint{p.x(), p.y(), p.z()};
  r.frames.push_back({p.x(), p.y(), p.z()});
  return r;
}

std::string mg_course_json(const MgCourse &c) {
  char buf[200];
  std::snprintf(buf, sizeof buf,
                "{\"day\":%u,\"level\":%d,\"par\":%d,\"nx\":%d,\"nz\":%d,\"cell\":%.3f,\"cup\":{\"i\":%d,\"k\":%d,\"rim\":%.4f,\"depth\":%.3f},"
                "\"tee\":{\"x\":%.3f,\"y\":%.4f,\"z\":%.3f},\"ball_radius\":%.3f,\"steps_per_frame\":%d,\"heights\":[",
                c.day, c.level, c.par, c.nx, c.nz, c.cell, c.cup_i, c.cup_k, c.cup_rim(), CUP_DEPTH, c.tee.x, c.tee.y, c.tee.z,
                MG_BALL_RADIUS, MG_STEPS_PER_FRAME);
  std::string out = buf;
  for (size_t i = 0; i < c.heights.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%s%.4f", i ? "," : "", c.heights[i]);
    out += buf;
  }
  out += "],\"obstacles\":[";
  for (size_t i = 0; i < c.obstacles.size(); ++i) {
    const MgObstacle &o = c.obstacles[i];
    const char *kind = o.kind == MgKind::Block ? "block" : o.kind == MgKind::Bumper ? "bumper" : "windmill";
    std::snprintf(buf, sizeof buf,
                  "%s{\"kind\":\"%s\",\"x\":%.3f,\"z\":%.3f,\"ground\":%.4f,\"half_x\":%.3f,\"half_y\":%.3f,\"half_z\":%.3f,\"yaw\":%.4f,\"omega\":%.4f}",
                  i ? "," : "", kind, o.x, o.z, ground_at(c, o.x, o.z), o.half_x, o.half_y, o.half_z, o.yaw, o.omega);
    out += buf;
  }
  std::snprintf(buf, sizeof buf, "],\"wall_top\":%.3f}", WALL_TOP);
  return out + buf;
}
