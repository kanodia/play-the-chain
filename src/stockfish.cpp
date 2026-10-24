// Stockfish 11 as an IC canister. Every search runs on-chain, in one update
// message, and reports the wasm instructions it used.

#include "stockfish.h"

#include <cstdlib>
#include <string>

#include "engine.h"
#include "games.h"
#include "ic_api.h"
#include "ui_html.h"

// A node costs about 8-10k instructions, so 2.5M nodes stays near 25B, well
// inside the 40B limit of one update message, whatever the position.
constexpr uint64_t MAX_NODES = 2'500'000;
constexpr int MAX_DEPTH = 20;

static uint64_t instructions() {
  return IC_API::performance_counter(
      IC_API::PerformanceCounterType::Instructions);
}

// Runs one capped search and measures it. Returns false on an invalid FEN.
static bool search(const std::string &fen, int depth, EngineResult &res,
                   uint64_t &used) {
  uint64_t start = instructions();
  bool ok = engine_bestmove(fen, depth, MAX_NODES, res);
  used = instructions() - start;
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
//   GET  /api/game/<id>         a game with a receipt for every engine move
//   POST /api/game              body "<depth>\n<w|b>"  -> new game
//   POST /api/game/<id>/move    body "<key>\n<uci>"    -> the engine replies

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
             ",\"score_cp\":" + std::to_string(p.score_cp);
    out += "}";
  }
  return out + "]}";
}

// The engine plays its move in `g`, measured, and records it.
static void engine_reply(Game &g) {
  EngineResult res;
  uint64_t start = instructions();
  engine_bestmove_after(g.moves(), g.depth, MAX_NODES, res);
  Ply p;
  p.uci = res.bestmove;
  p.by_engine = true;
  p.time_ns = IC_API::time();
  p.instructions = instructions() - start;
  p.nodes = res.nodes;
  p.depth = res.depth;
  p.score_cp = res.score_cp;
  g.plies.push_back(p);
}

static IC_HttpResponse api_info(IC_API &ic_api) {
  return json("{\"canister\":\"" + ic_api.get_canister_self().get_text() +
              "\",\"subnet\":\"" + ic_api.get_subnet_self().get_text() +
              "\",\"nodes\":" + std::to_string(ic_api.get_subnet_node_count()) +
              ",\"games\":" + std::to_string(games_count()) +
              ",\"engine\":\"Stockfish 11\",\"max_nodes\":" +
              std::to_string(MAX_NODES) + "}");
}

static IC_HttpResponse api_new_game(const std::string &body) {
  size_t nl = body.find('\n');
  int depth = std::atoi(body.substr(0, nl).c_str());
  char human = nl == std::string::npos || nl + 1 >= body.size() ? 'w' : body[nl + 1];
  if (depth < 1 || depth > MAX_DEPTH || (human != 'w' && human != 'b'))
    return error(400, "send <depth 1-20>\\n<w|b>");

  Game g = game_new(depth, human, IC_API::time());
  if (g.engine_to_move()) engine_reply(g);
  game_save(g);
  return json(game_json(g, true));
}

static IC_HttpResponse api_move(uint64_t id, const std::string &body) {
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

void http_request_update() {
  IC_API ic_api(CanisterUpdate{std::string(__func__)}, false);
  IC_HttpUpdateRequest request;
  ic_api.from_wire(request);

  const std::string &method = request.method;
  std::string path = path_of(request.url);
  std::string body(request.body.begin(), request.body.end());
  uint64_t id{0};
  std::string rest;

  IC_HttpResponse response = error(404, "not found");
  if (method == "GET" &&
      (path == "/" || (parse_id(path, "/game/", id, rest) && rest.empty()))) {
    response = respond(200, "text/html; charset=utf-8", UI_HTML);
  } else if (method == "GET" && path == "/api/info") {
    response = api_info(ic_api);
  } else if (method == "GET" && parse_id(path, "/api/game/", id, rest) &&
             rest.empty()) {
    Game g;
    if (game_load(id, g)) response = json(game_json(g, false));
  } else if (method == "POST" && path == "/api/game") {
    response = api_new_game(body);
  } else if (method == "POST" && parse_id(path, "/api/game/", id, rest) &&
             rest == "/move") {
    response = api_move(id, body);
  }
  ic_api.to_wire(response);
}
