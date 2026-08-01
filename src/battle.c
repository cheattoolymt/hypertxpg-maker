/*
 * battle.c - Turn-based command battle (spec section 3). Standard C only.
 *
 * Combatants:
 *   - Allies are the live party members in g->party (party[0] == player).
 *     Their current HP/MP live in the Game struct and persist after battle.
 *   - Enemies are spawned from actor templates into local Combatant records
 *     with their own HP/MP; they exist only for the duration of the fight.
 *
 * Turn model (spec 3):
 *   Each round, all living combatants (allies + enemies) are sorted by
 *   effective SPD descending and act once. Player-side combatants are driven
 *   by the CLI command menu; enemies pick a random living opponent.
 */
#include "battle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

/* A unified combatant view used only during a battle. */
typedef struct {
    int        is_ally;        /* 1 = party member, 0 = enemy */
    HtgMember *member;         /* ally: -> g->party[i]; enemy: NULL */
    /* enemy-only fields */
    char      *enemy_id;       /* actor id (owned) */
    int        enemy_hp;       /* enemy current HP */
    int        enemy_mp;       /* enemy current MP */
    int        defending;      /* halves incoming damage this round */
    int        alive;          /* cached for turn ordering */
} Combatant;

/* ---- small stat accessors that unify allies and enemies ---- */

static HtgActor *comb_actor(Game *g, Combatant *c) {
    if (c->is_ally) return htg_member_actor(g, c->member);
    return htg_find_actor(g->p, c->enemy_id);
}

static const char *comb_name(Game *g, Combatant *c) {
    HtgActor *a = comb_actor(g, c);
    if (a && a->name) return a->name;
    if (c->is_ally && c->member->actor_id) return c->member->actor_id;
    return c->enemy_id ? c->enemy_id : "?";
}

static int comb_hp(Combatant *c) {
    return c->is_ally ? c->member->hp : c->enemy_hp;
}
static void comb_set_hp(Combatant *c, int hp) {
    if (c->is_ally) c->member->hp = hp; else c->enemy_hp = hp;
}
static int comb_mp(Combatant *c) {
    return c->is_ally ? c->member->mp : c->enemy_mp;
}
static void comb_set_mp(Combatant *c, int mp) {
    if (c->is_ally) c->member->mp = mp; else c->enemy_mp = mp;
}

static int comb_stat(Game *g, Combatant *c, HtgStatKind stat) {
    if (c->is_ally) return htg_member_stat(g, c->member, stat);
    /* enemies use base stats (spec 2.3a: enemies normally carry no equip). */
    HtgActor *a = comb_actor(g, c);
    if (!a) return 0;
    switch (stat) {
        case HTG_STAT_ATK:   return a->atk;
        case HTG_STAT_DEF:   return a->def;
        case HTG_STAT_SPD:   return a->spd;
        case HTG_STAT_MAXHP: return a->hp;
        case HTG_STAT_MAXMP: return a->mp;
    }
    return 0;
}

static int comb_is_alive(Combatant *c) { return comb_hp(c) > 0; }

/* ---- item id copy helper ---- */
static char *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

/* ---- inventory helpers (mirror engine.c; kept local to avoid coupling) ---- */

static int inv_index(Game *g, const char *id) {
    for (size_t i = 0; i < g->inv_count; i++)
        if (strcmp(g->inventory[i], id) == 0) return (int)i;
    return -1;
}
static void inv_add(Game *g, const char *id) {
    if (g->inv_count >= HTG_MAX_INV) return;
    g->inventory[g->inv_count++] = dupstr(id);
}
static void inv_remove_at(Game *g, int idx) {
    if (idx < 0 || (size_t)idx >= g->inv_count) return;
    free(g->inventory[idx]);
    for (size_t j = (size_t)idx + 1; j < g->inv_count; j++)
        g->inventory[j - 1] = g->inventory[j];
    g->inv_count--;
}

/* ==========================================================================
 * Damage / actions
 * ========================================================================== */

static int compute_damage(int atk_power, int def, int defending) {
    int dmg = atk_power - def;
    if (dmg < 1) dmg = 1;
    if (defending) { dmg = dmg / 2; if (dmg < 1) dmg = 1; }
    return dmg;
}

