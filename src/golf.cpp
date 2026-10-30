#include "golf.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "hash-library/sha256.h"
#include "ic_api.h"

static const char *COUNT_PATH = "/golf_rounds_count.txt";

static std::string round_path(uint64_t id) {
  return "/golf_round_" + std::to_string(id) + ".txt";
}
static std::string day_path(uint32_t day) {
  return "/golf_day_" + std::to_string(day) + ".txt";
}

// Floats are stored by their bits, so a replay starts from exactly the same
// numbers.
static uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static float unbits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

GolfPoint GolfRound::ball(const GolfCourse &course) const {
  for (auto it = shots.rbegin(); it != shots.rend(); ++it)
    if (it->how != ShotEnd::Out) return it->end;
  return course.tee;
}

int GolfRound::strokes() const {
  int n = 0;
  for (const GolfShot &s : shots) n += s.how == ShotEnd::Out ? 2 : 1;
  return n;
}

bool GolfRound::holed() const {
  return !shots.empty() && shots.back().how == ShotEnd::Holed;
}

bool GolfRound::finished() const { return holed() || shots.size() >= GOLF_MAX_SHOTS; }

static const char *DRILLED_PATH = "/golf_drilled_from.txt";
static const char *VARIED_PATH = "/golf_varied_from.txt";

// The first round id recorded in a marker file.
static uint64_t first_round(const char *path) {
  std::ifstream file(path);
  uint64_t id{0};
  if (file) file >> id;
  return id ? id : 1; // no record: every round is new
}

bool GolfRound::drilled() const { return id >= first_round(DRILLED_PATH); }
bool GolfRound::varied() const { return id >= first_round(VARIED_PATH); }

GolfCourse golf_round_course(const GolfRound &r) {
  return golf_course(r.day, r.drilled(), r.level, r.varied());
}

int golf_take_level(std::string &line) {
  if (line.size() < 2 || line[0] != '#') return GOLF_CLASSIC;
  size_t end = line.find(' ');
  int level = std::atoi(line.substr(1, end - 1).c_str());
  line.erase(0, end == std::string::npos ? line.size() : end + 1);
  return level >= 1 && level <= GOLF_MAX_LEVEL ? level : GOLF_CLASSIC;
}

static uint64_t rounds_count();

static void mark_from(const char *path) {
  if (std::ifstream(path)) return;
  std::ofstream file(path, std::ios::trunc);
  file << rounds_count() + 1;
}

void golf_mark_drilled_from() { mark_from(DRILLED_PATH); }

void golf_mark_varied_from() { mark_from(VARIED_PATH); }

std::string golf_clean_name(const std::string &raw) {
  std::string out;
  for (char c : raw) {
    if (out.size() >= 16) break;
    if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '.')
      out += c;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  return out.empty() ? "anon" : out;
}

static uint64_t rounds_count() {
  std::ifstream file(COUNT_PATH);
  uint64_t n{0};
  if (file) file >> n;
  return n;
}

GolfRound golf_round_new(uint32_t day, const std::string &name, uint64_t now_ns, int level) {
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
  std::string seed = "golf/" + std::to_string(now_ns) + "/" + std::to_string(id) + "/" +
                     std::to_string(IC_API::performance_counter());
  GolfRound r;
  r.id = id;
  r.key = sha256(seed).substr(0, 20);
  r.day = day;
  r.created_ns = now_ns;
  r.name = golf_clean_name(name);
  r.level = level;
  return r;
}

// Line 1: "<key> <day> <created_ns> [#<level> ]<name...>" (no level: Normal;
// names can't contain '#'); then one shot per line:
// "<angle bits> <power bits> <end x bits> <end y bits> <r|h|o> <instructions>".
void golf_round_save(const GolfRound &r) {
  std::ofstream file(round_path(r.id), std::ios::trunc);
  file << r.key << ' ' << r.day << ' ' << r.created_ns << ' ';
  if (r.level != GOLF_CLASSIC) file << '#' << r.level << ' ';
  file << r.name << '\n';
  for (const GolfShot &s : r.shots)
    file << bits(s.angle) << ' ' << bits(s.power) << ' ' << bits(s.end.x) << ' '
         << bits(s.end.y) << ' '
         << (s.how == ShotEnd::Holed ? 'h' : s.how == ShotEnd::Out ? 'o' : 'r') << ' '
         << s.instructions << '\n';
  file.close();
  if (!file) IC_API::trap("Failed to write " + round_path(r.id));
}

