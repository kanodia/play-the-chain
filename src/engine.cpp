#include "engine.h"

#include <deque>
#include <memory>

#include "sf/bitboard.h"
#include "sf/movegen.h"
#include "sf/endgame.h"
#include "sf/position.h"
#include "sf/search.h"
#include "sf/thread.h"
#include "sf/tt.h"
#include "sf/uci.h"

namespace PSQT {
void init();
}

static const char *UCI_StartFEN =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

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

// Search the position set up in `pos`; `states` holds its history.
static void search(Position &pos, StateListPtr &states, int depth,
                   uint64_t max_nodes, EngineResult &out) {
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
}

bool engine_bestmove(const std::string &fen, int depth, uint64_t max_nodes,
                     EngineResult &out) {
  StateListPtr states(new std::deque<StateInfo>(1));
  Position pos;
  pos.set(fen, false, &states->back(), Threads.main());
  if (!pos.pos_is_ok()) return false;
  search(pos, states, depth, max_nodes, out);
  return true;
}

// Play `moves` from the start position. Stops with false at an illegal move.
static bool replay(const std::vector<std::string> &moves, Position &pos,
                   StateListPtr &states) {
  states = StateListPtr(new std::deque<StateInfo>(1));
  pos.set(UCI_StartFEN, false, &states->back(), Threads.main());
  for (std::string uci : moves) {
    Move m = UCI::to_move(pos, uci);
    if (m == MOVE_NONE) return false;
    states->emplace_back();
    pos.do_move(m, states->back());
  }
  return true;
}

GameState engine_game_state(const std::vector<std::string> &moves) {
  GameState st;
  StateListPtr states;
  Position pos;
  if (!replay(moves, pos, states)) return st;

  st.legal = true;
  st.fen = pos.fen();
  const StateInfo &si = states->back();
  if (MoveList<LEGAL>(pos).size() == 0)
    st.status = pos.checkers() ? GameStatus::Checkmate : GameStatus::Stalemate;
  else if (si.rule50 > 99 || si.repetition < 0) // 50 moves or threefold
    st.status = GameStatus::Draw;
  return st;
}

bool engine_bestmove_after(const std::vector<std::string> &moves, int depth,
                           uint64_t max_nodes, EngineResult &out) {
  StateListPtr states;
  Position pos;
  if (!replay(moves, pos, states)) return false;
  search(pos, states, depth, max_nodes, out);
  return true;
}
