/*
 * battle.h - Turn-based command battle system (spec section 3).
 *
 * A battle pits the whole live party (party[0] == player, plus recruited
 * allies) against one or more enemy actors. Each turn, every living
 * combatant (allies + enemies) is ordered by effective SPD (descending) and
 * acts once. Player-side combatants pick a command via the CLI menu
 * (attack / skill / item / defend / flee); enemies use a simple AI.
 *
 *   - Effective stats = base + equipment bonuses (runtime.c / spec 2.3a).
 *   - Damage = max(1, attacker ATK-or-skill.power - defender DEF); defending
 *     halves incoming damage that turn.
 *   - Skills consume MP and roll against `accuracy`; normal attacks always hit.
 *   - Victory grants each defeated enemy's `drop` items to the inventory.
 *   - Defeat (all party members at 0 HP) ends the game.
 */
#ifndef HTG_BATTLE_H
#define HTG_BATTLE_H

#include "runtime.h"

typedef enum {
    HTG_BATTLE_WIN = 0,   /* all enemies defeated */
    HTG_BATTLE_LOSE,      /* whole party knocked out -> game over */
    HTG_BATTLE_FLED       /* party escaped */
} HtgBattleResult;

/*
 * Fight `enemy_count` enemies (each an actor id). Mutates the party's live
 * HP/MP in `g` and adds dropped items to the inventory on victory. Returns
 * the outcome. Uses the shared UI helpers for all input.
 */
HtgBattleResult htg_battle_run(Game *g, const char *const *enemy_ids,
                               size_t enemy_count);

/* Convenience: a single-enemy encounter. */
HtgBattleResult htg_battle_encounter(Game *g, const char *enemy_id);

#endif /* HTG_BATTLE_H */
