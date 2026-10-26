// Stockfish 11 as an IC canister. Every search runs on-chain, in one update
// message, and reports the wasm instructions it used.

#include "stockfish.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "engine.h"
#include "fuel.h"
#include "games.h"
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

static uint64_t g_day = 0, g_used_today = 0, g_games_today = 0;

static void roll_day() {
  uint64_t day = IC_API::time() / 86'400'000'000'000ULL;
  if (day != g_day) {
    g_day = day;
    g_used_today = 0;
    g_games_today = 0;
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
}

// Heap state is not kept across an upgrade, so set the engine up again.
void canister_post_upgrade() {
  IC_API ic_api(CanisterPostUpgrade{std::string(__func__)}, false);
  engine_init();
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
//   GET  /  and  /game/<id>     the page
//   GET  /api/info              canister, subnet, games played
//   GET  /games                 the dashboard of every game
//   GET  /api/games             one summary line per game, and the cycles balance
//   GET  /api/game/<id>         a game with a receipt for every engine move
//   POST /api/game              body "<depth>\n<w|b>"  -> new game
//   POST /api/game/<id>/move    body "<key>\n<uci>"    -> the engine replies
//   POST /api/fuel                                     -> turn ICP sent to the
//                                                         canister into cycles

static std::string path_of(const std::string &url) {
  return url.substr(0, url.find('?'));
}

static IC_HttpResponse respond(uint16_t status, const std::string &type,
                               const std::string &body) {
  IC_HttpResponse response;
  response.status_code = status;
  response.headers.push_back({"Content-Type", type});
  response.headers.push_back({"Content-Length", std::to_string(body.size())});
  response.headers.push_back({"Cache-Control", "no-store"});
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

static IC_HttpResponse api_info(IC_API &ic_api) {
  roll_day();
  return json("{\"canister\":\"" + ic_api.get_canister_self().get_text() +
              "\",\"subnet\":\"" + ic_api.get_subnet_self().get_text() +
              "\",\"nodes\":" + std::to_string(ic_api.get_subnet_node_count()) +
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
                    ",\"nodes\":" + std::to_string(ic_api.get_subnet_node_count()) +
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

// A coroutine, because POST /api/fuel calls the ICP ledger and the CMC before
// it replies; every other route replies straight away.
static IC_Async http_request_update_async() {
  IC_API ic_api(CanisterUpdate{"http_request_update"}, false);
  IC_HttpUpdateRequest request;
  ic_api.from_wire(request);

  const std::string &method = request.method;
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
  if (method == "GET" &&
      (path == "/" || (parse_id(path, "/game/", id, rest) && rest.empty()))) {
    response = respond(200, "text/html; charset=utf-8", UI_HTML);
  } else if (method == "GET" && path == "/games") {
    response = respond(200, "text/html; charset=utf-8", GAMES_HTML);
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
