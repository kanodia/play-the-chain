#pragma once

#include "wasm_symbol.h"

void canister_init() WASM_SYMBOL_EXPORTED("canister_init");
void canister_post_upgrade() WASM_SYMBOL_EXPORTED("canister_post_upgrade");

void bestmove() WASM_SYMBOL_EXPORTED("canister_update bestmove");
void new_game() WASM_SYMBOL_EXPORTED("canister_update new_game");
