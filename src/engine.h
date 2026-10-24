#pragma once

#include <cstdint>
#include <string>

// A thin, thread-free facade over Stockfish 11, built with SF_NO_THREADS so a
// search runs synchronously inside the calling message.

struct EngineResult {
  std::string bestmove; // UCI notation, e.g. "e2e4"; empty if no legal move
  int score_cp = 0;     // centipawns from the side to move
  int depth = 0;        // completed search depth
  uint64_t nodes = 0;
};

// One-time setup: bitboards, bitbases, endgames, a small hash table.
void engine_init();

// Forget the hash and histories, as for a new game.
void engine_new_game();

// Search `fen` to a fixed `depth`. Returns false if the FEN is not a valid
// position.
bool engine_bestmove(const std::string &fen, int depth, EngineResult &out);
