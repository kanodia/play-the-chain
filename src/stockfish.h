#pragma once

#include "wasm_symbol.h"

void canister_init() WASM_SYMBOL_EXPORTED("canister_init");
void canister_post_upgrade() WASM_SYMBOL_EXPORTED("canister_post_upgrade");

void bestmove() WASM_SYMBOL_EXPORTED("canister_update bestmove");
void new_game() WASM_SYMBOL_EXPORTED("canister_update new_game");

// GET / serves the board; POST /move is upgraded to an update call that runs
// the search.
void http_request() WASM_SYMBOL_EXPORTED("canister_query http_request");
void http_request_update()
    WASM_SYMBOL_EXPORTED("canister_update http_request_update");
