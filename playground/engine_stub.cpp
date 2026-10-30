// Playground builds leave Stockfish out, to fit the playground's 2 MB module
// limit: the chess routes answer, but the engine never moves.
#include "engine.h"

void engine_init() {}
void engine_new_game() {}
bool engine_bestmove(const std::string &, int, uint64_t, EngineResult &) { return false; }
bool engine_bestmove_after(const std::vector<std::string> &, int, uint64_t, EngineResult &, int) { return false; }
std::string engine_random_move(const std::vector<std::string> &, uint64_t) { return ""; }
GameState engine_game_state(const std::vector<std::string> &) { return GameState{}; }