static void apply_damage(Game *g, Combatant *target, int dmg) {
    int hp = comb_hp(target) - dmg;
    if (hp < 0) hp = 0;
    comb_set_hp(target, hp);
    printf("  %s に %d のダメージ! (残りHP %d)\n", comb_name(g, target), dmg, hp);
    if (hp == 0) printf("  %s は倒れた!\n", comb_name(g, target));
}

/* Count / list living combatants on the opposite side of `self`. */
static int list_targets(Game *g, Combatant *all, int n, int want_ally,
                        int *out_idx, const char **out_labels,
                        char label_store[][96]) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (!comb_is_alive(&all[i])) continue;
        if (all[i].is_ally != want_ally) continue;
        snprintf(label_store[m], 96, "%s (HP%d)", comb_name(g, &all[i]),
                 comb_hp(&all[i]));
        out_labels[m] = label_store[m];
        out_idx[m] = i;
        m++;
    }
    return m;
}

/* Choose a random living target on the given side. Returns index or -1. */
static int random_target(Game *g, Combatant *all, int n, int want_ally) {
    (void)g;
    int idx[HTG_MAX_PARTY + 32];
    int m = 0;
    for (int i = 0; i < n; i++)
        if (comb_is_alive(&all[i]) && all[i].is_ally == want_ally)
            idx[m++] = i;
    if (m == 0) return -1;
    return idx[rand() % m];
}

/* ---- a normal attack from attacker to target ---- */
static void do_attack(Game *g, Combatant *att, Combatant *tgt) {
    printf("%s の攻撃!\n", comb_name(g, att));
    int dmg = compute_damage(comb_stat(g, att, HTG_STAT_ATK),
                             comb_stat(g, tgt, HTG_STAT_DEF), tgt->defending);
    apply_damage(g, tgt, dmg);
}

/* ---- a skill from attacker to target; returns 1 if it was used ---- */
static int do_skill(Game *g, Combatant *att, Combatant *tgt, HtgSkill *sk) {
    if (comb_mp(att) < sk->mp_cost) {
        printf("MPが足りない!\n");
        return 0;
    }
    comb_set_mp(att, comb_mp(att) - sk->mp_cost);
    printf("%s は %s を使った!\n", comb_name(g, att), sk->name ? sk->name : "スキル");

    int acc = sk->accuracy > 0 ? sk->accuracy : 100;
    if (rand() % 100 >= acc) {
        printf("  しかし外れた!\n");
        return 1;
    }
    int power = sk->power > 0 ? sk->power : comb_stat(g, att, HTG_STAT_ATK);
    int dmg = compute_damage(power, comb_stat(g, tgt, HTG_STAT_DEF),
                             tgt->defending);
    apply_damage(g, tgt, dmg);
    return 1;
}

/* ==========================================================================
 * Player command input for one ally
 * ==========================================================================
 * Returns:  1 = acted, 0 = fled (whole party), -1 = cancelled back to menu.
 */
