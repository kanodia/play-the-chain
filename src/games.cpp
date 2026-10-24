#include "games.h"

#include <fstream>
#include <sstream>

#include "hash-library/sha256.h"
#include "ic_api.h"

static const char *COUNT_PATH = "/games_count.txt";

static std::string game_path(uint64_t id) {
  return "/game_" + std::to_string(id) + ".txt";
}

std::vector<std::string> Game::moves() const {
  std::vector<std::string> out;
  for (const Ply &p : plies) out.push_back(p.uci);
  return out;
}

bool Game::engine_to_move() const {
  bool white_to_move = plies.size() % 2 == 0;
  return white_to_move == (human == 'b');
}

uint64_t games_count() {
  std::ifstream file(COUNT_PATH);
  uint64_t n{0};
  if (file) file >> n;
  return n;
}

Game game_new(int depth, char human, uint64_t now_ns) {
  uint64_t id = games_count() + 1;
  {
    std::ofstream file(COUNT_PATH, std::ios::trunc);
    file << id;
  }

  // Hard to guess, not a cryptographic secret: it keeps strangers from
  // moving in your game, nothing more.
  SHA256 sha256;
  std::string seed = std::to_string(now_ns) + "/" + std::to_string(id) + "/" +
                     std::to_string(IC_API::performance_counter());
  Game game;
  game.id = id;
  game.key = sha256(seed).substr(0, 20);
  game.depth = depth;
  game.human = human;
  game.created_ns = now_ns;
  return game;
}

// Line 1: "<key> <depth> <human> <created_ns>"; then one ply per line:
// "<uci> <e|h> <time_ns> <instructions> <nodes> <depth> <score_cp> <quick>".
void game_save(const Game &game) {
  std::ofstream file(game_path(game.id), std::ios::trunc);
  file << game.key << ' ' << game.depth << ' ' << game.human << ' '
       << game.created_ns << '\n';
  for (const Ply &p : game.plies)
    file << p.uci << ' ' << (p.by_engine ? 'e' : 'h') << ' ' << p.time_ns
         << ' ' << p.instructions << ' ' << p.nodes << ' ' << p.depth << ' '
         << p.score_cp << ' ' << (p.quick ? 1 : 0) << '\n';
  file.close();
  if (!file) IC_API::trap("Failed to write " + game_path(game.id));
}

bool game_load(uint64_t id, Game &game) {
  std::ifstream file(game_path(id));
  if (!file) return false;

  game = Game{};
  game.id = id;
  file >> game.key >> game.depth >> game.human >> game.created_ns;
  std::string line;
  std::getline(file, line); // rest of the header line
  while (std::getline(file, line)) {
    std::istringstream in(line);
    Ply p;
    char by{0};
    int quick{0};
    if (!(in >> p.uci >> by >> p.time_ns >> p.instructions >> p.nodes >>
          p.depth >> p.score_cp))
      continue;
    in >> quick; // absent in games saved before the daily budget
    p.by_engine = by == 'e';
    p.quick = quick != 0;
    game.plies.push_back(p);
  }
  return true;
}