bool golf_round_load(uint64_t id, GolfRound &r) {
  std::ifstream file(round_path(id));
  if (!file) return false;
  r = GolfRound{};
  r.id = id;
  file >> r.key >> r.day >> r.created_ns;
  std::getline(file, r.name);
  if (!r.name.empty() && r.name.front() == ' ') r.name.erase(r.name.begin());
  r.level = golf_take_level(r.name);
  std::string line;
  while (std::getline(file, line)) {
    std::istringstream in(line);
    uint32_t a, p, x, y;
    char how;
    GolfShot s;
    if (!(in >> a >> p >> x >> y >> how >> s.instructions)) continue;
    s.angle = unbits(a);
    s.power = unbits(p);
    s.end = {unbits(x), unbits(y)};
    s.how = how == 'h' ? ShotEnd::Holed : how == 'o' ? ShotEnd::Out : ShotEnd::Rest;
    r.shots.push_back(s);
  }
  return true;
}

std::vector<uint64_t> golf_day_rounds(uint32_t day) {
  std::vector<uint64_t> ids;
  std::ifstream file(day_path(day));
  uint64_t id;
  while (file >> id) ids.push_back(id);
  return ids;
}

static const char *how_text(ShotEnd how) {
  return how == ShotEnd::Holed ? "holed" : how == ShotEnd::Out ? "out" : "rest";
}

std::string golf_frames_json(const std::vector<GolfPoint> &frames) {
  std::string out = "[";
  char buf[48];
  for (size_t i = 0; i < frames.size(); ++i) {
    std::snprintf(buf, sizeof buf, "%s[%.3f,%.3f]", i ? "," : "", frames[i].x, frames[i].y);
    out += buf;
  }
  return out + "]";
}

std::string golf_round_json(const GolfRound &r, const GolfCourse &c, bool with_key) {
  char buf[200];
  GolfPoint b = r.ball(c);
  std::string out = "{\"id\":" + std::to_string(r.id) + ",\"day\":" + std::to_string(r.day) +
                    ",\"level\":" + std::to_string(r.level) +
                    ",\"name\":\"" + r.name + "\",\"created_ns\":" + std::to_string(r.created_ns) +
                    ",\"strokes\":" + std::to_string(r.strokes()) +
                    ",\"status\":\"" + (r.holed() ? "holed" : r.finished() ? "gave_up" : "playing") + "\"";
  std::snprintf(buf, sizeof buf, ",\"ball\":{\"x\":%.3f,\"y\":%.3f}", b.x, b.y);
  out += buf;
  if (with_key) out += ",\"key\":\"" + r.key + "\"";
  out += ",\"shots\":[";
  for (size_t i = 0; i < r.shots.size(); ++i) {
    const GolfShot &s = r.shots[i];
    std::snprintf(buf, sizeof buf,
                  "%s{\"angle\":%.2f,\"power\":%.2f,\"end\":{\"x\":%.3f,\"y\":%.3f},\"how\":\"%s\",\"instructions\":%llu}",
                  i ? "," : "", s.angle, s.power, s.end.x, s.end.y, how_text(s.how),
                  static_cast<unsigned long long>(s.instructions));
    out += buf;
  }
  return out + "]}";
}

// Today's best holed rounds: fewest strokes, then the earliest.
std::string golf_leaderboard_json(uint32_t day) {
  struct Entry { uint64_t id; std::string name; int strokes; uint64_t created_ns; int level; };
  std::vector<Entry> holed;
  size_t played = 0, aces = 0;
  for (uint64_t id : golf_day_rounds(day)) {
    GolfRound r;
    if (!golf_round_load(id, r) || r.shots.empty()) continue;
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
