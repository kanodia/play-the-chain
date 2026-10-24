#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

// Search `fen` to a fixed `depth`, stopping early after `max_nodes` positions
// (0 = no cap). The cap bounds the instructions of one search: on the IC a
// node costs about 8-10k instructions. Returns false if the FEN is not valid.
bool engine_bestmove(const std::string &fen, int depth, uint64_t max_nodes,
                     EngineResult &out);

// Games are kept as their moves in UCI notation, played from the start
// position, so the engine sees the whole history (repetitions, 50-move rule).

enum class GameStatus { Playing, Checkmate, Stalemate, Draw };

struct GameState {
  bool legal = false; // false if a move in the list is illegal
  std::string fen;    // position after the moves
  GameStatus status = GameStatus::Playing;
};

GameState engine_game_state(const std::vector<std::string> &moves);

// Search the position after `moves`, as engine_bestmove does for a FEN.
bool engine_bestmove_after(const std::vector<std::string> &moves, int depth,
                           uint64_t max_nodes, EngineResult &out);