static int ally_turn(Game *g, Combatant *all, int n, Combatant *self) {
    HtgActor *a = comb_actor(g, self);
    for (;;) {
        char title[128];
        snprintf(title, sizeof(title), "%s の行動 (HP%d MP%d)",
                 comb_name(g, self), comb_hp(self), comb_mp(self));
        const char *cmds[] = { "攻撃", "スキル/魔法", "アイテム", "防御", "逃亡" };
        int c = ui_menu(title, cmds, 5);
        if (c == 0) c = 4; /* EOF -> defend, keep game moving */

        if (c == 1) { /* attack */
            int idx[HTG_MAX_PARTY + 32]; const char *labels[HTG_MAX_PARTY + 32];
            char store[HTG_MAX_PARTY + 32][96];
            int m = list_targets(g, all, n, 0, idx, labels, store);
            if (m == 0) return 1;
            int sel = ui_menu("攻撃対象", labels, m);
            if (sel < 1) continue;
            do_attack(g, self, &all[idx[sel - 1]]);
            return 1;
        }
        if (c == 2) { /* skill */
            if (!a || a->skill_count == 0) { printf("使えるスキルがない。\n"); continue; }
            const char *slabels[64]; char sstore[64][96];
            int sk_idx[64]; int sm = 0;
            for (size_t i = 0; i < a->skill_count && sm < 64; i++) {
                HtgSkill *sk = htg_find_skill(g->p, a->skills[i]);
                if (!sk) continue;
                snprintf(sstore[sm], 96, "%s (MP%d 威力%d)",
                         sk->name ? sk->name : a->skills[i], sk->mp_cost, sk->power);
                slabels[sm] = sstore[sm];
                sk_idx[sm] = (int)i;
                sm++;
            }
            if (sm == 0) { printf("使えるスキルがない。\n"); continue; }
            int ssel = ui_menu("スキル/魔法", slabels, sm);
            if (ssel < 1) continue;
            HtgSkill *sk = htg_find_skill(g->p, a->skills[sk_idx[ssel - 1]]);
            if (!sk) continue;
            if (comb_mp(self) < sk->mp_cost) { printf("MPが足りない!\n"); continue; }
            int idx[HTG_MAX_PARTY + 32]; const char *labels[HTG_MAX_PARTY + 32];
            char store[HTG_MAX_PARTY + 32][96];
            int m = list_targets(g, all, n, 0, idx, labels, store);
            if (m == 0) return 1;
            int tsel = ui_menu("対象", labels, m);
            if (tsel < 1) continue;
            if (do_skill(g, self, &all[idx[tsel - 1]], sk)) return 1;
            continue;
        }
        if (c == 3) { /* item */
            if (g->inv_count == 0) { printf("アイテムを持っていない。\n"); continue; }
            const char *labels[HTG_MAX_INV]; char store[64][96];
            int usable_idx[64]; int m = 0;
            for (size_t i = 0; i < g->inv_count && m < 64; i++) {
                HtgItem *it = htg_find_item(g->p, g->inventory[i]);
                if (!it || !it->usable) continue;
                snprintf(store[m], 96, "%s", it->name ? it->name : g->inventory[i]);
                labels[m] = store[m];
                usable_idx[m] = (int)i;
                m++;
            }
            if (m == 0) { printf("戦闘で使えるアイテムがない。\n"); continue; }
            int isel = ui_menu("アイテム", labels, m);
            if (isel < 1) continue;
            int inv_i = usable_idx[isel - 1];
            HtgItem *it = htg_find_item(g->p, g->inventory[inv_i]);
            /* Apply heal effects to self (spec 2.2 direct-effect shortcut). */
            if (it->effect.has_effect) {
                if (it->effect.type && strcmp(it->effect.type, "heal_hp") == 0) {
                    int max = comb_stat(g, self, HTG_STAT_MAXHP);
                    int hp = comb_hp(self) + it->effect.amount;
                    if (hp > max) hp = max;
                    comb_set_hp(self, hp);
                    printf("%s を使った。HPが %d 回復した。(HP%d)\n",
                           it->name, it->effect.amount, hp);
                } else if (it->effect.type && strcmp(it->effect.type, "heal_mp") == 0) {
                    int max = comb_stat(g, self, HTG_STAT_MAXMP);
                    int mp = comb_mp(self) + it->effect.amount;
                    if (mp > max) mp = max;
                    comb_set_mp(self, mp);
                    printf("%s を使った。MPが %d 回復した。(MP%d)\n",
                           it->name, it->effect.amount, mp);
                } else {
                    printf("%s を使った。(効果: %s %d)\n", it->name,
                           it->effect.type ? it->effect.type : "?", it->effect.amount);
                }
            } else {
                printf("%s を使った。\n", it->name);
            }
            if (it->consumable) inv_remove_at(g, inv_i);
            return 1;
        }
        if (c == 4) { /* defend */
            self->defending = 1;
            printf("%s は身を守っている。\n", comb_name(g, self));
            return 1;
        }
        if (c == 5) { /* flee - whole party */
            int chance = 50; /* base 50%; could scale by spd, kept simple */
            if (rand() % 100 < chance) { printf("パーティは逃げ出した!\n"); return 0; }
            printf("しかし回り込まれてしまった!\n");
            return 1;
        }
    }
}

/* Enemy AI: attack a random living ally. */
static void enemy_turn(Game *g, Combatant *all, int n, Combatant *self) {
    int t = random_target(g, all, n, 1);
    if (t < 0) return;
    HtgActor *a = comb_actor(g, self);
    /* Prefer a usable damaging skill if affordable (simple heuristic). */
    if (a && a->skill_count > 0 && (rand() % 2) == 0) {
        HtgSkill *sk = htg_find_skill(g->p, a->skills[rand() % a->skill_count]);
        if (sk && comb_mp(self) >= sk->mp_cost) {
            do_skill(g, self, &all[t], sk);
            return;
        }
    }
    do_attack(g, self, &all[t]);
}

