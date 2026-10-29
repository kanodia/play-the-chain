// Stockfish 11 as an IC canister. Every search runs on-chain, in one update
// message, and reports the wasm instructions it used.

#include "stockfish.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

#include "engine.h"
#include "fuel.h"
#include "games.h"
#include "golf.h"
#include "minigolf.h"
#include "ic_api.h"
#include "ui_html.h"

// A node costs about 8-10k instructions, so 2.5M nodes stays near 25B, well
// inside the 40B limit of one update message, whatever the position.
constexpr uint64_t MAX_NODES = 2'500'000;
constexpr int MAX_DEPTH = 20;

// Spend limits, so nobody can drain the canister's cycles. Up to the daily
// budget moves are full strength; then quick depth-8 replies
// until the hard limit; then none until 00:00 UTC. Counted in heap memory, so
// an upgrade starts the day's count again. Measured on mainnet, a day at the
// old 200B budget cost about 160B cycles, ~2.5x the instruction estimate, so
// 80B full-strength instructions keeps a day near $0.10.
#ifndef PTC_DAILY_BUDGET
#define PTC_DAILY_BUDGET 80'000'000'000
#define PTC_DAILY_HARD_LIMIT 100'000'000'000
#endif
constexpr uint64_t DAILY_BUDGET = PTC_DAILY_BUDGET;
constexpr uint64_t DAILY_HARD_LIMIT = PTC_DAILY_HARD_LIMIT;
constexpr int QUICK_DEPTH = 8;
constexpr uint64_t QUICK_NODES = 20'000;
constexpr uint64_t MAX_GAMES_PER_DAY = 300;
constexpr size_t MAX_PLIES = 600;

constexpr uint64_t MAX_GOLF_ROUNDS_PER_DAY = 3000;

static uint64_t g_day = 0, g_used_today = 0, g_games_today = 0, g_golf_today = 0,
                g_mg_today = 0;

static void roll_day() {
  uint64_t day = IC_API::time() / 86'400'000'000'000ULL;
  if (day != g_day) {
    g_day = day;
    g_used_today = 0;
    g_games_today = 0;
    g_golf_today = 0;
    g_mg_today = 0;
  }
}

static bool budget_left() {
  roll_day();
  return g_used_today < DAILY_HARD_LIMIT;
}

// The shared fuel tank is nearly empty: no more engine moves until a refuel.
static bool out_of_fuel(IC_API &ic_api) {
  return ic_api.get_canister_liquid_cycle_balance() < FUEL_FLOOR;
}

static const char *OUT_OF_FUEL =
    "the chain is out of fuel: send ICP to the canister, then press Refuel";

// Past the daily budget, searches are cut down to a quick reply.
static bool quick_mode() { return g_used_today >= DAILY_BUDGET; }

static uint64_t instructions() {
  return IC_API::performance_counter(
      IC_API::PerformanceCounterType::Instructions);
}

// Runs one capped search and measures it. Returns false on an invalid FEN.
static bool search(const std::string &fen, int depth, EngineResult &res,
                   uint64_t &used) {
  bool quick = quick_mode();
  uint64_t start = instructions();
  bool ok = engine_bestmove(fen, quick ? std::min(depth, QUICK_DEPTH) : depth,
                            quick ? QUICK_NODES : MAX_NODES, res);
  used = instructions() - start;
  g_used_today += used;
  return ok;
}

void canister_init() {
  IC_API ic_api(CanisterInit{std::string(__func__)}, false);
  engine_init();
  golf_mark_drilled_from();
}

// Heap state is not kept across an upgrade, so set the engine up again.
void canister_post_upgrade() {
  IC_API ic_api(CanisterPostUpgrade{std::string(__func__)}, false);
  engine_init();
  golf_mark_drilled_from();
}

