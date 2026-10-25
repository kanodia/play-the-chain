# Play the Chain

**Play chess against Stockfish running inside an Internet Computer canister.**

▶ **https://ne5l7-xiaaa-aaaaj-qsm3q-cai.icp0.io**

[Stockfish](https://stockfishchess.org) is the strongest open-source chess
engine, and has been the top computer chess engine for years. Grandmasters
use it to analyse their games. It's over 12,000 lines of performance-tuned
C++, and here the whole engine runs inside a smart contract.

There is no server. One C++ canister serves the page, referees the game and
thinks of its moves. Every engine move is computed by all 13 nodes of an
application subnet, agreed on by consensus, and written to the canister's
stable memory, so every game has a permanent link anyone can replay.

The page has an **on-chain view** that explains each move twice, side by
side: in plain words, and as what actually runs.

## How it works

- **Stockfish 11** (the last version before neural nets), compiled from C++
  to wasm with [icpp-pro](https://github.com/icppWorld/icpp-pro) 7.0.0rc4.
- The IC has no threads, so `SF_NO_THREADS` makes the search run
  synchronously in the calling message: about 50 guarded lines in
  `thread.h/.cpp`, `tt.cpp`, `misc.cpp` and `syzygy/tbprobe.cpp`.
- A search costs about 8–10k wasm instructions per position. Depth 14 takes
  ~2B instructions, depth 18 ~12–14B, all inside the 40B limit of one
  message. Every search is capped at 2.5M positions (~25B instructions).
- The search is deterministic: the same position gives the same move and the
  same node count on a laptop, a local replica and mainnet.
- **The canister is the referee.** It checks each move with Stockfish's own
  move generator and rules on checkmate, stalemate, threefold repetition and
  the 50-move rule. Games are stored as files in stable memory
  (ic-wasi-polyfill), so they survive upgrades.
- **HTTP only, no agent library.** Every route is upgraded to
  `http_request_update`. Update replies go through consensus and are signed
  by the subnet, so the gateway accepts them without response certification.
- **Reverse gas, with a daily allowance.** Players need no wallet; the
  canister pays. Up to 200B instructions a day (~$0.10) moves are full
  strength, then quick depth-8 replies until 250B, then HTTP 429 until
  00:00 UTC.

## API

| Route | |
|---|---|
| `GET /`, `GET /game/<id>` | the page |
| `GET /api/info` | canister, subnet, node count, today's budget |
| `GET /api/game/<id>` | a game, with a receipt for every engine move |
| `POST /api/game` | body `<depth 1-20>\n<w\|b>`: a new game and its key |
| `POST /api/game/<id>/move` | body `<key>\n<uci move>`: your move and the reply |

The Candid method `bestmove : (fen : text, depth : nat8) -> (SearchResult)`
searches any position, under the same daily budget.

## Verify the running code

```bash
icpp build-wasm                     # icpp-pro 7.0.0rc4, wasi-sdk 25
shasum -a 256 build/stockfish.wasm
dfx canister info ne5l7-xiaaa-aaaaj-qsm3q-cai --network ic   # module hash
```

The deployed module hash is
`fabfccc317f5dc9418f546cca2d25beae111d4f10ff3a7f2094ddb1d1082a631`.

## Build and run locally

```bash
pip install icpp-pro==7.0.0rc4
python3 -I scripts/embed_ui.py      # after editing src/ui/index.html
icpp build-wasm
icp network start -d && icp deploy  # then open the URL it prints
icpp build-native && icpp run-native  # the engine natively, timed
```

## Licence

GPL-3.0, like Stockfish. See `LICENSE`. Stockfish is © the Stockfish
authors; this repository vendors the Stockfish 11 source in `src/sf` with
the changes described above.
