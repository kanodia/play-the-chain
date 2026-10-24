#include "engine.h"

#include <deque>
#include <memory>

#include "sf/bitboard.h"
#include "sf/endgame.h"
#include "sf/position.h"
#include "sf/search.h"
#include "sf/thread.h"
#include "sf/tt.h"
#include "sf/uci.h"

namespace PSQT {
void init();
}

void engine_init() {
  UCI::init(Options); // Hash defaults to 16 MB
  PSQT::init();
  Bitboards::init();
  Position::init();
  Bitbases::init();
  Endgames::init();
  Threads.set(1);
  Search::clear();
}

void engine_new_game() { Search::clear(); }

bool engine_bestmove(const std::string &fen, int depth, uint64_t max_nodes,
                     EngineResult &out) {
  StateListPtr states(new std::deque<StateInfo>(1));
  Position pos;
  pos.set(fen, false, &states->back(), Threads.main());
  if (!pos.pos_is_ok()) return false;

  Search::LimitsType limits;
  limits.startTime = now();
  limits.depth = depth;
  limits.nodes = max_nodes;

  // With SF_NO_THREADS this returns after the search has finished.
  Threads.start_thinking(pos, states, limits);

  MainThread *th = Threads.main();
  out = EngineResult{};
  out.nodes = Threads.nodes_searched();
  out.depth = th->completedDepth;
  if (!th->rootMoves.empty() && !th->rootMoves[0].pv.empty()) {
    out.bestmove = UCI::move(th->rootMoves[0].pv[0], false);
    out.score_cp = th->rootMoves[0].score * 100 / PawnValueEg;
  }
  return true;
}