/* ==========================================================================
 * Battle driver
 * ========================================================================== */

static int count_alive(Combatant *all, int n, int want_ally) {
    int c = 0;
    for (int i = 0; i < n; i++)
        if (all[i].is_ally == want_ally && comb_is_alive(&all[i])) c++;
    return c;
}

/* SPD-descending comparison via an index array (stable-ish, simple). */
static void order_by_speed(Game *g, Combatant *all, int n, int *order) {
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - 1 - i; j++) {
            int sa = comb_stat(g, &all[order[j]], HTG_STAT_SPD);
            int sb = comb_stat(g, &all[order[j + 1]], HTG_STAT_SPD);
            if (sb > sa) { int t = order[j]; order[j] = order[j + 1]; order[j + 1] = t; }
        }
    }
}

HtgBattleResult htg_battle_run(Game *g, const char *const *enemy_ids,
                               size_t enemy_count) {
    if (enemy_count == 0) return HTG_BATTLE_WIN;

    /* Assemble the combatant array: allies first, then enemies. */
    int n = 0;
    Combatant all[HTG_MAX_PARTY + 32];
    memset(all, 0, sizeof(all));

    for (size_t i = 0; i < g->party_count && n < HTG_MAX_PARTY + 32; i++) {
        all[n].is_ally = 1;
        all[n].member  = &g->party[i];
        n++;
    }
    int enemy_start = n;
    for (size_t i = 0; i < enemy_count && n < HTG_MAX_PARTY + 32; i++) {
        HtgActor *ea = htg_find_actor(g->p, enemy_ids[i]);
        if (!ea) continue;
        all[n].is_ally  = 0;
        all[n].enemy_id = dupstr(enemy_ids[i]);
        all[n].enemy_hp = ea->hp;
        all[n].enemy_mp = ea->mp;
        n++;
    }

    ui_header("戦闘開始!");
    printf("敵: ");
    for (int i = enemy_start; i < n; i++)
        printf("%s%s", comb_name(g, &all[i]), (i + 1 < n) ? " / " : "\n");

    HtgBattleResult result = HTG_BATTLE_WIN;
    int guard = 0;

    while (guard++ < 1000) {
        if (count_alive(all, n, 0) == 0) { result = HTG_BATTLE_WIN; break; }
        if (count_alive(all, n, 1) == 0) { result = HTG_BATTLE_LOSE; break; }

        /* Reset defend flags at the start of each round. */
        for (int i = 0; i < n; i++) all[i].defending = 0;

        int order[HTG_MAX_PARTY + 32];
        order_by_speed(g, all, n, order);

        int fled = 0;
        for (int oi = 0; oi < n; oi++) {
            Combatant *c = &all[order[oi]];
            if (!comb_is_alive(c)) continue;
            if (count_alive(all, n, 0) == 0 || count_alive(all, n, 1) == 0) break;

            if (c->is_ally) {
                int r = ally_turn(g, all, n, c);
                if (r == 0) { fled = 1; break; }
            } else {
                enemy_turn(g, all, n, c);
            }
        }
        if (fled) { result = HTG_BATTLE_FLED; break; }
    }

    /* ---- outcome handling ---- */
    if (result == HTG_BATTLE_WIN) {
        ui_header("勝利!");
        /* Grant drops from every enemy. */
        for (int i = enemy_start; i < n; i++) {
            HtgActor *ea = htg_find_actor(g->p, all[i].enemy_id);
            if (!ea) continue;
            for (size_t d = 0; d < ea->drop_count; d++) {
                if (inv_index(g, ea->drop[d]) < 0 ||
                    /* allow stacking duplicates of drops */ 1) {
                    inv_add(g, ea->drop[d]);
                    HtgItem *it = htg_find_item(g->p, ea->drop[d]);
                    printf("* %s を手に入れた!\n", it && it->name ? it->name : ea->drop[d]);
                }
            }
        }
    } else if (result == HTG_BATTLE_FLED) {
        ui_header("逃走成功");
    } else {
        ui_header("敗北...");
    }

    for (int i = enemy_start; i < n; i++) free(all[i].enemy_id);
    return result;
}

HtgBattleResult htg_battle_encounter(Game *g, const char *enemy_id) {
    const char *ids[1] = { enemy_id };
    return htg_battle_run(g, ids, 1);
}
