/*
 * engine.c - `htg run` execution engine (spec sections 3-6). Standard C only.
 *
 * State model:
 *   - The live game state lives in the shared Game struct (runtime.h), shared
 *     with the battle (battle.c) and save/load (save.c) modules.
 *   - Flags/vars are kept in p->flags / p->vars (JSON objects). The engine
 *     reads and mutates these directly; because they are the model's owned
 *     clones, gameplay never corrupts the on-disk project.
 *   - The party is a list of live members (party[0] == player) each tracking
 *     current HP/MP and equipment; the current room and inventory are tracked
 *     in the same struct.
 *
 * This stage adds, on top of exploration/events:
 *   - Turn-based battle on encounters and a manual "戦う" hook (spec 3).
 *   - Effective stats from equipment, and equipping items from the menu.
 *   - Save / load of the run state to a separate JSON file (spec 6).
 *   - Game-over handling on party wipe.
 */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "json.h"
#include "ui.h"
#include "runtime.h"
#include "battle.h"
#include "save.h"

/* ==========================================================================
 * flag / var access on the live JSON state
 * ========================================================================== */

static int flag_get(Game *g, const char *name) {
    return json_object_get_bool(g->p->flags, name, 0);
}
static void flag_set(Game *g, const char *name, int value) {
    if (!g->p->flags) g->p->flags = json_new_object();
    json_object_set(g->p->flags, name, json_new_bool(value));
}
static double var_get(Game *g, const char *name) {
    return json_object_get_number(g->p->vars, name, 0);
}
static void var_set(Game *g, const char *name, double value) {
    if (!g->p->vars) g->p->vars = json_new_object();
    json_object_set(g->p->vars, name, json_new_number(value));
}

/* ==========================================================================
 * Condition evaluation (spec 2.5)
 * ==========================================================================
 * Grammar (no parentheses, left-to-right):
 *   cond  := term (( "&&" | "||" ) term)*
 *   term  := "flag:" name ("==" ("true"|"false"))?
 *          | "var:"  name op number
 *   op    := == != > < >= <=
 */

static const char *trim(const char *s, const char **end) {
    while (*s == ' ' || *s == '\t') s++;
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *end = e;
    return s;
}

static int eval_term(Game *g, const char *s, const char *e) {
    char buf[256];
    size_t n = (size_t)(e - s);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';

    if (strncmp(buf, "flag:", 5) == 0) {
        char *body = buf + 5;
        char *eq = strstr(body, "==");
        int want = 1;
        if (eq) {
            *eq = '\0';
            const char *rhs = eq + 2;
            while (*rhs == ' ') rhs++;
            want = (strncmp(rhs, "true", 4) == 0) ? 1 : 0;
        }
        char *name = body;
        while (*name == ' ') name++;
        char *nend = name + strlen(name);
        while (nend > name && nend[-1] == ' ') *--nend = '\0';
        return flag_get(g, name) == want;
    }

    if (strncmp(buf, "var:", 4) == 0) {
        char *body = buf + 4;
        static const char *ops[] = { ">=", "<=", "==", "!=", ">", "<" };
        for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
            char *op = strstr(body, ops[i]);
            if (op) {
                char name[128];
                size_t nn = (size_t)(op - body);
                if (nn >= sizeof(name)) nn = sizeof(name) - 1;
                memcpy(name, body, nn); name[nn] = '\0';
                char *ne = name + strlen(name);
                while (ne > name && ne[-1] == ' ') *--ne = '\0';
                char *ns = name; while (*ns == ' ') ns++;
                const char *rhs = op + strlen(ops[i]);
                while (*rhs == ' ') rhs++;
                double lv = var_get(g, ns);
                double rv = strtod(rhs, NULL);
                if (strcmp(ops[i], ">=") == 0) return lv >= rv;
                if (strcmp(ops[i], "<=") == 0) return lv <= rv;
                if (strcmp(ops[i], "==") == 0) return lv == rv;
                if (strcmp(ops[i], "!=") == 0) return lv != rv;
                if (strcmp(ops[i], ">")  == 0) return lv > rv;
                if (strcmp(ops[i], "<")  == 0) return lv < rv;
            }
        }
        char *ns = body; while (*ns == ' ') ns++;
        return var_get(g, ns) != 0;
    }

    return 1;
}

