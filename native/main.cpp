// Native spike: run the thread-free Stockfish at increasing depths and time it.
// Build with `icpp build-native`, run with `icpp run-native`.

#include <chrono>
#include <cstdio>

#include "../src/engine.h"

int main() {
  const char *positions[] = {
      "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
      "r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3",
      "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
  };

  engine_init();
  for (const char *fen : positions) {
    std::printf("\n%s\n", fen);
    engine_new_game();
    for (int depth : {6, 10, 14, 18}) {
      EngineResult r;
      auto t0 = std::chrono::steady_clock::now();
      if (!engine_bestmove(fen, depth, r)) return 1;
      double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
      std::printf("  depth %2d  %-6s %5d cp  %9llu nodes  %8.1f ms\n", r.depth,
                  r.bestmove.c_str(), r.score_cp,
                  static_cast<unsigned long long>(r.nodes), ms);
    }
  }
  return 0;
}
