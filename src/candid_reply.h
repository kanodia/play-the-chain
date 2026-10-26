#pragma once

// Just enough Candid decoding for a `variant { Ok : nat64 | nat; Err : variant
// { ... } }` reply: the ICP ledger's and the CMC's error types are too big to
// spell out, and only their labels matter to the fuel tank.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct OkOrErr {
  bool decoded = false;
  bool ok = false;
  __uint128_t value = 0; // Ok
  std::string err;       // Err: the label of the error, e.g. "Processing"
};

uint32_t idl_hash(const std::string &s);

// err_labels: the Err labels to recognise; others come back as "Err".
OkOrErr decode_ok_or_err(const std::vector<std::byte> &bytes,
                         const std::vector<std::string> &err_labels);
