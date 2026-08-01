/*
 * save.h - Save / load of live gameplay state (spec section 6).
 *
 * The save file is a *separate* human-readable JSON document (spec 6), distinct
 * from the .htgp/.htgb project. It captures only the mutable run state:
 *
 *   {
 *     "htg_save": 1,
 *     "project_title": "...",
 *     "current_room": "room_id",
 *     "inventory": ["item_a", "item_b"],
 *     "flags": { ... },        // snapshot of live flags
 *     "vars":  { ... },        // snapshot of live vars
 *     "party": [
 *       { "actor": "player_default", "hp": 90, "mp": 20,
 *         "weapon": "item_iron_sword", "armor": null, "accessory": null }
 *     ]
 *   }
 *
 * The static content (rooms/events/actor templates) always comes from the
 * loaded project; the save only replays state on top of it.
 */
#ifndef HTG_SAVE_H
#define HTG_SAVE_H

#include "runtime.h"

/* Default save file name used by the in-game menu. */
#define HTG_SAVE_DEFAULT "htg_save.json"

/*
 * Write the current game state to `path` as pretty JSON.
 * Returns 0 on success, non-zero on I/O/allocation failure.
 */
int htg_save_game(const Game *g, const char *path);

/*
 * Load state from `path` into an already-initialized Game `g` (its project
 * must already be loaded). Restores current room, inventory, flags, vars and
 * party (including per-member HP/MP and equipment). Returns 0 on success,
 * non-zero on failure (file missing / parse error / mismatched project).
 * On failure `g` is left unchanged as far as practical.
 */
int htg_load_game(Game *g, const char *path);

#endif /* HTG_SAVE_H */
