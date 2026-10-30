#include "minigolf.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "golf.h" // golf_clean_name
#include "hash-library/sha256.h"
#include "ic_api.h"

static const char *COUNT_PATH = "/mg_rounds_count.txt";

static std::string round_path(uint64_t id) {
  return "/mg_round_" + std::to_string(id) + ".txt";
}
static std::string day_path(uint32_t day) {
  return "/mg_day_" + std::to_string(day) + ".txt";
}

// Floats are stored by their bits, so a replay starts from exactly the same
// numbers.
static uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static float unbits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

MgPoint MgRound::ball(const MgCourse &course) const {
  for (auto it = shots.rbegin(); it != shots.rend(); ++it)
    if (it->how != MgEnd::Out) return it->end;
  return course.tee;
}

int MgRound::strokes() const {
  int n = 0;
  for (const MgStroke &s : shots) n += s.how == MgEnd::Out ? 2 : 1;
  return n;
}

bool MgRound::holed() const {
  return !shots.empty() && shots.back().how == MgEnd::Holed;
}

bool MgRound::finished() const { return holed() || shots.size() >= MG_MAX_SHOTS; }

static uint64_t rounds_count() {
  std::ifstream file(COUNT_PATH);
  uint64_t n{0};
  if (file) file >> n;
  return n;
}

static const char *VARIED_PATH = "/mg_varied_from.txt";

bool MgRound::varied() const {
  std::ifstream file(VARIED_PATH);
  uint64_t from{0};
  if (file) file >> from;
  return id >= (from ? from : 1); // no record: every round is new
}

void mg_mark_varied_from() {
  if (std::ifstream(VARIED_PATH)) return;
  std::ofstream file(VARIED_PATH, std::ios::trunc);
  file << rounds_count() + 1;
}

MgCourse mg_round_course(const MgRound &r) { return mg_course(r.day, r.level, r.varied()); }

MgRound mg_round_new(uint32_t day, const std::string &name, uint64_t now_ns, int level) {
  uint64_t id = rounds_count() + 1;
  {
    std::ofstream file(COUNT_PATH, std::ios::trunc);
    file << id;
  }
  {
    std::ofstream file(day_path(day), std::ios::app);
    file << id << '\n';
  }
  SHA256 sha256;
  std::string seed = "minigolf/" + std::to_string(now_ns) + "/" + std::to_string(id) + "/" +
                     std::to_string(IC_API::performance_counter());
  MgRound r;
  r.id = id;
  r.key = sha256(seed).substr(0, 20);
  r.day = day;
  r.created_ns = now_ns;
  r.name = golf_clean_name(name);
  r.level = level;
  return r;
}

// Line 1: "<key> <day> <created_ns> [#<level> ]<name...>", as in Chain Golf; then one stroke per line:
// "<yaw bits> <power bits> <end x> <end y> <end z bits> <r|h|o> <instructions>".
void mg_round_save(const MgRound &r) {
  std::ofstream file(round_path(r.id), std::ios::trunc);
  file << r.key << ' ' << r.day << ' ' << r.created_ns << ' ';
  if (r.level != MG_CLASSIC) file << '#' << r.level << ' ';
  file << r.name << '\n';
  for (const MgStroke &s : r.shots)
    file << bits(s.yaw) << ' ' << bits(s.power) << ' ' << bits(s.end.x) << ' '
         << bits(s.end.y) << ' ' << bits(s.end.z) << ' '
         << (s.how == MgEnd::Holed ? 'h' : s.how == MgEnd::Out ? 'o' : 'r') << ' '
         << s.instructions << '\n';
  file.close();
  if (!file) IC_API::trap("Failed to write " + round_path(r.id));
}

bool mg_round_load(uint64_t id, MgRound &r) {
  std::ifstream file(round_path(id));
  if (!file) return false;
  r = MgRound{};
  r.id = id;
  file >> r.key >> r.day >> r.created_ns;
  std::getline(file, r.name);
  if (!r.name.empty() && r.name.front() == ' ') r.name.erase(r.name.begin());
  r.level = golf_take_level(r.name);
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream in(line);
    uint32_t a, p, x, y, z;
    char how;
    MgStroke s;
    if (!(in >> a >> p >> x >> y >> z >> how >> s.instructions)) continue;
    s.yaw = unbits(a);
    s.power = unbits(p);
    s.end = {unbits(x), unbits(y), unbits(z)};
    s.how = how == 'h' ? MgEnd::Holed : how == 'o' ? MgEnd::Out : MgEnd::Rest;
    r.shots.push_back(s);
  }
  return true;
}