void bestmove() {
  IC_API ic_api(CanisterUpdate{std::string(__func__)}, false);

  std::string fen;
  uint8_t depth{0};
  CandidArgs args_in;
  args_in.append(CandidTypeText{&fen});
  args_in.append(CandidTypeNat8{&depth});
  ic_api.from_wire(args_in);

  if (depth < 1 || depth > MAX_DEPTH) IC_API::trap("depth must be 1..20");
  if (!budget_left()) IC_API::trap("daily budget used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) IC_API::trap(OUT_OF_FUEL);

  EngineResult res;
  uint64_t used{0};
  if (!search(fen, depth, res, used)) IC_API::trap("invalid FEN: " + fen);

  CandidTypeRecord r_out;
  r_out.append("bestmove", CandidTypeText{res.bestmove});
  r_out.append("score_cp", CandidTypeInt32{res.score_cp});
  r_out.append("depth", CandidTypeNat8{static_cast<uint8_t>(res.depth)});
  r_out.append("nodes", CandidTypeNat64{res.nodes});
  r_out.append("instructions", CandidTypeNat64{used});
  ic_api.to_wire(r_out);
}

void new_game() {
  IC_API ic_api(CanisterUpdate{std::string(__func__)}, false);
  engine_new_game();
  ic_api.to_wire();
}

//----------------------------------------------------------------------------------
// HTTP: the board page, and a small JSON API in which the canister is the
// referee - it checks every human move, plays its own and records both.
//
//   GET  /                      the home page
//   GET  /chess  and  /game/<id>  the chess page
//   GET  /api/info              canister, subnet, games played
//   GET  /games                 the dashboard of every game
//   GET  /api/games             one summary line per game, and the cycles balance
//   GET  /api/game/<id>         a game with a receipt for every engine move
//   POST /api/game              body "<depth>\n<w|b>"  -> new game
//   POST /api/game/<id>/move    body "<key>\n<uci>"    -> the engine replies
//   GET  /golf  and  /golf/<id> the Chain Golf page
//   GET  /api/golf/today        today's hole and leaderboard
//   POST /api/golf/round        body "<nickname>"   -> new round
//   POST /api/golf/round/<id>/shot  body "<key>\n<angle>\n<power>"
//   GET  /api/golf/round/<id>   a round, with its hole
//   GET  /api/golf/round/<id>/shot/<n>  simulate shot n again, and check it
//   GET  /minigolf  and  /minigolf/<id>   Chain Mini-Golf, in 3D, with Bullet
//   GET  /api/minigolf/today, POST /api/minigolf/round, ...  as for golf;
//        a putt is "<key>\n<yaw 0-360>\n<power 1-100>"
//   POST /api/fuel                                     -> turn ICP sent to the
//                                                         canister into cycles

static std::string path_of(const std::string &url) {
  return url.substr(0, url.find('?'));
}

static IC_HttpResponse respond(uint16_t status, const std::string &type,
                               const std::string &body,
                               const char *cache = "no-store") {
  IC_HttpResponse response;
  response.status_code = status;
  response.headers.push_back({"Content-Type", type});
  response.headers.push_back({"Content-Length", std::to_string(body.size())});
  response.headers.push_back({"Cache-Control", cache});
  response.body.assign(body.begin(), body.end());
  return response;
}

static IC_HttpResponse json(const std::string &body) {
  return respond(200, "application/json", body);
}

static IC_HttpResponse error(uint16_t status, const std::string &msg) {
  return respond(status, "application/json", "{\"error\":\"" + msg + "\"}");
}

// "/api/game/12/move" with prefix "/api/game/" -> 12, rest "/move"
static bool parse_id(const std::string &path, const std::string &prefix,
                     uint64_t &id, std::string &rest) {
  if (path.rfind(prefix, 0) != 0) return false;
  size_t start = prefix.size(), end = start;
  while (end < path.size() && isdigit(static_cast<unsigned char>(path[end])))
    ++end;
  if (end == start || end - start > 18) return false;
  id = std::stoull(path.substr(start, end - start));
  rest = path.substr(end);
  return true;
}

static const char *status_text(GameStatus s) {
  switch (s) {
  case GameStatus::Checkmate: return "checkmate";
  case GameStatus::Stalemate: return "stalemate";
  case GameStatus::Draw: return "draw";
  default: return "playing";
  }
}

static std::string game_json(const Game &g, bool with_key) {
  GameState st = engine_game_state(g.moves());
  std::string out = "{\"id\":" + std::to_string(g.id) +
                    ",\"depth\":" + std::to_string(g.depth) +
                    ",\"human\":\"" + std::string(1, g.human) + "\"" +
                    ",\"created_ns\":" + std::to_string(g.created_ns) +
                    ",\"fen\":\"" + st.fen + "\"" + ",\"status\":\"" +
                    status_text(st.status) + "\"";
  if (with_key) out += ",\"key\":\"" + g.key + "\"";
  out += ",\"moves\":[";
  for (size_t i = 0; i < g.plies.size(); ++i) {
    const Ply &p = g.plies[i];
    if (i) out += ",";
    out += "{\"uci\":\"" + p.uci + "\",\"by\":\"" +
           (p.by_engine ? "engine" : "human") +
           "\",\"time_ns\":" + std::to_string(p.time_ns);
    if (p.by_engine)
      out += ",\"instructions\":" + std::to_string(p.instructions) +
             ",\"nodes\":" + std::to_string(p.nodes) +
             ",\"depth\":" + std::to_string(p.depth) +
             ",\"score_cp\":" + std::to_string(p.score_cp) +
             ",\"quick\":" + (p.quick ? "true" : "false");
    out += "}";
  }
  return out + "]}";
}

// The engine plays its move in `g`, measured, and records it. The caller
// checks budget_left() first.
static void engine_reply(Game &g) {
  bool quick = quick_mode();
  EngineResult res;
  uint64_t start = instructions();
  engine_bestmove_after(g.moves(), quick ? std::min(g.depth, QUICK_DEPTH) : g.depth,
                        quick ? QUICK_NODES : MAX_NODES, res);
  Ply p;
  p.uci = res.bestmove;
  p.by_engine = true;
  p.time_ns = IC_API::time();
  p.instructions = instructions() - start;
  p.nodes = res.nodes;
  p.depth = res.depth;
  p.score_cp = res.score_cp;
  p.quick = quick;
  g.plies.push_back(p);
  g_used_today += p.instructions;
}

static std::string u128(__uint128_t v) {
  if (v == 0) return "0";
  std::string out;
  while (v) { out.insert(out.begin(), char('0' + int(v % 10))); v /= 10; }
  return out;
}

// The tank: cycles, the accounts to send ICP to, and every refuel.
static std::string fuel_json(IC_API &ic_api) {
  std::vector<FuelEntry> log = fuel_log();
  __uint128_t cycles{0};
  uint64_t e8s{0};
  std::string entries;
  for (size_t i = 0; i < log.size(); ++i) {
    cycles += log[i].cycles;
    e8s += log[i].e8s;
    if (i + 20 < log.size()) continue; // the last 20
    if (!entries.empty()) entries += ",";
    entries += "{\"time_ns\":" + std::to_string(log[i].time_ns) +
               ",\"e8s\":" + std::to_string(log[i].e8s) +
               ",\"block\":" + std::to_string(log[i].block) +
               ",\"cycles\":" + u128(log[i].cycles) + "}";
  }
  return "\"balance\":" + u128(ic_api.get_canister_self_cycle_balance()) +
         ",\"liquid\":" + u128(ic_api.get_canister_liquid_cycle_balance()) +
         ",\"floor\":" + std::to_string(FUEL_FLOOR) +
         ",\"fuel\":{\"principal\":\"" + FUEL_PRINCIPAL +
         "\",\"account\":\"" + FUEL_ACCOUNT +
         "\",\"refuels\":" + std::to_string(log.size()) +
         ",\"e8s\":" + std::to_string(e8s) + ",\"cycles\":" + u128(cycles) +
         ",\"log\":[" + entries + "]}";
}

// Test builds run on local networks whose runtime lacks the newer subnet
// calls; mainnet builds ask the IC.
static std::string subnet_text(IC_API &ic_api) {
#ifdef PTC_PLAYGROUND
  (void)ic_api;
  return "local";
#else
  return ic_api.get_subnet_self().get_text();
#endif
}

static uint32_t subnet_nodes(IC_API &ic_api) {
#ifdef PTC_PLAYGROUND
  (void)ic_api;
  return 13;
#else
  return ic_api.get_subnet_node_count();
#endif
}

static IC_HttpResponse api_info(IC_API &ic_api) {
  roll_day();
  return json("{\"canister\":\"" + ic_api.get_canister_self().get_text() +
              "\",\"subnet\":\"" + subnet_text(ic_api) +
              "\",\"nodes\":" + std::to_string(subnet_nodes(ic_api)) +
              ",\"games\":" + std::to_string(games_count()) +
              ",\"engine\":\"Stockfish 11\",\"max_nodes\":" +
              std::to_string(MAX_NODES) +
              ",\"used_today\":" + std::to_string(g_used_today) +
              ",\"budget\":" + std::to_string(DAILY_BUDGET) +
              ",\"hard_limit\":" + std::to_string(DAILY_HARD_LIMIT) +
              ",\"quick_depth\":" + std::to_string(QUICK_DEPTH) + "," +
              fuel_json(ic_api) + "}");
}

// One summary per game, oldest first. A game is about 50k instructions to
// load and replay, so a few thousand games still fit easily in one message.
static IC_HttpResponse api_games(IC_API &ic_api) {
  roll_day();
  uint64_t n = games_count();
  std::string out = "{" + fuel_json(ic_api) +
                    ",\"nodes\":" + std::to_string(subnet_nodes(ic_api)) +
                    ",\"used_today\":" + std::to_string(g_used_today) +
                    ",\"budget\":" + std::to_string(DAILY_BUDGET) +
                    ",\"now_ns\":" + std::to_string(IC_API::time()) +
                    ",\"games\":[";
  bool first = true;
  for (uint64_t id = 1; id <= n; ++id) {
    Game g;
    if (!game_load(id, g)) continue;
    GameState st = engine_game_state(g.moves());
    uint64_t instr{0}, nodes{0}, last_ns = g.created_ns;
    int engine_moves{0}, quick{0};
    for (const Ply &p : g.plies) {
      last_ns = std::max(last_ns, p.time_ns);
      if (!p.by_engine) continue;
      ++engine_moves;
      instr += p.instructions;
      nodes += p.nodes;
      quick += p.quick;
    }
    // After checkmate, whoever moved last won.
    std::string winner;
    if (st.status == GameStatus::Checkmate && !g.plies.empty())
      winner = g.plies.back().by_engine ? "engine" : "human";
    if (!first) out += ",";
    first = false;
    out += "{\"id\":" + std::to_string(g.id) +
           ",\"depth\":" + std::to_string(g.depth) +
           ",\"human\":\"" + std::string(1, g.human) + "\"" +
           ",\"created_ns\":" + std::to_string(g.created_ns) +
           ",\"last_ns\":" + std::to_string(last_ns) +
           ",\"status\":\"" + status_text(st.status) + "\"" +
           ",\"winner\":\"" + winner + "\"" +
           ",\"plies\":" + std::to_string(g.plies.size()) +
           ",\"engine_moves\":" + std::to_string(engine_moves) +
           ",\"quick_moves\":" + std::to_string(quick) +
           ",\"instructions\":" + std::to_string(instr) +
           ",\"nodes\":" + std::to_string(nodes) + "}";
  }
  return json(out + "]}");
}

static IC_HttpResponse api_new_game(IC_API &ic_api, const std::string &body) {
  size_t nl = body.find('\n');
  int depth = std::atoi(body.substr(0, nl).c_str());
  char human = nl == std::string::npos || nl + 1 >= body.size() ? 'w' : body[nl + 1];
  if (depth < 1 || depth > MAX_DEPTH || (human != 'w' && human != 'b'))
    return error(400, "send <depth 1-20>\\n<w|b>");
  if (!budget_left() || g_games_today >= MAX_GAMES_PER_DAY)
    return error(429, "today's on-chain budget is used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) return error(503, OUT_OF_FUEL);
  ++g_games_today;

  Game g = game_new(depth, human, IC_API::time());
  if (g.engine_to_move()) engine_reply(g);
  game_save(g);
  return json(game_json(g, true));
}

static IC_HttpResponse api_move(IC_API &ic_api, uint64_t id,
                                const std::string &body) {
  Game g;
  if (!game_load(id, g)) return error(404, "no such game");

  size_t nl = body.find('\n');
  if (nl == std::string::npos) return error(400, "send <key>\\n<uci>");
  if (body.substr(0, nl) != g.key) return error(403, "not your game");
  std::string uci = body.substr(nl + 1);
  while (!uci.empty() && isspace(static_cast<unsigned char>(uci.back())))
    uci.pop_back();

  if (g.engine_to_move()) return error(409, "not your turn");
  if (engine_game_state(g.moves()).status != GameStatus::Playing)
    return error(409, "the game is over");
  if (g.plies.size() + 2 > MAX_PLIES) return error(409, "the game is too long");
  if (!budget_left())
    return error(429, "today's on-chain budget is used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) return error(503, OUT_OF_FUEL);

  std::vector<std::string> moves = g.moves();
  moves.push_back(uci);
  if (uci.size() < 4 || uci.size() > 5 || !engine_game_state(moves).legal)
    return error(400, "illegal move " + uci);

  Ply p;
  p.uci = uci;
  p.time_ns = IC_API::time();
  g.plies.push_back(p);
  if (engine_game_state(g.moves()).status == GameStatus::Playing)
    engine_reply(g);
  game_save(g);
  return json(game_json(g, false));
}

// Nothing here is certified, so every request, a 404 included, is upgraded to
// an update call: its reply goes through consensus and the gateway accepts it
// as is.
void http_request() {
  IC_API ic_api(CanisterQuery{std::string(__func__)}, false);
  IC_HttpRequest request;
  ic_api.from_wire(request);

  IC_HttpResponse response;
  response.status_code = 204;
  response.upgrade = true; // the gateway calls http_request_update
  ic_api.to_wire(response);
}

//----------------------------------------------------------------------------------
// Chain Golf: the canister simulates every shot with Box2D, so the browser
// only ever shows what the chain computed.

static uint32_t golf_today() {
  return static_cast<uint32_t>(IC_API::time() / 86'400'000'000'000ULL);
}

static IC_HttpResponse api_golf_today() {
  roll_day();
  uint32_t day = golf_today();
  return json("{\"course\":" + golf_course_json(golf_course(day)) +
              ",\"leaderboard\":" + golf_leaderboard_json(day) + "}");
}

static IC_HttpResponse api_golf_new(IC_API &ic_api, const std::string &body) {
  if (!budget_left() || g_golf_today >= MAX_GOLF_ROUNDS_PER_DAY)
    return error(429, "today's on-chain budget is used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) return error(503, OUT_OF_FUEL);
  ++g_golf_today;
  uint32_t day = golf_today();
  GolfRound r = golf_round_new(day, body, IC_API::time());
  golf_round_save(r);
  return json(golf_round_json(r, golf_course(day), true));
}

static IC_HttpResponse api_golf_shot(IC_API &ic_api, uint64_t id,
                                     const std::string &body) {
  GolfRound r;
  if (!golf_round_load(id, r)) return error(404, "no such round");
  std::istringstream in(body);
  std::string key, angle_s, power_s;
  std::getline(in, key);
  std::getline(in, angle_s);
  std::getline(in, power_s);
  if (key != r.key) return error(403, "not your round");
  if (r.day != golf_today()) return error(409, "that hole has closed, play today's");
  if (r.finished()) return error(409, "the round is over");
  float angle = std::strtof(angle_s.c_str(), nullptr);
  float power = std::strtof(power_s.c_str(), nullptr);
  if (!std::isfinite(angle) || !std::isfinite(power) || angle < 0 ||
      angle > GOLF_MAX_ANGLE || power <= 0 || power > GOLF_MAX_POWER)
    return error(400, "send <key>\\n<angle 0-180>\\n<power 1-100>");
  if (!budget_left())
    return error(429, "today's on-chain budget is used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) return error(503, OUT_OF_FUEL);

  GolfCourse course = golf_round_course(r);
  uint64_t start = instructions();
  ShotResult res = golf_shoot(course, r.ball(course), angle, power);
  GolfShot shot{angle, power, res.end, res.how, instructions() - start};
  g_used_today += shot.instructions;
  r.shots.push_back(shot);
  golf_round_save(r);
  return json("{\"round\":" + golf_round_json(r, course, false) +
              ",\"frames\":" + golf_frames_json(res.frames) + "}");
}

static IC_HttpResponse api_golf_round(uint64_t id) {
  GolfRound r;
  if (!golf_round_load(id, r)) return error(404, "no such round");
  GolfCourse course = golf_round_course(r);
  return json("{\"round\":" + golf_round_json(r, course, false) +
              ",\"course\":" + golf_course_json(course) + "}");
}

// Simulates shot n of a round again, from where it started, and says whether
// it ends exactly where the round says it did.
static IC_HttpResponse api_golf_replay(IC_API &ic_api, uint64_t id, size_t n) {
  GolfRound r;
  if (!golf_round_load(id, r)) return error(404, "no such round");
  if (n >= r.shots.size()) return error(404, "no such shot");
  if (!budget_left())
    return error(429, "today's on-chain budget is used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) return error(503, OUT_OF_FUEL);
  GolfCourse course = golf_round_course(r);
  GolfRound before = r;
  before.shots.resize(n);
  const GolfShot &s = r.shots[n];
  uint64_t start = instructions();
  ShotResult res = golf_shoot(course, before.ball(course), s.angle, s.power);
  uint64_t used = instructions() - start;
  g_used_today += used;
  bool matches = res.how == s.how && res.end.x == s.end.x && res.end.y == s.end.y;
  return json("{\"frames\":" + golf_frames_json(res.frames) +
              ",\"matches\":" + (matches ? "true" : "false") +
              ",\"instructions\":" + std::to_string(used) + "}");
}

//----------------------------------------------------------------------------------
// Chain Mini-Golf: the same, in 3D, with Bullet Physics. Windmill blades start
// each putt at an angle fixed by the day and the stroke number.

static IC_HttpResponse api_mg_today() {
  roll_day();
  uint32_t day = golf_today();
  return json("{\"course\":" + mg_course_json(mg_course(day)) +
              ",\"leaderboard\":" + mg_leaderboard_json(day) + "}");
}

static IC_HttpResponse api_mg_new(IC_API &ic_api, const std::string &body) {
  if (!budget_left() || g_mg_today >= MAX_GOLF_ROUNDS_PER_DAY)
    return error(429, "today's on-chain budget is used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) return error(503, OUT_OF_FUEL);
  ++g_mg_today;
  uint32_t day = golf_today();
  MgRound r = mg_round_new(day, body, IC_API::time());
  mg_round_save(r);
  return json(mg_round_json(r, mg_course(day), true));
}

static IC_HttpResponse api_mg_shot(IC_API &ic_api, uint64_t id, const std::string &body) {
  MgRound r;
  if (!mg_round_load(id, r)) return error(404, "no such round");
  std::istringstream in(body);
  std::string key, yaw_s, power_s;
  std::getline(in, key);
  std::getline(in, yaw_s);
  std::getline(in, power_s);
  if (key != r.key) return error(403, "not your round");
  if (r.day != golf_today()) return error(409, "that hole has closed, play today's");
  if (r.finished()) return error(409, "the round is over");
  float yaw = std::strtof(yaw_s.c_str(), nullptr);
  float power = std::strtof(power_s.c_str(), nullptr);
  if (!std::isfinite(yaw) || !std::isfinite(power) || yaw < 0 || yaw > 360 ||
      power <= 0 || power > MG_MAX_POWER)
    return error(400, "send <key>\\n<yaw 0-360>\\n<power 1-100>");
  if (!budget_left())
    return error(429, "today's on-chain budget is used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) return error(503, OUT_OF_FUEL);

  MgCourse course = mg_course(r.day);
  float phase = mg_phase(r.day, r.shots.size());
  uint64_t start = instructions();
  MgShot res = mg_shoot(course, r.ball(course), yaw, power, phase);
  MgStroke stroke{yaw, power, res.end, res.how, instructions() - start};
  g_used_today += stroke.instructions;
  r.shots.push_back(stroke);
  mg_round_save(r);
  return json("{\"round\":" + mg_round_json(r, course, false) +
              ",\"frames\":" + mg_frames_json(res.frames) + "}");
}

static IC_HttpResponse api_mg_round(uint64_t id) {
  MgRound r;
  if (!mg_round_load(id, r)) return error(404, "no such round");
  MgCourse course = mg_course(r.day);
  return json("{\"round\":" + mg_round_json(r, course, false) +
              ",\"course\":" + mg_course_json(course) + "}");
}

static IC_HttpResponse api_mg_replay(IC_API &ic_api, uint64_t id, size_t n) {
  MgRound r;
  if (!mg_round_load(id, r)) return error(404, "no such round");
  if (n >= r.shots.size()) return error(404, "no such shot");
  if (!budget_left())
    return error(429, "today's on-chain budget is used up, back at 00:00 UTC");
  if (out_of_fuel(ic_api)) return error(503, OUT_OF_FUEL);
  MgCourse course = mg_course(r.day);
  MgRound before = r;
  before.shots.resize(n);
  const MgStroke &s = r.shots[n];
  uint64_t start = instructions();
  MgShot res = mg_shoot(course, before.ball(course), s.yaw, s.power, mg_phase(r.day, n));
  uint64_t used = instructions() - start;
  g_used_today += used;
  bool matches = res.how == s.how && res.end.x == s.end.x && res.end.y == s.end.y &&
                 res.end.z == s.end.z;
  return json("{\"frames\":" + mg_frames_json(res.frames) +
              ",\"matches\":" + (matches ? "true" : "false") +
              ",\"instructions\":" + std::to_string(used) + "}");
}

#ifdef PTC_PLAYGROUND
// Test builds only: search a grid of shots from a spot and return the one that
// ends nearest the cup (or holes), to check that every day's hole can be won.
static IC_HttpResponse api_test_golf_best(const std::string &body) {
  std::istringstream in(body);
  uint32_t day;
  float x, y;
  if (!(in >> day >> x >> y)) return error(400, "send <day> <x> <y>");
  GolfCourse c = golf_course(day);
  if (y > 999) x = c.tee.x, y = c.tee.y; // "the tee"
  float best = 1e9f, ba = 0, bp = 0;
  ShotResult pick;
  for (float a = 2.5f; a < 178; a += 2.5f)
    for (float p = 4; p <= 100; p += 4) {
      ShotResult r = golf_shoot(c, {x, y}, a, p);
      float d = r.how == ShotEnd::Holed ? -1 : r.how == ShotEnd::Out ? 1e8f : std::fabs(r.end.x - c.cup_x);
      if (d < best) { best = d; pick = r; ba = a; bp = p; }
    }
  char buf[200];
  std::snprintf(buf, sizeof buf, "{\"angle\":%.1f,\"power\":%.1f,\"holed\":%s,\"x\":%.4f,\"y\":%.4f,\"cup_x\":%.2f,\"par\":%d",
                ba, bp, pick.how == ShotEnd::Holed ? "true" : "false", pick.end.x, pick.end.y, c.cup_x, c.par);
  return json(std::string(buf) + ",\"frames\":" + golf_frames_json(pick.frames) +
              ",\"course\":" + golf_course_json(c) + "}");
}

static IC_HttpResponse api_test_mg_best(const std::string &body) {
  std::istringstream in(body);
  uint32_t day;
  size_t stroke;
  float x, y, z, from, to;
  if (!(in >> day >> x >> y >> z >> stroke >> from >> to)) return error(400, "send <day> <x> <y> <z> <stroke> <yaw from> <yaw to>");
  MgCourse c = mg_course(day);
  float best = 1e9f, by = 0, bp = 0;
  MgShot pick;
  for (float yaw = from; yaw < to; yaw += 6)
    for (float p = 8; p <= 100; p += 8) {
      MgShot r = mg_shoot(c, {x, y, z}, yaw, p, mg_phase(day, stroke));
      float d = r.how == MgEnd::Holed ? -1 : r.how == MgEnd::Out ? 1e8f : std::hypot(r.end.x - c.cup_x(), r.end.z - c.cup_z());
      if (d < best) { best = d; pick = r; by = yaw; bp = p; }
    }
  char buf[220];
  std::snprintf(buf, sizeof buf, "{\"yaw\":%.1f,\"power\":%.1f,\"holed\":%s,\"dist\":%.3f,\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"par\":%d}",
                by, bp, pick.how == MgEnd::Holed ? "true" : "false", best, pick.end.x, pick.end.y, pick.end.z, c.par);
  return json(buf);
}
#endif

// ---------- link previews ----------
// What a shared link shows on X, WhatsApp and the like. Every page carries
// the same Open Graph tags (scripts/embed_ui.py); this fills them in.
struct Preview {
  std::string title, desc;
  const char *image; // "home", "chess" or "golf"
};

static std::string attr(const std::string &s) {
  std::string out;
  for (char c : s) {
    if (c == '&') out += "&amp;";
    else if (c == '"') out += "&quot;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else out += c;
  }
  return out;
}

static void replace_all(std::string &s, const std::string &from, const std::string &to) {
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
    s.replace(at, from.size(), to);
}

static IC_HttpResponse page(IC_API &ic_api, const char *html, const std::string &path,
                            const Preview &p) {
  std::string origin = "https://" + ic_api.get_canister_self().get_text() + ".icp0.io";
  std::string out = html;
  replace_all(out, "__OG_TITLE__", attr(p.title));
  replace_all(out, "__OG_DESC__", attr(p.desc));
  replace_all(out, "__OG_URL__", attr(origin + path));
  replace_all(out, "__OG_IMAGE__", origin + "/og/" + p.image + ".jpg?v=" + OG_V);
  return respond(200, "text/html; charset=utf-8", out);
}

static const char *CHESS_DESC =
    "Stockfish, the strongest open-source chess engine, runs inside an Internet Computer "
    "canister. 13 computers compute and agree on every move. Free, no wallet.";
static const char *GOLF_DESC =
    "One hole a day. Every shot is simulated by Box2D on 13 computers, so nobody can fake a "
    "score. Free, no wallet.";

static Preview chess_preview(uint64_t id) {
  Game g;
  if (!id || !game_load(id, g)) return {"Can you beat Stockfish? It runs on-chain.", CHESS_DESC, "chess"};
  size_t moves = (g.plies.size() + 1) / 2;
  return {"Game #" + std::to_string(id) + " vs Stockfish, " + std::to_string(moves) +
              (moves == 1 ? " move" : " moves") + ": replay it on-chain",
          "Every move was computed by 13 computers and stored on-chain. Replay it, then play your own.",
          "chess"};
}

static Preview golf_preview(uint64_t id) {
  GolfRound r;
  if (!id || !golf_round_load(id, r)) return {"Chain Golf: one hole a day. Can you sink it?", GOLF_DESC, "golf"};
  std::string who = r.name == "anon" ? "Someone" : r.name;
  int n = r.strokes();
  std::string title = r.holed() ? (n == 1 ? who + " made a hole-in-one on Chain Golf"
                                          : who + " holed today's Chain Golf in " + std::to_string(n))
                                 : who + "'s round on Chain Golf: " + std::to_string(n) + " strokes";
  return {title, "Every shot was simulated by Box2D on 13 computers. Watch the canister replay it, then beat it.",
          "golf"};
}

// A coroutine, because POST /api/fuel calls the ICP ledger and the CMC before
// it replies; every other route replies straight away.
static IC_Async http_request_update_async() {
  IC_API ic_api(CanisterUpdate{"http_request_update"}, false);
  IC_HttpUpdateRequest request;
  ic_api.from_wire(request);

  // Link-preview crawlers check pages and images with HEAD first; answer it
  // like GET.
  const std::string method = request.method == "HEAD" ? "GET" : request.method;
  std::string path = path_of(request.url);
  std::string body(request.body.begin(), request.body.end());
  uint64_t id{0};
  std::string rest;

  if (method == "POST" && path == "/api/fuel") {
    std::string out = co_await refuel(ic_api.get_canister_self());
    ic_api.to_wire(json(out));
    co_return;
  }

  IC_HttpResponse response = error(404, "not found");
  if (method == "GET" && path == "/") {
    response = page(ic_api, HOME_HTML, path,
                    {"Play the Chain: games that run inside a smart contract",
                     "Stockfish chess and Box2D golf in one Internet Computer canister. Every move is "
                     "computed by 13 computers. Free, no wallet.", "home"});
  } else if (method == "GET" &&
             (path == "/chess" || (parse_id(path, "/game/", id, rest) && rest.empty()))) {
    response = page(ic_api, UI_HTML, path, chess_preview(path == "/chess" ? 0 : id));
  } else if (method == "GET" &&
             (path == "/golf" || (parse_id(path, "/golf/", id, rest) && rest.empty()))) {
    response = page(ic_api, GOLF_HTML, path, golf_preview(path == "/golf" ? 0 : id));
  } else if (method == "GET" &&
             (path == "/minigolf" || (parse_id(path, "/minigolf/", id, rest) && rest.empty()))) {
    response = page(ic_api, MINIGOLF_HTML, path,
                    {"Chain Mini-Golf: Bullet physics on-chain",
                     "A 3D mini-golf hole a day. Every putt is simulated by Bullet Physics inside an "
                     "Internet Computer canister.", "golf"});
  } else if (method == "GET" && (path == "/og/home.jpg" || path == "/og/chess.jpg" || path == "/og/golf.jpg")) {
    // Asked for as /og/<page>.jpg?v=<hash of the images>: safe to keep a year.
    const unsigned char *img = path == "/og/home.jpg" ? OG_HOME_JPG : path == "/og/chess.jpg" ? OG_CHESS_JPG : OG_GOLF_JPG;
    size_t len = path == "/og/home.jpg" ? sizeof OG_HOME_JPG : path == "/og/chess.jpg" ? sizeof OG_CHESS_JPG : sizeof OG_GOLF_JPG;
    response = respond(200, "image/jpeg", std::string(reinterpret_cast<const char *>(img), len),
                       "public, max-age=31536000, immutable");
  } else if (method == "GET" && path == "/robots.txt") {
    response = respond(200, "text/plain; charset=utf-8", "User-agent: *\nAllow: /\n",
                       "public, max-age=86400");
  } else if (method == "GET" && path == "/fabric.js") {
    // Asked for as /fabric.js?v=<hash of its contents>: safe to keep a year.
    response = respond(200, "text/javascript; charset=utf-8", FABRIC_JS,
                       "public, max-age=31536000, immutable");
  } else if (method == "GET" && path == "/fabric.css") {
    response = respond(200, "text/css; charset=utf-8", FABRIC_CSS,
                       "public, max-age=31536000, immutable");
  } else if (method == "GET" && path == "/three.module.min.js") {
#ifdef PTC_PLAYGROUND
    // Playground builds must stay under 2 MB, so they send the browser to the
    // same three.js release on unpkg.
    response.status_code = 302;
    response.headers = {{"Location", "https://unpkg.com/three@0.160.0/build/three.module.min.js"}};
    response.body.clear();
#else
    // A fixed release of three.js: browsers keep it for a year.
    response = respond(200, "text/javascript; charset=utf-8", THREE_JS,
                       "public, max-age=31536000, immutable");
#endif
#ifdef PTC_PLAYGROUND
  } else if (method == "POST" && path == "/api/test/golf-best") {
    response = api_test_golf_best(body);
  } else if (method == "POST" && path == "/api/test/mg-best") {
    response = api_test_mg_best(body);
#endif
  } else if (method == "GET" && path == "/api/minigolf/today") {
    response = api_mg_today();
  } else if (method == "POST" && path == "/api/minigolf/round") {
    response = api_mg_new(ic_api, body);
  } else if (parse_id(path, "/api/minigolf/round/", id, rest)) {
    uint64_t n{0};
    std::string tail;
    if (method == "POST" && rest == "/shot") response = api_mg_shot(ic_api, id, body);
    else if (method == "GET" && rest.empty()) response = api_mg_round(id);
    else if (method == "GET" && parse_id(rest, "/shot/", n, tail) && tail.empty())
      response = api_mg_replay(ic_api, id, static_cast<size_t>(n));
  } else if (method == "GET" && path == "/api/golf/today") {
    response = api_golf_today();
  } else if (method == "POST" && path == "/api/golf/round") {
    response = api_golf_new(ic_api, body);
  } else if (parse_id(path, "/api/golf/round/", id, rest)) {
    uint64_t n{0};
    std::string tail;
    if (method == "POST" && rest == "/shot") response = api_golf_shot(ic_api, id, body);
    else if (method == "GET" && rest.empty()) response = api_golf_round(id);
    else if (method == "GET" && parse_id(rest, "/shot/", n, tail) && tail.empty())
      response = api_golf_replay(ic_api, id, static_cast<size_t>(n));
  } else if (method == "GET" && path == "/games") {
    response = page(ic_api, GAMES_HTML, path,
                    {"Every game on Play the Chain, and what it cost",
                     "Every chess game and golf round played inside one Internet Computer canister, "
                     "with the compute each one used.", "home"});
  } else if (method == "GET" && path == "/api/games") {
    response = api_games(ic_api);
  } else if (method == "GET" && path == "/api/info") {
    response = api_info(ic_api);
  } else if (method == "GET" && parse_id(path, "/api/game/", id, rest) &&
             rest.empty()) {
    Game g;
    if (game_load(id, g)) response = json(game_json(g, false));
  } else if (method == "POST" && path == "/api/game") {
    response = api_new_game(ic_api, body);
  } else if (method == "POST" && parse_id(path, "/api/game/", id, rest) &&
             rest == "/move") {
    response = api_move(ic_api, id, body);
  }
  ic_api.to_wire(response);
}

void http_request_update() { http_request_update_async(); }
