#include "stats.h"

#include <fstream>
#include <map>

static std::string day_path(uint32_t day) { return "/stats_" + std::to_string(day) + ".txt"; }

static std::map<std::string, uint64_t> load(uint32_t day) {
  std::map<std::string, uint64_t> m;
  std::ifstream file(day_path(day));
  std::string key;
  uint64_t n;
  while (file >> key >> n) m[key] = n;
  return m;
}

// Today's counters, kept in memory between messages and written through on
// every change; read back from the file after an upgrade.
static uint32_t g_day = 0;
static std::map<std::string, uint64_t> g_today;

void stats_add(uint32_t day, const std::string &key, uint64_t n) {
  if (day != g_day) {
    g_today = load(day);
    g_day = day;
  }
  g_today[key] += n;
  std::ofstream file(day_path(day), std::ios::trunc);
  for (const auto &[k, v] : g_today) file << k << ' ' << v << '\n';
}

std::string stats_json(uint32_t from_day, uint32_t to_day) {
  std::string out = "{";
  for (uint32_t day = from_day; day <= to_day; ++day) {
    std::map<std::string, uint64_t> m = day == g_day ? g_today : load(day);
    if (m.empty()) continue;
    if (out.size() > 1) out += ",";
    out += "\"" + std::to_string(day) + "\":{";
    bool first = true;
    for (const auto &[k, v] : m) {
      out += std::string(first ? "" : ",") + "\"" + k + "\":" + std::to_string(v);
      first = false;
    }
    out += "}";
  }
  return out + "}";
}

bool stats_event_allowed(const std::string &name) {
  static const char *EVENTS[] = {
      "visit_new",  "visit_return", "chess_start", "chess_end",  "chess_win",
      "golf_start", "golf_finish",  "golf_holed",  "mg_start",   "mg_finish",
      "feed_post",  "feed_react",   "share",       "refuel_click", "refuel_done"};
  for (const char *e : EVENTS)
    if (name == e) return true;
  return false;
}
