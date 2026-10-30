# Play the Chain

**Games whose every move runs inside one Internet Computer canister, written in C++.**

▶ **https://ne5l7-xiaaa-aaaaj-qsm3q-cai.icp0.io**

Famous C++ engines, compiled to WebAssembly with
[icpp-pro](https://github.com/icppWorld/icpp-pro) and running on-chain. There is
no server: one canister serves the pages, runs the engines, referees every move
and stores every game. Each move or shot is computed by all 13 nodes of an
application subnet and agreed on by consensus.

| | Engine | Page |
|---|---|---|
| ♟ Chess against Stockfish | [Stockfish 11](https://stockfishchess.org), GPL-3 | `/chess`, `/game/<id>` |
| ⛳ Chain Golf, one hole a day | [Box2D 2.4.2](https://box2d.org), MIT | `/golf`, `/golf/<id>` |
| 🏌 Chain Mini-Golf, 3D (unlisted, in progress) | [Bullet 3.25](https://github.com/bulletphysics/bullet3), zlib | `/minigolf` |
| 📣 The feed: results players post | | `/feed` |
| 📊 Every game and what it cost | | `/games` |

## How it works

- **The canister is the referee.** The browser sends only a move, or an angle
  and a power. The canister checks it, computes the reply or simulates the
  shot, stores it, and returns the result to animate. Scores can't be faked:
  `GET /api/golf/round/<id>/shot/<n>` simulates any stored shot again and says
  whether it lands in exactly the same place (wasm floats are deterministic).
- **Stockfish without threads.** `SF_NO_THREADS` runs the search synchronously
  in the calling message (about 50 guarded lines in `src/sf`). A depth-18 move
  is ~12B instructions, inside the 40B per-message limit.
- **20 levels in every game**, unlocked one at a time by winning (beat
  Stockfish, or hole at or under par); progress is kept in the browser.
  - Chess: levels 1–9 add random legal moves (60% down to 5%, picked by a
    hash of the game and move number, so every node agrees), then Stockfish's
    Skill Level rises, and level 20 is full strength at depth 18. The page
    sends only the level; the canister picks the settings.
  - Golf and 3D: level 10 is the classic hole, bit for bit, so every older
    round still recomputes. Below it the cup is wider and holds faster balls,
    with an aim guide; above it the cup tightens, a headwind grows (golf), the
    windmill speeds up and bumpers guard the cup (3D). One daily board ranks
    the highest level first, then the fewest strokes.
  - `native/golf_test.cpp <level>` and `native/minigolf_test.cpp <level> <days>`
    check that a greedy player can hole every day at a level.
- **Storage:** every game, round and post is a small file in stable memory
  through ic-wasi-polyfill (`/game_<id>.txt`, `/golf_round_<id>.txt`,
  `/feed_<id>.txt`, …), so everything survives upgrades.
- **HTTP only.** Every route is upgraded to `http_request_update`, so replies
  go through consensus. Pages, three.js, the WebGL backdrop (`fabric.js`) and
  the link-preview images are all served by the canister.
- **Reverse gas with a daily budget.** Players need no wallet. Up to 80B
  instructions a day are full strength, then lighter searches until 100B, then
  HTTP 429 until 00:00 UTC. Engine moves also pause before the freezing
  threshold, so the canister never freezes.
- **A shared fuel tank.** Anyone can send ICP to the canister and press Refuel.
  The canister pays 10% to the developer's account (`DEV_ACCOUNT` in
  `src/fuel.cpp`, said on the page), sends the rest to the cycles minting
  canister and calls `notify_top_up`. The ledger arguments are built as raw
  Candid by `scripts/fuel_args.py`.
- **The feed.** Finished games and rounds are posted automatically unless the
  player unticks "Post my result". Only the browser holding a round's or
  game's key can post it, once. A post stores only which result it is: the headline is written from
  the canister's own record when the feed is read.

## Repository layout

```
src/stockfish.cpp      canister entry points, HTTP routes, APIs, feed, link previews
src/engine.*           Stockfish wrapper: search, game state
src/games.*            chess games in stable memory
src/golf*.{h,cpp}      Chain Golf: course generation, Box2D simulation, rounds
src/minigolf*.{h,cpp}  3D mini-golf on Bullet
src/fuel.* candid_reply.*  the fuel tank: ledger + CMC calls, reply parsing
src/feed.*             feed storage
src/ui/*.html          the pages; fabric.js/.css: the shared WebGL backdrop
src/ui/og/*.jpg        link-preview cards (1200x630)
src/ui_html.h          GENERATED from src/ui by scripts/embed_ui.py
src/sf src/box2d src/bullet   vendored engines
native/                native builds and solvability tests for the golf holes
playground/            a smaller test build (no Stockfish, extra /api/test/* routes)
icp-local/             local test network with both builds, on port 8000
scripts/embed_ui.py    embeds the pages, scripts and images into src/ui_html.h
scripts/fuel_args.py   Candid bytes for the ledger calls in src/fuel.cpp
```

## Build and run locally

You need Python 3.11+, [icp-cli](https://github.com/dfinity/icp-cli) 1.7+
(`brew install icp-cli`), and icpp-pro, which installs wasi-sdk for you.

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install icpp-pro==7.0.0rc4

python3 -I scripts/embed_ui.py      # after any change in src/ui
icpp build-wasm                     # -> build/stockfish.wasm

cd icp-local
icp network start -d
icp deploy stockfish                # http://stockfish.local.localhost:8000/
```

The local network runs the same wasm as mainnet. It includes an ICP ledger, so
you can check hand-built Candid against a real one:
`icp canister call ryjl3-tyaaa-aaaaa-aaaba-cai transfer <hex> --args-format hex`.
Refuel itself only works on mainnet: the fuel accounts are set for the mainnet
canister. dfx 0.30–0.33's local replica can't run this wasm (it imports newer
system calls); use icp-cli.

Optional:

```bash
icp deploy pg                       # test build, after building playground/:
                                    #   cd playground && icpp build-wasm
icpp build-native && icpp run-native  # the engine natively, timed
```

## API

| Route | |
|---|---|
| `GET /api/info` | canister, subnet, node count, today's budget, fuel tank |
| `GET /api/games` | every chess game and its cost |
| `POST /api/game` | body `L<level 1-20>\n<w\|b>` (or `<depth>\n<w\|b>[\n<skill>\n<blunder %>]`): a new game and its key |
| `GET /api/game/<id>` | a game, with a receipt for every engine move |
| `POST /api/game/<id>/move` | body `<key>\n<uci move>`: your move and the reply |
| `GET /api/golf/today?level=<1-20>` | today's hole at a level, and the day's board |
| `POST /api/golf/round` | body `<name>[\n<level 1-20>]`: a new round and its key |
| `POST /api/golf/round/<id>/shot` | body `<key>\n<angle>\n<power>`: a shot and its path |
| `GET /api/golf/round/<id>[/shot/<n>]` | a round; or shot n simulated again, with `matches` |
| `/api/minigolf/...` | the same shape as golf |
| `GET /api/feed?before=<id>` | the 30 newest posts |
| `POST /api/feed` | body `<golf\|chess>\n<id>\n<key>\n<name>`: post your result |
| `POST /api/feed/<id>/react` | body `0`, `1` or `2` |
| `POST /api/fuel` | turn ICP on the canister's account into cycles |

The Candid method `bestmove : (fen : text, depth : nat8) -> (SearchResult)`
searches any position, under the same daily budget.

## Contributing and deploying

Fork, branch, and open a pull request; describe what you changed and how you
tested it locally. Only the maintainer's keys control the mainnet canister, so
deploys are done by the maintainer after review:

```bash
icpp build-wasm
dfx canister install stockfish --network ic --mode upgrade --wasm build/stockfish.wasm --identity <controller>
```

Upgrades must keep existing data readable: games, rounds and posts are plain
text files, so add new fields at the end of a line and give them defaults
when they're missing (see `game_load` and `fuel_log`).

## Verify the running code

```bash
icpp build-wasm                     # icpp-pro 7.0.0rc4
shasum -a 256 build/stockfish.wasm
dfx canister info ne5l7-xiaaa-aaaaj-qsm3q-cai --network ic   # module hash
```

The two hashes match when you build the commit that is deployed.

## Licence

GPL-3.0, like Stockfish. See `LICENSE`. This repository vendors Stockfish 11
(`src/sf`, GPL-3, with the changes described above), Box2D 2.4.2
(`src/box2d`, MIT), Bullet Physics 3.25 (`src/bullet`, zlib) and three.js
0.160.0 (`src/ui/vendor`, MIT), each under its own licence.
