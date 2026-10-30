#include "feed.h"

#include <fstream>

#include "ic_api.h"

static const char *COUNT_PATH = "/feed_count.txt";

static std::string post_path(uint64_t id) { return "/feed_" + std::to_string(id) + ".txt"; }
static std::string ref_path(const std::string &kind, uint64_t ref) {
  return "/feed_ref_" + kind + "_" + std::to_string(ref) + ".txt";
}

uint64_t feed_count() {
  std::ifstream file(COUNT_PATH);
  uint64_t n{0};
  if (file) file >> n;
  return n;
}

// Line 1: "<kind> <ref> <time_ns> <fire> <clap> <wow>"; line 2: the name.
void feed_save(const FeedPost &p) {
  std::ofstream file(post_path(p.id), std::ios::trunc);
  file << p.kind << ' ' << p.ref << ' ' << p.time_ns;
  for (uint32_t r : p.react) file << ' ' << r;
  file << '\n' << p.name << '\n';
  file.close();
  if (!file) IC_API::trap("Failed to write " + post_path(p.id));
}

bool feed_load(uint64_t id, FeedPost &p) {
  std::ifstream file(post_path(id));
  if (!file) return false;
  p = FeedPost{};
  p.id = id;
  file >> p.kind >> p.ref >> p.time_ns;
  for (uint32_t &r : p.react) file >> r;
  file.ignore(1);
  std::getline(file, p.name);
  return !p.kind.empty();
}

uint64_t feed_posted(const std::string &kind, uint64_t ref) {
  std::ifstream file(ref_path(kind, ref));
  uint64_t id{0};
  if (file) file >> id;
  return id;
}

FeedPost feed_add(const std::string &kind, uint64_t ref, const std::string &name,
                  uint64_t now_ns) {
  FeedPost p;
  p.id = feed_count() + 1;
  p.kind = kind;
  p.ref = ref;
  p.time_ns = now_ns;
  p.name = name;
  feed_save(p);
  {
    std::ofstream file(COUNT_PATH, std::ios::trunc);
    file << p.id;
  }
  std::ofstream file(ref_path(kind, ref), std::ios::trunc);
  file << p.id;
  return p;
}
