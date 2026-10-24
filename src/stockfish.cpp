// Stockfish 11 as an IC canister. Every search runs on-chain, in one update
// message, and reports the wasm instructions it used.

#include "stockfish.h"

#include <cstdlib>
#include <string>

#include "engine.h"
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
// HTTP

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

static bool is_page(const std::string &method, const std::string &path) {
  return method == "GET" && (path == "/" || path == "/index.html");
}

static bool is_move(const std::string &method, const std::string &path) {
  return method == "POST" && path == "/move";
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

// GET / -> the board. POST /move, body "<depth>\n<fen>" -> JSON with the
// move and what it cost.
void http_request_update() {
  IC_API ic_api(CanisterUpdate{std::string(__func__)}, false);
  IC_HttpUpdateRequest request;
  ic_api.from_wire(request);

  std::string path = path_of(request.url);
  if (is_page(request.method, path)) {
    ic_api.to_wire(respond(200, "text/html; charset=utf-8", UI_HTML));
    return;
  }
  if (!is_move(request.method, path)) {
    ic_api.to_wire(respond(404, "text/plain", "Not found"));
    return;
  }

  std::string body(request.body.begin(), request.body.end());
  size_t nl = body.find('\n');
  int depth = nl == std::string::npos ? 0 : std::atoi(body.substr(0, nl).c_str());
  std::string fen = nl == std::string::npos ? "" : body.substr(nl + 1);

  EngineResult res;
  uint64_t used{0};
  if (depth < 1 || depth > MAX_DEPTH || !search(fen, depth, res, used) ||
      res.bestmove.empty()) {
    ic_api.to_wire(respond(400, "text/plain", "Send \"<depth 1-20>\\n<FEN>\""));
    return;
  }

  std::string json = "{\"bestmove\":\"" + res.bestmove +
                     "\",\"score_cp\":" + std::to_string(res.score_cp) +
                     ",\"depth\":" + std::to_string(res.depth) +
                     ",\"nodes\":" + std::to_string(res.nodes) +
                     ",\"instructions\":" + std::to_string(used) + "}";
  ic_api.to_wire(respond(200, "application/json", json));
}
