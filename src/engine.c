/*
 * engine.c - `htg run` execution engine (spec section 4). Standard C only.
 *
 * State model:
 *   - Live flags/vars are kept in p->flags / p->vars (JSON objects). The engine
 *     reads and mutates these directly; because they are the model's owned
 *     clones, gameplay never corrupts the on-disk project.
 *   - The player's current room, inventory and recruited party are tracked in a
 *     local Game struct that borrows ids from the model.
 */
#include "engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include "json.h"
#include "ui.h"

/* ==========================================================================
 * Runtime state
 * ========================================================================== */

#define MAX_INV     256
#define MAX_PARTY   16

typedef struct {
    HtgProject *p;
    const char *current_room;          /* room id (borrowed) */
    char       *inventory[MAX_INV];    /* owned copies of item ids */
    size_t      inv_count;
    char       *party[MAX_PARTY];      /* owned copies of actor ids */
    size_t      party_count;
    int         running;
} Game;

/* ---- flag / var access on the live JSON state ---- */

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

/* Trim leading/trailing ASCII spaces in place-ish: returns pointer, sets end. */
static const char *trim(const char *s, const char **end) {
    while (*s == ' ' || *s == '\t') s++;
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *end = e;
    return s;
}

static int eval_term(Game *g, const char *s, const char *e) {
    /* copy into a local buffer for easy tokenizing */
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
        /* trim name */
        char *name = body;
        while (*name == ' ') name++;
        char *nend = name + strlen(name);
        while (nend > name && nend[-1] == ' ') *--nend = '\0';
        return flag_get(g, name) == want;
    }

    if (strncmp(buf, "var:", 4) == 0) {
        char *body = buf + 4;
        /* find operator */
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
        /* no operator: truthy if non-zero */
        char *ns = body; while (*ns == ' ') ns++;
        return var_get(g, ns) != 0;
    }

    /* Unknown term shape: treat as true so authors are not silently blocked. */
    return 1;
}

int htg_eval_condition(HtgProject *p, const char *cond) {
    if (!cond || !*cond) return 1;
    Game tmp; memset(&tmp, 0, sizeof(tmp)); tmp.p = p;

    /* Split on && / || left-to-right. */
    const char *s = cond;
    int acc = 0;
    int have_acc = 0;
    int pending_or = 0, pending_and = 0;

    while (*s) {
        /* find next connective */
        const char *amp = strstr(s, "&&");
        const char *bar = strstr(s, "||");
        const char *conn = NULL;
        int is_and = 0;
        if (amp && (!bar || amp < bar)) { conn = amp; is_and = 1; }
        else if (bar)                   { conn = bar; is_and = 0; }

        const char *term_end = conn ? conn : s + strlen(s);
        const char *te;
        const char *ts = trim(s, &te);
        if (term_end < te) te = term_end; /* clip to connective */
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
    if (g->inv_count >= MAX_INV) return;
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
        if (strcmp(g->party[i], id) == 0) return 1;
    return 0;
}
static void party_add(Game *g, const char *id) {
    int cap = g->p->meta.max_party_size > 0 ? g->p->meta.max_party_size : MAX_PARTY;
    if ((int)g->party_count >= cap) {
        printf("(パーティが満員のため '%s' は加入できませんでした)\n", id);
        return;
    }
    if (party_has(g, id)) return;
    g->party[g->party_count++] = dup_str(id);
    HtgActor *a = htg_find_actor(g->p, id);
    printf("* %s がパーティに加わった!\n", a && a->name ? a->name : id);
}

/* ==========================================================================
 * Event playback (spec 2.5, 4)
 * ==========================================================================
 * Returns nothing; mutates game state. Chains auto->next_event and choices.
 */

static void apply_set_var(Game *g, const char *expr) {
    /* expr form: "name=value" or "name+=value" or "name-=value" */
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
    /* trim name */
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

        /* Print lines whose condition passes. */
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
            /* Build a menu from choice texts. */
            const char *opts[64];
            size_t nopt = e->choice_count < 64 ? e->choice_count : 64;
            for (size_t i = 0; i < nopt; i++)
                opts[i] = e->choices[i].text ? e->choices[i].text : "(無題)";
            int sel = ui_menu("選択", opts, (int)nopt);
            if (sel < 1) return;
            HtgChoice *ch = &e->choices[sel - 1];
            if (ch->set_flag) { flag_set(g, ch->set_flag, 1); printf("(フラグ %s を立てた)\n", ch->set_flag); }
            if (ch->set_var)  apply_set_var(g, ch->set_var);
            if (ch->join_party) party_add(g, ch->join_party);
            event_id = ch->goto_event; /* NULL ends the chain */
            continue;
        }

        /* auto event: chain to next_event (NULL ends). */
        event_id = e->next_event;
    }
}

