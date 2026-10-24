#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Games are stored one file each, /game_<id>.txt, through ic-wasi-polyfill's
// file system: the files live in stable memory and survive upgrades.

struct Ply {
  std::string uci;
  bool by_engine = false;
  uint64_t time_ns = 0; // IC time the move was recorded
  // Engine moves only: what the search used.
  uint64_t instructions = 0;
  uint64_t nodes = 0;
  int depth = 0;
  int score_cp = 0;
};

struct Game {
  uint64_t id = 0;
  std::string key; // needed to move; the page keeps it in the browser
  int depth = 14;
  char human = 'w'; // the side the human plays
  uint64_t created_ns = 0;
  std::vector<Ply> plies;

  std::vector<std::string> moves() const;
  bool engine_to_move() const;
};

// A new game with the next id. Not saved yet.
Game game_new(int depth, char human, uint64_t now_ns);

bool game_load(uint64_t id, Game &game);
void game_save(const Game &game);

// How many games were ever created.
uint64_t games_count();
