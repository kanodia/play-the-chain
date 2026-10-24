// Stockfish 11 as an IC canister. Every search runs on-chain, in one update
// message, and reports the wasm instructions it used.

#include "stockfish.h"

#include <string>

#include "engine.h"
#include "ic_api.h"

static uint64_t instructions() {
  return IC_API::performance_counter(
      IC_API::PerformanceCounterType::Instructions);
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

  if (depth < 1 || depth > 30) IC_API::trap("depth must be 1..30");

  uint64_t start = instructions();
  EngineResult res;
  if (!engine_bestmove(fen, depth, res)) IC_API::trap("invalid FEN: " + fen);
  uint64_t used = instructions() - start;

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