std::vector<uint64_t> mg_day_rounds(uint32_t day) {
  std::vector<uint64_t> ids;
  std::ifstream file(day_path(day));
  uint64_t id;
  while (file >> id) ids.push_back(id);
  return ids;
}

static const char *how_text(MgEnd how) {
  return how == MgEnd::Holed ? "holed" : how == MgEnd::Out ? "out" : "rest";
}

std::string mg_frames_json(const std::vector<MgPoint> &frames) {
  std::string out = "[";
  char buf[64];
  for (size_t i = 0; i < frames.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%s[%.3f,%.3f,%.3f]", i ? "," : "", frames[i].x, frames[i].y, frames[i].z);
    out += buf;
  }
  return out + "]";
}

std::string mg_round_json(const MgRound &r, const MgCourse &c, bool with_key) {
  char buf[220];
  MgPoint b = r.ball(c);
  std::string out = "{\"id\":" + std::to_string(r.id) + ",\"day\":" + std::to_string(r.day) +
                    ",\"level\":" + std::to_string(r.level) +
                    ",\"name\":\"" + r.name + "\",\"created_ns\":" + std::to_string(r.created_ns) +
                    ",\"strokes\":" + std::to_string(r.strokes()) +
                    ",\"status\":\"" + (r.holed() ? "holed" : r.finished() ? "gave_up" : "playing") + "\"";
  std::snprintf(buf, sizeof buf, ",\"ball\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"next_phase\":%.4f", b.x, b.y, b.z,
                mg_phase(r.day, r.shots.size()));
  out += buf;
  if (with_key) out += ",\"key\":\"" + r.key + "\"";
  out += ",\"shots\":[";
  for (size_t i = 0; i < r.shots.size(); ++i) {
    const MgStroke &s = r.shots[i];
    std::snprintf(buf, sizeof buf,
                  "%s{\"yaw\":%.2f,\"power\":%.2f,\"phase\":%.4f,\"end\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"how\":\"%s\",\"instructions\":%llu}",
                  i ? "," : "", s.yaw, s.power, mg_phase(r.day, i), s.end.x, s.end.y, s.end.z, how_text(s.how),
                  static_cast<unsigned long long>(s.instructions));
    out += buf;
  }
  return out + "]}";
}

// Today's best holed rounds: fewest strokes, then the earliest.
std::string mg_leaderboard_json(uint32_t day) {
  struct Entry { uint64_t id; std::string name; int strokes; uint64_t created_ns; int level; };
  std::vector<Entry> holed;
  size_t played = 0, aces = 0;
  for (uint64_t id : mg_day_rounds(day)) {
    MgRound r;
    if (!mg_round_load(id, r) || r.shots.empty()) continue;
    ++played;
    if (!r.holed()) continue;
    if (r.strokes() == 1) ++aces;
    holed.push_back({r.id, r.name, r.strokes(), r.created_ns, r.level});
  }
  std::sort(holed.begin(), holed.end(), [](const Entry &a, const Entry &b) {
    if (a.level != b.level) return a.level > b.level;
    return a.strokes != b.strokes ? a.strokes < b.strokes : a.created_ns < b.created_ns;
  });
  std::string out = "{\"played\":" + std::to_string(played) + ",\"holed\":" +
                    std::to_string(holed.size()) + ",\"aces\":" + std::to_string(aces) + ",\"top\":[";
  for (size_t i = 0; i < holed.size() && i < 20; ++i)
    out += std::string(i ? "," : "") + "{\"id\":" + std::to_string(holed[i].id) + ",\"name\":\"" +
           holed[i].name + "\",\"strokes\":" + std::to_string(holed[i].strokes) +
           ",\"level\":" + std::to_string(holed[i].level) + "}";
  return out + "]}";
}