/* ==========================================================================
 * Item usage (direct effect shortcut, spec 2.2)
 * ========================================================================== */

static void use_item(Game *g, const char *item_id) {
    HtgItem *it = htg_find_item(g->p, item_id);
    if (!it) { printf("(不明なアイテム)\n"); return; }
    if (!it->usable) { printf("%s は使用できない。\n", it->name); return; }

    if (it->effect.has_effect) {
        /* Without an active battle/party HP model here, we report the effect;
         * numeric application lands with the battle stage (spec section 3/5). */
        printf("%s を使った。効果: %s (%d)\n", it->name,
               it->effect.type ? it->effect.type : "?", it->effect.amount);
    }
    if (it->on_use_event) play_event(g, it->on_use_event);
    if (it->consumable) {
        inv_remove_one(g, item_id);
        printf("(%s は消費された)\n", it->name);
    }
}

/* ==========================================================================
 * Room presentation & navigation (spec 2.1, 4)
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

/* Pick up all items currently listed in the room (once). */
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

/* Roll a random encounter placeholder (battle system is a later stage). */
static void maybe_encounter(Game *g) {
    HtgRoom *r = htg_find_room(g->p, g->current_room);
    if (!r || !r->has_encounter || !r->encounter_actor) return;
    int roll = rand() % 100;
    if (roll < r->encounter_chance) {
        HtgActor *a = htg_find_actor(g->p, r->encounter_actor);
        printf("\n! %s が現れた!(戦闘システムは次の実装段階で解決されます)\n",
               a && a->name ? a->name : r->encounter_actor);
    }
}

/* Enter a room: run on_enter_event, describe, pick items, roll encounter. */
static void enter_room(Game *g, const char *room_id) {
    g->current_room = room_id;
    HtgRoom *r = htg_find_room(g->p, room_id);
    if (r && r->on_enter_event) play_event(g, r->on_enter_event);
    if (!g->running) return;
    describe_room(g);
    pickup_room_items(g);
    maybe_encounter(g);
}

/* Move menu: list unlocked exits, honor locked_by_flag. */
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
    HtgActor *pl = htg_find_actor(g->p, g->p->meta.player_default_actor);
    if (pl) printf("主人公: %s HP%d MP%d ATK%d DEF%d SPD%d\n",
                   pl->name ? pl->name : "?", pl->hp, pl->mp, pl->atk, pl->def, pl->spd);
    for (size_t i = 0; i < g->party_count; i++) {
        HtgActor *a = htg_find_actor(g->p, g->party[i]);
        if (a) printf("仲間: %s HP%d MP%d ATK%d DEF%d SPD%d\n",
                      a->name ? a->name : g->party[i], a->hp, a->mp, a->atk, a->def, a->spd);
    }
    if (g->party_count == 0) printf("(仲間なし)\n");
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

    printf("=== %s ===\n", p->meta.title ? p->meta.title : "HyperTxPG");
    enter_room(&g, start);

    const char *opts[] = { "移動", "調べる(再表示)", "持ち物", "状況", "終了" };
    while (g.running) {
        int c = ui_menu("コマンド", opts, 5);
        if (c == 0 || c == 5) break;
        switch (c) {
            case 1: do_move(&g); break;
            case 2: describe_room(&g); break;
            case 3: show_inventory(&g); break;
            case 4: show_status(&g); break;
        }
    }

    printf("\nゲームを終了しました。\n");

    for (size_t i = 0; i < g.inv_count; i++) free(g.inventory[i]);
    for (size_t i = 0; i < g.party_count; i++) free(g.party[i]);
    return 0;
}
