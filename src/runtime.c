/*
 * runtime.c - Shared live-state helpers (see runtime.h). Standard C only.
 *
 * Effective stats follow spec 2.3a / 3:
 *     effective = base + sum of equipped items' equip.<stat>_bonus
 */
#include "runtime.h"

#include <string.h>

HtgActor *htg_member_actor(Game *g, const HtgMember *m) {
    if (!g || !m || !m->actor_id) return NULL;
    return htg_find_actor(g->p, m->actor_id);
}

/* Add the bonus for one equipped slot (item id may be NULL) to *acc. */
static void add_equip_bonus(Game *g, const char *item_id, HtgStatKind stat,
                            int *acc) {
    if (!item_id) return;
    HtgItem *it = htg_find_item(g->p, item_id);
    if (!it || !it->equip.has_equip) return;
    switch (stat) {
        case HTG_STAT_ATK:   *acc += it->equip.atk_bonus; break;
        case HTG_STAT_DEF:   *acc += it->equip.def_bonus; break;
        case HTG_STAT_SPD:   *acc += it->equip.spd_bonus; break;
        case HTG_STAT_MAXHP: *acc += it->equip.hp_bonus;  break;
        case HTG_STAT_MAXMP: *acc += it->equip.mp_bonus;  break;
    }
}

int htg_member_stat(Game *g, const HtgMember *m, HtgStatKind stat) {
    HtgActor *a = htg_member_actor(g, m);
    if (!a) return 0;

    int base = 0;
    switch (stat) {
        case HTG_STAT_ATK:   base = a->atk; break;
        case HTG_STAT_DEF:   base = a->def; break;
        case HTG_STAT_SPD:   base = a->spd; break;
        case HTG_STAT_MAXHP: base = a->hp;  break;
        case HTG_STAT_MAXMP: base = a->mp;  break;
    }

    add_equip_bonus(g, m->equip_weapon,    stat, &base);
    add_equip_bonus(g, m->equip_armor,     stat, &base);
    add_equip_bonus(g, m->equip_accessory, stat, &base);
    return base;
}
