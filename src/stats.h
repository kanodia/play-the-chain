#pragma once

#include <cstdint>
#include <string>

// On-chain analytics: daily counters, nothing about any one visitor (no IPs,
// no cookies, no ids). One file a day, /stats_<day>.txt, "<key> <count>" per
// line. Keys come from fixed lists, so the files stay small: "pv:golf",
// "src:x", "dev:mobile", "lang:en", "bot:twitter", "ev:golf_finish", ...

void stats_add(uint32_t day, const std::string &key, uint64_t n = 1);

// {"<day>":{"<key>":<count>,...},...} for the days from..to.
std::string stats_json(uint32_t from_day, uint32_t to_day);

// The events a page may report with POST /api/event.
bool stats_event_allowed(const std::string &name);
