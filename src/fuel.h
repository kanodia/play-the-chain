#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ic_api.h"
#include "ic_async.h"

// The shared fuel tank. Anyone sends ICP to the canister's own ledger account
// (from funnAI, the NNS app or an exchange); refuel() hands it to the cycles
// minting canister, which tops this canister up with cycles. No credits and no
// accounts: the cycles pay for everybody's moves.

// The canister's principal and ICP account, to send ICP to.
extern const char *FUEL_PRINCIPAL;
extern const char *FUEL_ACCOUNT;
// The developer's ICP account, which gets DEV_SHARE_PERCENT of each refuel.
extern const char *DEV_ACCOUNT;

// Below this many liquid cycles (spendable above the freezing threshold) the
// engine stops playing, so the canister never freezes and the page can still
// ask for fuel.
constexpr uint64_t FUEL_FLOOR = 20'000'000'000;

struct FuelEntry {
  uint64_t time_ns = 0;
  uint64_t e8s = 0;   // ICP converted, after the ledger fee
  uint64_t block = 0; // the ICP ledger block of the payment to the CMC
  __uint128_t cycles = 0;
  uint64_t dev_e8s = 0;   // the developer's share, after the ledger fee
  uint64_t dev_block = 0; // the ledger block of that payment
};

// Every refuel, oldest first. Kept in stable memory.
std::vector<FuelEntry> fuel_log();

// Records a developer share that was paid but not recorded, so the next
// refuel does not take a second one. Does nothing once any refuel is logged.
void fuel_note_paid_share(uint64_t block, uint64_t e8s);

// Looks for ICP on the canister's account and turns it into cycles. Returns
// the outcome as a JSON object.
IC_Task<std::string> refuel(CandidTypePrincipal self);
