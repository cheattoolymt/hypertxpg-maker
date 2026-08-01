/*
 * runtime.h - Shared live game-state model for `htg run` (spec sections 3-6).
 *
 * The exploration engine (engine.c), the battle system (battle.c) and the
 * save/load module (save.c) all operate on the same in-memory game state.
 * Keeping that state in one shared struct lets the three modules cooperate
 * without any of them owning the others.
 *
 * Design:
 *   - Flags/vars live in p->flags / p->vars (the project's owned JSON clones);
 *     gameplay reads/mutates them there so the on-disk project is never harmed.
 *   - HP/MP are *runtime* values: actor structs only carry base stats, so the
 *     party's current HP/MP is tracked here per member. Effective ATK/DEF/SPD/
 *     max-HP/max-MP are base + equipped-item bonuses, computed on demand.
 *   - The player's own equipped items are tracked in the party member entry
 *     for slot 0 (the player); recruited allies keep their actor's equipment.
 */
#ifndef HTG_RUNTIME_H
#define HTG_RUNTIME_H

#include "model.h"

#define HTG_MAX_INV     256
#define HTG_MAX_PARTY   16

/*
 * A live party member. `actor_id` borrows into the model; the member tracks
 * current HP/MP plus the three equipment slots (item ids, owned copies or
 * NULL). Slot 0 is always the player (meta.player_default_actor); recruited
 * allies follow in join order.
 */
typedef struct {
    char *actor_id;            /* owned copy of actor id */
    int   hp;                  /* current HP (runtime) */
    int   mp;                  /* current MP (runtime) */
    char *equip_weapon;        /* item id or NULL (owned) */
    char *equip_armor;         /* item id or NULL (owned) */
    char *equip_accessory;     /* item id or NULL (owned) */
} HtgMember;

typedef struct {
    HtgProject *p;
    const char *current_room;              /* room id (borrowed) */
    char       *inventory[HTG_MAX_INV];    /* owned copies of item ids */
    size_t      inv_count;
    HtgMember   party[HTG_MAX_PARTY];       /* party[0] == player */
    size_t      party_count;
    int         running;
} Game;

/* ---- effective-stat helpers (base + equipment bonuses; spec 2.3a/3) ---- */

/* Effective stat for a live member. `stat` is one of the HTG_STAT_* codes. */
typedef enum {
    HTG_STAT_ATK = 0,
    HTG_STAT_DEF,
    HTG_STAT_SPD,
    HTG_STAT_MAXHP,
    HTG_STAT_MAXMP
} HtgStatKind;

int htg_member_stat(Game *g, const HtgMember *m, HtgStatKind stat);

/* Look up the actor backing a member (borrowed, may be NULL). */
HtgActor *htg_member_actor(Game *g, const HtgMember *m);

#endif /* HTG_RUNTIME_H */
