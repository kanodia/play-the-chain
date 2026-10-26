#include "candid_reply.h"

#include <utility>

uint32_t idl_hash(const std::string &s) {
  uint32_t h = 0;
  for (unsigned char c : s) h = h * 223 + c;
  return h;
}

namespace {
struct CType {
  int64_t op = 0;
  std::vector<std::pair<uint32_t, int64_t>> fields; // record, variant
};

class Reader {
public:
  explicit Reader(const std::vector<std::byte> &b) : m_b(b) {}
  bool ok() const { return m_ok; }
  uint8_t byte() {
    if (m_i >= m_b.size()) { m_ok = false; return 0; }
    return static_cast<uint8_t>(m_b[m_i++]);
  }
  __uint128_t uleb() {
    __uint128_t r = 0;
    for (int shift = 0; m_ok; shift += 7) {
      uint8_t x = byte();
      if (shift < 128) r |= __uint128_t(x & 0x7f) << shift;
      if (!(x & 0x80)) break;
    }
    return r;
  }
  int64_t sleb() {
    int64_t r = 0;
    int shift = 0;
    uint8_t x = 0;
    do {
      x = byte();
      if (shift < 64) r |= int64_t(x & 0x7f) << shift;
      shift += 7;
    } while (m_ok && (x & 0x80));
    if (shift < 64 && (x & 0x40)) r |= -(int64_t(1) << shift);
    return r;
  }
  uint64_t u64le() {
    uint64_t r = 0;
    for (int k = 0; k < 8; ++k) r |= uint64_t(byte()) << (8 * k);
    return r;
  }

private:
  const std::vector<std::byte> &m_b;
  size_t m_i = 0;
  bool m_ok = true;
};
} // namespace


OkOrErr decode_ok_or_err(const std::vector<std::byte> &bytes,
                                const std::vector<std::string> &err_labels) {
  OkOrErr out;
  Reader r(bytes);
  if (r.byte() != 'D' || r.byte() != 'I' || r.byte() != 'D' || r.byte() != 'L')
    return out;
  std::vector<CType> types(static_cast<size_t>(r.uleb()));
  for (CType &t : types) {
    t.op = r.sleb();
    if (t.op == -20 || t.op == -21) { // record, variant
      size_t n = static_cast<size_t>(r.uleb());
      for (size_t k = 0; k < n && r.ok(); ++k) {
        uint32_t h = static_cast<uint32_t>(r.uleb());
        t.fields.push_back({h, r.sleb()});
      }
    } else if (t.op == -18 || t.op == -19) { // opt, vec
      r.sleb();
    } else {
      return out; // func, service: not in these replies
    }
    if (!r.ok()) return out;
  }
  if (r.uleb() != 1) return out;
  int64_t top = r.sleb();
  if (top < 0 || static_cast<size_t>(top) >= types.size() ||
      types[top].op != -21)
    return out;

  size_t idx = static_cast<size_t>(r.uleb());
  if (idx >= types[top].fields.size()) return out;
  auto [hash, type] = types[top].fields[idx];
  if (hash == idl_hash("Ok")) {
    if (type == -12) out.value = r.u64le(); // nat64
    else if (type == -4) out.value = r.uleb(); // nat
    else return out;
    out.ok = true;
  } else {
    out.err = "Err";
    if (type >= 0 && static_cast<size_t>(type) < types.size() &&
        types[type].op == -21) {
      size_t e = static_cast<size_t>(r.uleb());
      if (e < types[type].fields.size())
        for (const std::string &label : err_labels)
          if (idl_hash(label) == types[type].fields[e].first) out.err = label;
    }
  }
  out.decoded = r.ok();
  return out;
}