int htg_eval_condition(HtgProject *p, const char *cond) {
    if (!cond || !*cond) return 1;
    Game tmp; memset(&tmp, 0, sizeof(tmp)); tmp.p = p;

    const char *s = cond;
    int acc = 0;
    int have_acc = 0;
    int pending_or = 0, pending_and = 0;

    while (*s) {
        const char *amp = strstr(s, "&&");
        const char *bar = strstr(s, "||");
        const char *conn = NULL;
        int is_and = 0;
        if (amp && (!bar || amp < bar)) { conn = amp; is_and = 1; }
        else if (bar)                   { conn = bar; is_and = 0; }

        const char *term_end = conn ? conn : s + strlen(s);
        const char *te;
        const char *ts = trim(s, &te);
        if (term_end < te) te = term_end;
        int v = eval_term(&tmp, ts, te);

        if (!have_acc) { acc = v; have_acc = 1; }
        else if (pending_and) acc = acc && v;
        else if (pending_or)  acc = acc || v;

        pending_and = pending_or = 0;
        if (!conn) break;
        if (is_and) pending_and = 1; else pending_or = 1;
        s = conn + 2;
    }
    return have_acc ? acc : 1;
}

/* ==========================================================================
 * Inventory / party helpers
 * ========================================================================== */

static char *dup_str(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

static int inv_has(Game *g, const char *id) {
    for (size_t i = 0; i < g->inv_count; i++)
        if (strcmp(g->inventory[i], id) == 0) return 1;
    return 0;
}
static void inv_add(Game *g, const char *id) {
    if (g->inv_count >= HTG_MAX_INV) return;
    g->inventory[g->inv_count++] = dup_str(id);
}
static void inv_remove_one(Game *g, const char *id) {
    for (size_t i = 0; i < g->inv_count; i++) {
        if (strcmp(g->inventory[i], id) == 0) {
            free(g->inventory[i]);
            for (size_t j = i + 1; j < g->inv_count; j++)
                g->inventory[j - 1] = g->inventory[j];
            g->inv_count--;
            return;
        }
    }
}

static int party_has(Game *g, const char *id) {
    for (size_t i = 0; i < g->party_count; i++)
        if (g->party[i].actor_id && strcmp(g->party[i].actor_id, id) == 0) return 1;
    return 0;
}

/* Add a live party member from an actor template, seeding HP/MP from the
 * actor's effective max values (base + any equipment it declares). */
static void party_add_member(Game *g, const char *actor_id) {
    int cap = g->p->meta.max_party_size > 0 ? g->p->meta.max_party_size : HTG_MAX_PARTY;
    if ((int)g->party_count >= cap) {
        printf("(パーティが満員のため '%s' は加入できませんでした)\n", actor_id);
        return;
    }
    if (party_has(g, actor_id)) return;

    HtgActor *a = htg_find_actor(g->p, actor_id);
    if (!a) { printf("(未定義のアクター '%s')\n", actor_id); return; }

    HtgMember *m = &g->party[g->party_count++];
    memset(m, 0, sizeof(*m));
    m->actor_id = dup_str(actor_id);
    /* Seed the actor's declared equipment so bonuses apply from the start. */
    m->equip_weapon    = dup_str(a->equip_weapon);
    m->equip_armor     = dup_str(a->equip_armor);
    m->equip_accessory = dup_str(a->equip_accessory);
    m->hp = htg_member_stat(g, m, HTG_STAT_MAXHP);
    m->mp = htg_member_stat(g, m, HTG_STAT_MAXMP);
}

static void party_join(Game *g, const char *actor_id) {
    if (party_has(g, actor_id)) return;
    size_t before = g->party_count;
    party_add_member(g, actor_id);
    if (g->party_count > before) {
        HtgActor *a = htg_find_actor(g->p, actor_id);
        printf("* %s がパーティに加わった!\n", a && a->name ? a->name : actor_id);
    }
}

/* True while at least one party member has HP > 0. */
static int party_alive(Game *g) {
    for (size_t i = 0; i < g->party_count; i++)
        if (g->party[i].hp > 0) return 1;
    return 0;
}

/* ==========================================================================
 * Event playback (spec 2.5, 4)
 * ========================================================================== */

static void apply_set_var(Game *g, const char *expr) {
    if (!expr) return;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", expr);
    char *eq = strchr(buf, '=');
    if (!eq) return;
    int add = 0, sub = 0;
    if (eq > buf && eq[-1] == '+') { add = 1; eq[-1] = '\0'; }
    else if (eq > buf && eq[-1] == '-') { sub = 1; eq[-1] = '\0'; }
    else *eq = '\0';
    const char *rhs = eq + 1;
    char *name = buf; while (*name == ' ') name++;
    char *ne = name + strlen(name); while (ne > name && ne[-1] == ' ') *--ne = '\0';
    double rv = strtod(rhs, NULL);
    if (add)      var_set(g, name, var_get(g, name) + rv);
    else if (sub) var_set(g, name, var_get(g, name) - rv);
    else          var_set(g, name, rv);
    printf("(変数 %s = %g)\n", name, var_get(g, name));
}

static void play_event(Game *g, const char *event_id) {
    int guard = 0;
    while (event_id && *event_id && guard++ < 1000) {
        HtgEvent *e = htg_find_event(g->p, event_id);
        if (!e) {
            printf("(未定義のイベント '%s')\n", event_id);
            return;
        }

        for (size_t i = 0; i < e->line_count; i++) {
            HtgEventLine *ln = &e->lines[i];
            if (ln->condition && !htg_eval_condition(g->p, ln->condition))
                continue;
            if (ln->speaker && *ln->speaker)
                printf("%s: %s\n", ln->speaker, ln->text ? ln->text : "");
            else
                printf("%s\n", ln->text ? ln->text : "");
        }

        int is_choice = e->type && strcmp(e->type, "choice") == 0;
        if (is_choice && e->choice_count > 0) {
            const char *opts[64];
            size_t nopt = e->choice_count < 64 ? e->choice_count : 64;
            for (size_t i = 0; i < nopt; i++)
                opts[i] = e->choices[i].text ? e->choices[i].text : "(無題)";
            int sel = ui_menu("選択", opts, (int)nopt);
            if (sel < 1) return;
            HtgChoice *ch = &e->choices[sel - 1];
            if (ch->set_flag) { flag_set(g, ch->set_flag, 1); printf("(フラグ %s を立てた)\n", ch->set_flag); }
            if (ch->set_var)  apply_set_var(g, ch->set_var);
            if (ch->join_party) party_join(g, ch->join_party);
            event_id = ch->goto_event;
            continue;
        }

        event_id = e->next_event;
    }
}

/* ==========================================================================
 * Item usage outside battle (direct effect shortcut, spec 2.2)
 * ========================================================================== */

/* Apply a heal effect to the player (party[0]); returns the amount applied. */
static void use_item(Game *g, const char *item_id) {
    HtgItem *it = htg_find_item(g->p, item_id);
    if (!it) { printf("(不明なアイテム)\n"); return; }
    if (!it->usable) { printf("%s は使用できない。\n", it->name); return; }

    if (it->effect.has_effect && g->party_count > 0) {
        HtgMember *m = &g->party[0];
        if (it->effect.type && strcmp(it->effect.type, "heal_hp") == 0) {
            int max = htg_member_stat(g, m, HTG_STAT_MAXHP);
            m->hp += it->effect.amount; if (m->hp > max) m->hp = max;
            printf("%s を使った。HPが %d 回復した。(HP%d/%d)\n",
                   it->name, it->effect.amount, m->hp, max);
        } else if (it->effect.type && strcmp(it->effect.type, "heal_mp") == 0) {
            int max = htg_member_stat(g, m, HTG_STAT_MAXMP);
            m->mp += it->effect.amount; if (m->mp > max) m->mp = max;
            printf("%s を使った。MPが %d 回復した。(MP%d/%d)\n",
                   it->name, it->effect.amount, m->mp, max);
        } else {
            printf("%s を使った。効果: %s (%d)\n", it->name,
                   it->effect.type ? it->effect.type : "?", it->effect.amount);
        }
    }
    if (it->on_use_event) play_event(g, it->on_use_event);
    if (it->consumable) {
        inv_remove_one(g, item_id);
        printf("(%s は消費された)\n", it->name);
    }
}

/* ==========================================================================
 * Equipment (spec 2.3a / 3)
 * ========================================================================== */

static char **member_slot_ptr(HtgMember *m, const char *slot) {
    if (!slot) return NULL;
    if (strcmp(slot, "weapon") == 0)    return &m->equip_weapon;
    if (strcmp(slot, "armor") == 0)     return &m->equip_armor;
    if (strcmp(slot, "accessory") == 0) return &m->equip_accessory;
    return NULL;
}

static void do_equip(Game *g) {
    if (g->party_count == 0) return;
    /* Choose a member. */
    const char *labels[HTG_MAX_PARTY]; char store[HTG_MAX_PARTY][96];
    for (size_t i = 0; i < g->party_count; i++) {
        HtgActor *a = htg_find_actor(g->p, g->party[i].actor_id);
        snprintf(store[i], 96, "%s", a && a->name ? a->name : g->party[i].actor_id);
        labels[i] = store[i];
    }
    int msel = ui_menu("装備するキャラ", labels, (int)g->party_count);
    if (msel < 1) return;
    HtgMember *m = &g->party[msel - 1];

    /* List equippable items in the inventory. */
    const char *ilabels[HTG_MAX_INV]; char istore[64][128];
    int inv_idx[64]; int m2 = 0;
    for (size_t i = 0; i < g->inv_count && m2 < 64; i++) {
        HtgItem *it = htg_find_item(g->p, g->inventory[i]);
        if (!it || !it->equip.has_equip) continue;
        snprintf(istore[m2], 128, "%s [%s] ATK+%d DEF+%d SPD+%d HP+%d MP+%d",
                 it->name ? it->name : g->inventory[i],
                 it->equip.slot ? it->equip.slot : "?",
                 it->equip.atk_bonus, it->equip.def_bonus, it->equip.spd_bonus,
                 it->equip.hp_bonus, it->equip.mp_bonus);
        ilabels[m2] = istore[m2];
        inv_idx[m2] = (int)i;
        m2++;
    }
    if (m2 == 0) { printf("装備できるアイテムがない。\n"); return; }
    int isel = ui_menu("装備するアイテム", ilabels, m2);
    if (isel < 1) return;
    HtgItem *it = htg_find_item(g->p, g->inventory[inv_idx[isel - 1]]);
    char **slot = member_slot_ptr(m, it->equip.slot);
    if (!slot) { printf("不明なスロットです。\n"); return; }

    free(*slot);
    *slot = dup_str(g->inventory[inv_idx[isel - 1]]);
    printf("%s を装備した。\n", it->name ? it->name : "アイテム");
    /* Clamp current HP/MP to the (possibly changed) new maxima. */
    int maxhp = htg_member_stat(g, m, HTG_STAT_MAXHP);
    int maxmp = htg_member_stat(g, m, HTG_STAT_MAXMP);
    if (m->hp > maxhp) m->hp = maxhp;
    if (m->mp > maxmp) m->mp = maxmp;
}

/* ==========================================================================
 * Room presentation & navigation (spec 2.1, 3, 4)
 * ========================================================================== */

static void describe_room(Game *g) {
    HtgRoom *r = htg_find_room(g->p, g->current_room);
    if (!r) { printf("(部屋 '%s' が見つかりません)\n", g->current_room); g->running = 0; return; }
    printf("\n== %s ==\n", r->name ? r->name : g->current_room);
    if (r->description && *r->description) printf("%s\n", r->description);

    if (r->item_count > 0) {
        printf("床にアイテムがある:");
        for (size_t i = 0; i < r->item_count; i++) {
            HtgItem *it = htg_find_item(g->p, r->items[i]);
            printf(" %s", it && it->name ? it->name : r->items[i]);
        }
        printf("\n");
    }
}

static void pickup_room_items(Game *g) {
    HtgRoom *r = htg_find_room(g->p, g->current_room);
    if (!r) return;
    for (size_t i = 0; i < r->item_count; i++) {
        if (!inv_has(g, r->items[i])) {
            inv_add(g, r->items[i]);
            HtgItem *it = htg_find_item(g->p, r->items[i]);
            printf("* %s を拾った\n", it && it->name ? it->name : r->items[i]);
        }
    }
}

/* Handle a battle outcome: on defeat, end the game (game over). */
static void handle_battle_result(Game *g, HtgBattleResult res) {
    if (res == HTG_BATTLE_LOSE) {
        printf("\n=== GAME OVER ===\n");
        g->running = 0;
    }
}

/* Roll a random encounter and resolve it via the battle system (spec 3). */
static void maybe_encounter(Game *g) {
    HtgRoom *r = htg_find_room(g->p, g->current_room);
    if (!r || !r->has_encounter || !r->encounter_actor) return;
    int roll = rand() % 100;
    if (roll < r->encounter_chance) {
        HtgActor *a = htg_find_actor(g->p, r->encounter_actor);
        printf("\n! %s が現れた!\n", a && a->name ? a->name : r->encounter_actor);
        HtgBattleResult res = htg_battle_encounter(g, r->encounter_actor);
        handle_battle_result(g, res);
    }
}

static void enter_room(Game *g, const char *room_id) {
    HtgRoom *rr = htg_find_room(g->p, room_id);
    g->current_room = rr ? rr->id : room_id;
    if (rr && rr->on_enter_event) play_event(g, rr->on_enter_event);
    if (!g->running) return;
    describe_room(g);
    pickup_room_items(g);
    maybe_encounter(g);
}

static void do_move(Game *g) {
    HtgRoom *r = htg_find_room(g->p, g->current_room);
    if (!r || r->exit_count == 0) { printf("どこにも行けない。\n"); return; }

    const char *labels[64];
    const char *dests[64];
    char linebuf[64][160];
    size_t n = 0;
    for (size_t i = 0; i < r->exit_count && n < 64; i++) {
        HtgExit *ex = &r->exits[i];
        int locked = ex->locked_by_flag && !flag_get(g, ex->locked_by_flag);
        HtgRoom *dr = htg_find_room(g->p, ex->to);
        snprintf(linebuf[n], sizeof(linebuf[n]), "%s -> %s%s",
                 ex->direction,
                 dr && dr->name ? dr->name : (ex->to ? ex->to : "?"),
                 locked ? " [施錠]" : "");
        labels[n] = linebuf[n];
        dests[n] = locked ? NULL : ex->to;
        n++;
    }
    int sel = ui_menu("移動先", labels, (int)n);
    if (sel < 1) return;
    if (!dests[sel - 1]) {
        printf("そこは施錠されている。\n");
        return;
    }
    enter_room(g, dests[sel - 1]);
}

static void show_inventory(Game *g) {
    ui_header("持ち物");
    if (g->inv_count == 0) { printf("(なし)\n"); return; }
    for (size_t i = 0; i < g->inv_count; i++) {
        HtgItem *it = htg_find_item(g->p, g->inventory[i]);
        printf("%zu. %s\n", i + 1, it && it->name ? it->name : g->inventory[i]);
    }
    int sel;
    if (!ui_read_int(">> 使うアイテム番号(空Enterで戻る): ", &sel, 1, 0)) return;
    if (sel < 1 || (size_t)sel > g->inv_count) return;
    use_item(g, g->inventory[sel - 1]);
}

static void show_status(Game *g) {
    ui_header("パーティ状況");
    for (size_t i = 0; i < g->party_count; i++) {
        HtgMember *m = &g->party[i];
        HtgActor *a = htg_find_actor(g->p, m->actor_id);
        const char *nm = a && a->name ? a->name : m->actor_id;
        printf("%s%s HP%d/%d MP%d/%d ATK%d DEF%d SPD%d\n",
               i == 0 ? "主人公 " : "仲間 ", nm,
               m->hp, htg_member_stat(g, m, HTG_STAT_MAXHP),
               m->mp, htg_member_stat(g, m, HTG_STAT_MAXMP),
               htg_member_stat(g, m, HTG_STAT_ATK),
               htg_member_stat(g, m, HTG_STAT_DEF),
               htg_member_stat(g, m, HTG_STAT_SPD));
        if (m->equip_weapon || m->equip_armor || m->equip_accessory) {
            HtgItem *w = m->equip_weapon ? htg_find_item(g->p, m->equip_weapon) : NULL;
            HtgItem *ar = m->equip_armor ? htg_find_item(g->p, m->equip_armor) : NULL;
            HtgItem *ac = m->equip_accessory ? htg_find_item(g->p, m->equip_accessory) : NULL;
            printf("   装備: 武器=%s 防具=%s 装飾=%s\n",
                   w ? (w->name ? w->name : m->equip_weapon) : "なし",
                   ar ? (ar->name ? ar->name : m->equip_armor) : "なし",
                   ac ? (ac->name ? ac->name : m->equip_accessory) : "なし");
        }
    }
    if (g->party_count == 0) printf("(仲間なし)\n");
}

/* Save / load menu (spec 6). */
static void do_save(Game *g) {
    char path[512];
    if (!ui_read_string(">> セーブファイル名(空Enterで既定 " HTG_SAVE_DEFAULT "): ",
                        path, sizeof(path), 1))
        return;
    if (path[0] == '\0') snprintf(path, sizeof(path), "%s", HTG_SAVE_DEFAULT);
    if (htg_save_game(g, path) == 0)
        printf("→ '%s' にセーブしました。\n", path);
    else
        printf("!! セーブに失敗しました。\n");
}

static void do_load(Game *g) {
    char path[512];
    if (!ui_read_string(">> ロードするファイル名(空Enterで既定 " HTG_SAVE_DEFAULT "): ",
                        path, sizeof(path), 1))
        return;
    if (path[0] == '\0') snprintf(path, sizeof(path), "%s", HTG_SAVE_DEFAULT);
    if (htg_load_game(g, path) == 0) {
        printf("→ '%s' からロードしました。\n", path);
        describe_room(g);
    } else {
        printf("!! ロードに失敗しました(ファイルが無い/壊れている/別プロジェクト)。\n");
    }
}

/* ==========================================================================
 * Main loop
 * ========================================================================== */

int htg_engine_run(HtgProject *p) {
    if (!p) return 1;
    srand((unsigned)time(NULL));

    Game g;
    memset(&g, 0, sizeof(g));
    g.p = p;
    g.running = 1;

    const char *start = p->meta.start_room;
    if (!start || !*start || !htg_find_room(p, start)) {
        fprintf(stderr, "エラー: 開始部屋 '%s' が見つかりません。\n", start ? start : "(未設定)");
        return 1;
    }

    /* Seed the party with the player's default actor (party[0]). */
    const char *pl_id = p->meta.player_default_actor;
    if (!pl_id || !htg_find_actor(p, pl_id)) {
        fprintf(stderr, "エラー: 主人公アクター '%s' が見つかりません。\n",
                pl_id ? pl_id : "(未設定)");
        return 1;
    }
    party_add_member(&g, pl_id);

    printf("=== %s ===\n", p->meta.title ? p->meta.title : "HyperTxPG");
    enter_room(&g, start);

    const char *opts[] = {
        "移動", "調べる(再表示)", "持ち物", "装備", "状況",
        "セーブ", "ロード", "終了"
    };
    while (g.running) {
        if (!party_alive(&g)) {
            printf("\n=== GAME OVER ===\n");
            break;
        }
        int c = ui_menu("コマンド", opts, 8);
        if (c == 0 || c == 8) break;
        switch (c) {
            case 1: do_move(&g); break;
            case 2: describe_room(&g); break;
            case 3: show_inventory(&g); break;
            case 4: do_equip(&g); break;
            case 5: show_status(&g); break;
            case 6: do_save(&g); break;
            case 7: do_load(&g); break;
        }
    }

    printf("\nゲームを終了しました。\n");

    for (size_t i = 0; i < g.inv_count; i++) free(g.inventory[i]);
    for (size_t i = 0; i < g.party_count; i++) {
        free(g.party[i].actor_id);
        free(g.party[i].equip_weapon);
        free(g.party[i].equip_armor);
        free(g.party[i].equip_accessory);
    }
    return 0;
}
