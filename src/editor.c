/*
 * editor.c - Interactive `htg edit` editor (spec sections 3, 4 & 5).
 * Standard C only.
 *
 * The editor loads the .htgp into the data model (for validation/reporting)
 * but performs all mutations against the raw JSON source tree (p->source),
 * because htg_project_save() serializes that tree. Each top collection
 * ("rooms","items","actors","skills","events") is a JSON object keyed by id;
 * CRUD operations add/remove/replace members of those objects.
 */
#include "editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "json.h"
#include "model.h"
#include "ui.h"

/* ==========================================================================
 * Small JSON-tree helpers scoped to editing
 * ========================================================================== */

/* local strdup (POSIX strdup avoided for strict C portability) */
static char *strdup_local(const char *s);

/* Get (or lazily create) a top-level object collection on the root. */
static JsonValue *root_collection(JsonValue *root, const char *key) {
    JsonValue *c = json_object_get(root, key);
    if (!c || c->type != JSON_OBJECT) {
        c = json_new_object();
        json_object_set(root, key, c);
    }
    return c;
}

/* Validate an id: non-empty, ASCII alphanumeric or underscore. */
static int valid_id(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_')) return 0;
    }
    return 1;
}

/* Split a comma-separated list into a JSON array of trimmed strings. Empty
 * input yields an empty array. */
static JsonValue *csv_to_array(const char *csv) {
    JsonValue *arr = json_new_array();
    if (!csv || !*csv) return arr;
    const char *p = csv;
    char item[256];
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        size_t n = 0;
        while (*p && *p != ',' && n + 1 < sizeof(item)) item[n++] = *p++;
        /* trim trailing spaces */
        while (n > 0 && (item[n - 1] == ' ' || item[n - 1] == '\t')) n--;
        item[n] = '\0';
        if (n > 0) json_array_add(arr, json_new_string(item));
        if (*p == ',') p++;
    }
    return arr;
}

/* Print the ids in a collection as a numbered list; returns count. */
static size_t list_collection(JsonValue *coll, const char *label) {
    size_t n = json_object_size(coll);
    ui_header(label);
    if (n == 0) {
        printf("(登録なし)\n");
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        const char *id = json_object_key_at(coll, i);
        JsonValue *o = json_object_get(coll, id);
        const char *name = json_object_get_string(o, "name", "");
        printf("%zu. %s%s%s\n", i + 1, id,
               (name && *name) ? " - " : "", name ? name : "");
    }
    return n;
}

/*
 * Ask the user to pick an existing id from a collection (for edit/delete).
 * Returns a heap-copy of the chosen id (caller frees) or NULL on cancel/empty.
 */
static char *pick_existing(JsonValue *coll, const char *label) {
    size_t n = list_collection(coll, label);
    if (n == 0) return NULL;
    int idx;
    if (!ui_read_int(">> 番号を選択(空Enterで戻る): ", &idx, 1, 0)) return NULL;
    if (idx < 1 || (size_t)idx > n) {
        if (idx != 0) printf("!! 範囲外です。\n");
        return NULL;
    }
    const char *id = json_object_key_at(coll, (size_t)idx - 1);
    return id ? strdup_local(id) : NULL;
}

static char *strdup_local(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

/*
 * Read a fresh id for a new entry, enforcing the non-duplicate + charset rule
 * (spec 4.3). Writes into out. Returns 1 on success, 0 on cancel (EOF/empty).
 */
static int read_new_id(JsonValue *coll, char *out, size_t cap) {
    for (;;) {
        if (!ui_read_string("ID(半角英数・_、重複不可、空Enterで中止): ",
                            out, cap, 1))
            return 0;
        if (out[0] == '\0') return 0;
        if (!valid_id(out)) {
            printf("!! IDは半角英数字と _ のみ使用できます。\n");
            continue;
        }
        if (json_object_get(coll, out)) {
            printf("!! ID '%s' は既に存在します。\n", out);
            continue;
        }
        return 1;
    }
}

/* ==========================================================================
 * Room editing
 * ========================================================================== */

/* Build/replace a room object interactively. `existing` may be NULL (create). */
static void room_form(JsonValue *root, JsonValue *coll, const char *id,
                      JsonValue *existing) {
    JsonValue *o = json_new_object();
    char buf[512];

    const char *cur_name = existing ? json_object_get_string(existing, "name", "") : "";
    snprintf(buf, sizeof(buf), "表示名%s%s%s: ",
             *cur_name ? " [" : "", cur_name, *cur_name ? "]" : "");
    char name[256];
    ui_read_string(buf, name, sizeof(name), 1);
    if (name[0] == '\0' && existing) snprintf(name, sizeof(name), "%s", cur_name);
    json_object_set(o, "name", json_new_string(name));

    char desc[512];
    ui_read_string("説明: ", desc, sizeof(desc), 1);
    if (desc[0] == '\0' && existing)
        snprintf(desc, sizeof(desc), "%s",
                 json_object_get_string(existing, "description", ""));
    json_object_set(o, "description", json_new_string(desc));

    /* Exits: loop of direction -> destination (+ optional lock flag). */
    JsonValue *exits = json_new_object();
    printf("--- 出口(exits)を追加します。方向名を空Enterで終了 ---\n");
    for (;;) {
        char dir[64];
        if (!ui_read_string("方向(例: north、空Enterで終了): ", dir, sizeof(dir), 1))
            break;
        if (dir[0] == '\0') break;
        char to[128];
        ui_read_string("  遷移先room_id: ", to, sizeof(to), 1);
        char lock[128];
        ui_read_string("  施錠フラグ(なければ空Enter): ", lock, sizeof(lock), 1);
        if (lock[0] != '\0') {
            JsonValue *ex = json_new_object();
            json_object_set(ex, "to", json_new_string(to));
            json_object_set(ex, "locked_by_flag", json_new_string(lock));
            json_object_set(exits, dir, ex);
        } else {
            json_object_set(exits, dir, json_new_string(to));
        }
    }
    json_object_set(o, "exits", exits);

    char items[512];
    ui_read_string("配置アイテム(カンマ区切りID、なければ空Enter): ",
                   items, sizeof(items), 1);
    json_object_set(o, "items", csv_to_array(items));

    char onenter[128];
    ui_read_string("on_enter_event(なければ空Enter): ", onenter, sizeof(onenter), 1);
    if (onenter[0] != '\0')
        json_object_set(o, "on_enter_event", json_new_string(onenter));

    if (ui_read_yesno("ランダムエンカウントを設定しますか?(y/n): ", 0)) {
        int chance = 0;
        ui_read_int("  遭遇率(0-100): ", &chance, 1, 0);
        if (chance < 0) chance = 0;
        if (chance > 100) chance = 100;
        char actor[128];
        ui_read_string("  出現actor_id: ", actor, sizeof(actor), 1);
        JsonValue *enc = json_new_object();
        json_object_set(enc, "chance", json_new_number(chance));
        json_object_set(enc, "actor", json_new_string(actor));
        json_object_set(o, "encounter", enc);
    }

    json_object_set(coll, id, o); /* replaces existing member if present */
    (void)root;
    printf("→ 部屋 '%s' を保存しました\n", id);
}

/* ==========================================================================
 * Actor editing
 * ========================================================================== */

static void actor_form(JsonValue *coll, const char *id, JsonValue *existing) {
    JsonValue *o = json_new_object();
    char name[256];
    ui_read_string("表示名: ", name, sizeof(name), 1);
    if (name[0] == '\0' && existing)
        snprintf(name, sizeof(name), "%s", json_object_get_string(existing, "name", ""));
    json_object_set(o, "name", json_new_string(name));

    int hp = 0, mp = 0, atk = 0, def = 0, spd = 0;
    ui_read_int("HP: ", &hp, 1, existing ? (int)json_object_get_number(existing, "hp", 0) : 0);
    ui_read_int("MP: ", &mp, 1, existing ? (int)json_object_get_number(existing, "mp", 0) : 0);
    ui_read_int("攻撃力: ", &atk, 1, existing ? (int)json_object_get_number(existing, "atk", 0) : 0);
    ui_read_int("防御力: ", &def, 1, existing ? (int)json_object_get_number(existing, "def", 0) : 0);
    ui_read_int("素早さ: ", &spd, 1, existing ? (int)json_object_get_number(existing, "spd", 0) : 0);
    json_object_set(o, "hp", json_new_number(hp));
    json_object_set(o, "mp", json_new_number(mp));
    json_object_set(o, "atk", json_new_number(atk));
    json_object_set(o, "def", json_new_number(def));
    json_object_set(o, "spd", json_new_number(spd));

    int is_enemy = ui_read_yesno("このキャラは敵ですか?(y/n): ",
                                 existing ? json_object_get_bool(existing, "is_enemy", 0) : 0);
    json_object_set(o, "is_enemy", json_new_bool(is_enemy));

    char skills[512];
    ui_read_string("使用スキル(カンマ区切りID、なければ空Enter): ", skills, sizeof(skills), 1);
    json_object_set(o, "skills", csv_to_array(skills));

    if (is_enemy) {
        char drop[512];
        ui_read_string("ドロップアイテム(カンマ区切りID、なければ空Enter): ", drop, sizeof(drop), 1);
        json_object_set(o, "drop", csv_to_array(drop));
    } else {
        int recruitable = ui_read_yesno("仲間として加入可能?(recruitable)(y/n): ",
                                        existing ? json_object_get_bool(existing, "recruitable", 0) : 0);
        json_object_set(o, "recruitable", json_new_bool(recruitable));
        /*味方は3スロットの装備を持つ(初期は未装備) */
        JsonValue *eq = json_new_object();
        JsonValue *cur = existing ? json_object_get(existing, "equipment") : NULL;
        const char *w = cur ? json_object_get_string(cur, "weapon", NULL) : NULL;
        const char *a = cur ? json_object_get_string(cur, "armor", NULL) : NULL;
        const char *ac = cur ? json_object_get_string(cur, "accessory", NULL) : NULL;
        json_object_set(eq, "weapon", w ? json_new_string(w) : json_new_null());
        json_object_set(eq, "armor", a ? json_new_string(a) : json_new_null());
        json_object_set(eq, "accessory", ac ? json_new_string(ac) : json_new_null());
        json_object_set(o, "equipment", eq);
    }

    json_object_set(coll, id, o);
    printf("→ キャラ '%s' を保存しました\n", id);
}

/* ==========================================================================
 * Item editing
 * ========================================================================== */

static void item_form(JsonValue *coll, const char *id, JsonValue *existing) {
    JsonValue *o = json_new_object();
    char name[256];
    ui_read_string("表示名: ", name, sizeof(name), 1);
    if (name[0] == '\0' && existing)
        snprintf(name, sizeof(name), "%s", json_object_get_string(existing, "name", ""));
    json_object_set(o, "name", json_new_string(name));

    char desc[512];
    ui_read_string("説明: ", desc, sizeof(desc), 1);
    json_object_set(o, "description", json_new_string(desc));

    int usable = ui_read_yesno("使用可能アイテムですか?(usable)(y/n): ",
                               existing ? json_object_get_bool(existing, "usable", 0) : 0);
    json_object_set(o, "usable", json_new_bool(usable));

    if (usable) {
        int consumable = ui_read_yesno("使用で消費されますか?(consumable)(y/n): ",
                                       existing ? json_object_get_bool(existing, "consumable", 0) : 0);
        json_object_set(o, "consumable", json_new_bool(consumable));

        char onuse[128];
        ui_read_string("on_use_event(なければ空Enter): ", onuse, sizeof(onuse), 1);
        if (onuse[0] != '\0')
            json_object_set(o, "on_use_event", json_new_string(onuse));

        if (ui_read_yesno("直接効果(effect)を設定しますか?(y/n): ", 0)) {
            char type[64];
            ui_read_string("  効果タイプ(例: heal_hp / heal_mp / buff_atk): ", type, sizeof(type), 1);
            int amount = 0;
            ui_read_int("  効果量(amount): ", &amount, 1, 0);
            JsonValue *ef = json_new_object();
            json_object_set(ef, "type", json_new_string(type));
            json_object_set(ef, "amount", json_new_number(amount));
            json_object_set(o, "effect", ef);
        }
    }

    if (ui_read_yesno("装備品(equip)にしますか?(y/n): ",
                      existing ? (json_object_get(existing, "equip") != NULL) : 0)) {
        const char *slot_opts[] = { "weapon", "armor", "accessory" };
        int s = ui_menu("装備スロット", slot_opts, 3);
        if (s < 1) s = 1;
        JsonValue *eq = json_new_object();
        json_object_set(eq, "slot", json_new_string(slot_opts[s - 1]));
        int v;
        ui_read_int("  atk_bonus: ", &v, 1, 0); json_object_set(eq, "atk_bonus", json_new_number(v));
        ui_read_int("  def_bonus: ", &v, 1, 0); json_object_set(eq, "def_bonus", json_new_number(v));
        ui_read_int("  spd_bonus: ", &v, 1, 0); json_object_set(eq, "spd_bonus", json_new_number(v));
        ui_read_int("  hp_bonus: ",  &v, 1, 0); json_object_set(eq, "hp_bonus",  json_new_number(v));
        ui_read_int("  mp_bonus: ",  &v, 1, 0); json_object_set(eq, "mp_bonus",  json_new_number(v));
        json_object_set(o, "equip", eq);
    }

    json_object_set(coll, id, o);
    printf("→ アイテム '%s' を保存しました\n", id);
}

/* ==========================================================================
 * Skill editing
 * ========================================================================== */

static void skill_form(JsonValue *coll, const char *id, JsonValue *existing) {
    JsonValue *o = json_new_object();
    char name[256];
    ui_read_string("表示名: ", name, sizeof(name), 1);
    if (name[0] == '\0' && existing)
        snprintf(name, sizeof(name), "%s", json_object_get_string(existing, "name", ""));
    json_object_set(o, "name", json_new_string(name));

    const char *type_opts[] = { "physical", "magic" };
    int t = ui_menu("スキル種別", type_opts, 2);
    if (t < 1) t = 1;
    json_object_set(o, "type", json_new_string(type_opts[t - 1]));

    int power = 0, mp = 0, acc = 100;
    ui_read_int("威力(power): ", &power, 1, existing ? (int)json_object_get_number(existing, "power", 0) : 0);
    ui_read_int("消費MP(mp_cost): ", &mp, 1, existing ? (int)json_object_get_number(existing, "mp_cost", 0) : 0);
    ui_read_int("命中率(accuracy 0-100): ", &acc, 1, existing ? (int)json_object_get_number(existing, "accuracy", 100) : 100);
    json_object_set(o, "power", json_new_number(power));
    json_object_set(o, "mp_cost", json_new_number(mp));
    json_object_set(o, "accuracy", json_new_number(acc));

    char element[64];
    ui_read_string("属性(element、なければ空Enter): ", element, sizeof(element), 1);
    if (element[0] != '\0')
        json_object_set(o, "element", json_new_string(element));

    json_object_set(coll, id, o);
    printf("→ スキル '%s' を保存しました\n", id);
}

/* ==========================================================================
 * Event editing (auto / choice)
 * ========================================================================== */

static void event_form(JsonValue *coll, const char *id, JsonValue *existing) {
    (void)existing;
    JsonValue *o = json_new_object();

    const char *type_opts[] = { "auto (選択肢なし)", "choice (選択肢あり)" };
    int t = ui_menu("イベント種別", type_opts, 2);
    int is_choice = (t == 2);
    json_object_set(o, "type", json_new_string(is_choice ? "choice" : "auto"));

    /* lines loop */
    JsonValue *lines = json_new_array();
    printf("--- セリフ(lines)を追加します。textを空Enterで終了 ---\n");
    for (;;) {
        char text[512];
        if (!ui_read_string("text(空Enterで終了): ", text, sizeof(text), 1)) break;
        if (text[0] == '\0') break;
        char speaker[128];
        ui_read_string("  speaker(話者、地の文なら空Enter): ", speaker, sizeof(speaker), 1);
        char cond[256];
        ui_read_string("  condition(表示条件、なければ空Enter): ", cond, sizeof(cond), 1);
        JsonValue *ln = json_new_object();
        json_object_set(ln, "speaker", json_new_string(speaker));
        json_object_set(ln, "text", json_new_string(text));
        if (cond[0] != '\0') json_object_set(ln, "condition", json_new_string(cond));
        json_array_add(lines, ln);
    }
    json_object_set(o, "lines", lines);

    JsonValue *choices = json_new_array();
    if (is_choice) {
        printf("--- 選択肢(choices)を追加します。textを空Enterで終了 ---\n");
        for (;;) {
            char text[512];
            if (!ui_read_string("選択肢text(空Enterで終了): ", text, sizeof(text), 1)) break;
            if (text[0] == '\0') break;
            JsonValue *ch = json_new_object();
            json_object_set(ch, "text", json_new_string(text));
            char go[128];
            ui_read_string("  goto(次イベントID、終了なら空Enter=null): ", go, sizeof(go), 1);
            json_object_set(ch, "goto", go[0] ? json_new_string(go) : json_new_null());
            char sf[128];
            ui_read_string("  set_flag(なければ空Enter): ", sf, sizeof(sf), 1);
            if (sf[0]) json_object_set(ch, "set_flag", json_new_string(sf));
            char sv[128];
            ui_read_string("  set_var(例: gold=10、なければ空Enter): ", sv, sizeof(sv), 1);
            if (sv[0]) json_object_set(ch, "set_var", json_new_string(sv));
            char jp[128];
            ui_read_string("  join_party(加入actor_id、なければ空Enter): ", jp, sizeof(jp), 1);
            if (jp[0]) json_object_set(ch, "join_party", json_new_string(jp));
            json_array_add(choices, ch);
        }
    } else {
        char ne[128];
        ui_read_string("next_event(連鎖先ID、終了なら空Enter=null): ", ne, sizeof(ne), 1);
        json_object_set(o, "next_event", ne[0] ? json_new_string(ne) : json_new_null());
    }
    json_object_set(o, "choices", choices);

    json_object_set(coll, id, o);
    printf("→ イベント '%s' を保存しました\n", id);
}

/* ==========================================================================
 * Generic category submenu (spec 4.2)
 * ========================================================================== */

typedef void (*FormFn)(JsonValue *coll, const char *id, JsonValue *existing);

/* Adapter so the room form (which also wants root) matches FormFn via a
 * file-scope pointer. */
static JsonValue *g_root_for_room = NULL;
static void room_form_adapter(JsonValue *coll, const char *id, JsonValue *existing) {
    room_form(g_root_for_room, coll, id, existing);
}

static void category_menu(JsonValue *root, const char *coll_key,
                          const char *label, FormFn form) {
    const char *opts[] = {
        "新規作成", "既存編集", "既存削除", "一覧表示", "戻る"
    };
    char title[128];
    snprintf(title, sizeof(title), "%s編集", label);

    for (;;) {
        JsonValue *coll = root_collection(root, coll_key);
        int c = ui_menu(title, opts, 5);
        if (c == 0 || c == 5) return;
        switch (c) {
            case 1: { /* create */
                char id[128];
                if (!read_new_id(coll, id, sizeof(id))) { printf("中止しました。\n"); break; }
                form(coll, id, NULL);
                break;
            }
            case 2: { /* edit existing */
                char *id = pick_existing(coll, label);
                if (!id) break;
                JsonValue *existing = json_object_get(coll, id);
                form(coll, id, existing);
                free(id);
                break;
            }
            case 3: { /* delete */
                char *id = pick_existing(coll, label);
                if (!id) break;
                if (ui_read_yesno("本当に削除しますか?(y/n): ", 0)) {
                    json_object_remove(coll, id);
                    printf("→ '%s' を削除しました\n", id);
                }
                free(id);
                break;
            }
            case 4: /* list */
                list_collection(coll, label);
                break;
        }
    }
}

/* ==========================================================================
 * Game settings (spec 4.1 item 6)
 * ========================================================================== */

static void settings_menu(JsonValue *root) {
    const char *opts[] = {
        "タイトルを変更", "開始部屋(start_room)を変更", "最大パーティ人数を変更",
        "フラグを追加/削除", "変数を追加/削除", "戻る"
    };
    for (;;) {
        JsonValue *meta = root_collection(root, "meta");
        int c = ui_menu("ゲーム設定", opts, 6);
        if (c == 0 || c == 6) return;
        switch (c) {
            case 1: {
                char t[256];
                printf("現在: %s\n", json_object_get_string(meta, "title", ""));
                if (ui_read_string("新しいタイトル: ", t, sizeof(t), 1) && t[0])
                    json_object_set(meta, "title", json_new_string(t));
                break;
            }
            case 2: {
                char t[128];
                printf("現在: %s\n", json_object_get_string(meta, "start_room", ""));
                if (ui_read_string("新しい開始部屋ID: ", t, sizeof(t), 1) && t[0])
                    json_object_set(meta, "start_room", json_new_string(t));
                break;
            }
            case 3: {
                int v;
                if (ui_read_int("最大パーティ人数: ", &v, 1,
                                (int)json_object_get_number(meta, "max_party_size", 3)))
                    json_object_set(meta, "max_party_size", json_new_number(v));
                break;
            }
            case 4: {
                JsonValue *flags = root_collection(root, "flags");
                printf("現在のフラグ数: %zu\n", json_object_size(flags));
                char name[128];
                if (!ui_read_string("フラグ名(既存なら削除、新規なら追加): ", name, sizeof(name), 1) || !name[0])
                    break;
                if (json_object_get(flags, name)) {
                    json_object_remove(flags, name);
                    printf("→ フラグ '%s' を削除しました\n", name);
                } else {
                    int init = ui_read_yesno("初期値 true?(y/n): ", 0);
                    json_object_set(flags, name, json_new_bool(init));
                    printf("→ フラグ '%s' を追加しました\n", name);
                }
                break;
            }
            case 5: {
                JsonValue *vars = root_collection(root, "vars");
                printf("現在の変数数: %zu\n", json_object_size(vars));
                char name[128];
                if (!ui_read_string("変数名(既存なら削除、新規なら追加): ", name, sizeof(name), 1) || !name[0])
                    break;
                if (json_object_get(vars, name)) {
                    json_object_remove(vars, name);
                    printf("→ 変数 '%s' を削除しました\n", name);
                } else {
                    int init = 0;
                    ui_read_int("初期値(整数): ", &init, 1, 0);
                    json_object_set(vars, name, json_new_number(init));
                    printf("→ 変数 '%s' を追加しました\n", name);
                }
                break;
            }
        }
    }
}

/* ==========================================================================
 * Save-time cross validation (spec section 5) - warnings only.
 * ========================================================================== */

static int coll_has(JsonValue *root, const char *coll_key, const char *id) {
    JsonValue *c = json_object_get(root, coll_key);
    return id && c && json_object_get(c, id) != NULL;
}

/* Emit a warning and bump *count. */
static void warn(int *count, const char *fmt, const char *a, const char *b) {
    printf("  [警告] ");
    printf(fmt, a, b);
    printf("\n");
    (*count)++;
}

static void validate_project(JsonValue *root) {
    int warnings = 0;
    printf("\n--- 保存時検証 ---\n");

    JsonValue *meta = json_object_get(root, "meta");
    const char *start = json_object_get_string(meta, "start_room", "");
    if (!coll_has(root, "rooms", start))
        warn(&warnings, "meta.start_room '%s' が rooms に存在しません%s", start, "");

    /* rooms: exits.to, on_enter_event, items */
    JsonValue *rooms = json_object_get(root, "rooms");
    for (size_t i = 0; i < json_object_size(rooms); i++) {
        const char *rid = json_object_key_at(rooms, i);
        JsonValue *r = json_object_get(rooms, rid);
        JsonValue *exits = json_object_get(r, "exits");
        for (size_t j = 0; j < json_object_size(exits); j++) {
            const char *dir = json_object_key_at(exits, j);
            JsonValue *ex = json_object_get(exits, dir);
            const char *to = (ex && ex->type == JSON_STRING)
                             ? ex->as.string
                             : json_object_get_string(ex, "to", "");
            if (!coll_has(root, "rooms", to))
                warn(&warnings, "room '%s' の出口 -> '%s' が存在しません", rid, to);
        }
        const char *oe = json_object_get_string(r, "on_enter_event", NULL);
        if (oe && !coll_has(root, "events", oe))
            warn(&warnings, "room '%s' の on_enter_event '%s' が存在しません", rid, oe);
        JsonValue *items = json_object_get(r, "items");
        for (size_t j = 0; j < json_array_size(items); j++) {
            const char *iid = json_get_string(json_array_get(items, j), NULL);
            if (iid && !coll_has(root, "items", iid))
                warn(&warnings, "room '%s' のアイテム '%s' が存在しません", rid, iid);
        }
        JsonValue *enc = json_object_get(r, "encounter");
        if (enc) {
            const char *ea = json_object_get_string(enc, "actor", NULL);
            if (ea && !coll_has(root, "actors", ea))
                warn(&warnings, "room '%s' の encounter.actor '%s' が存在しません", rid, ea);
        }
    }

    /* items: on_use_event */
    JsonValue *items = json_object_get(root, "items");
    for (size_t i = 0; i < json_object_size(items); i++) {
        const char *iid = json_object_key_at(items, i);
        const char *ue = json_object_get_string(json_object_get(items, iid), "on_use_event", NULL);
        if (ue && !coll_has(root, "events", ue))
            warn(&warnings, "item '%s' の on_use_event '%s' が存在しません", iid, ue);
    }

    /* actors: skills, drop */
    JsonValue *actors = json_object_get(root, "actors");
    for (size_t i = 0; i < json_object_size(actors); i++) {
        const char *aid = json_object_key_at(actors, i);
        JsonValue *a = json_object_get(actors, aid);
        JsonValue *sk = json_object_get(a, "skills");
        for (size_t j = 0; j < json_array_size(sk); j++) {
            const char *sid = json_get_string(json_array_get(sk, j), NULL);
            if (sid && !coll_has(root, "skills", sid))
                warn(&warnings, "actor '%s' のスキル '%s' が存在しません", aid, sid);
        }
        JsonValue *dr = json_object_get(a, "drop");
        for (size_t j = 0; j < json_array_size(dr); j++) {
            const char *did = json_get_string(json_array_get(dr, j), NULL);
            if (did && !coll_has(root, "items", did))
                warn(&warnings, "actor '%s' のドロップ '%s' が存在しません", aid, did);
        }
    }

    /* events: choices[].goto, next_event, join_party */
    JsonValue *events = json_object_get(root, "events");
    for (size_t i = 0; i < json_object_size(events); i++) {
        const char *eid = json_object_key_at(events, i);
        JsonValue *e = json_object_get(events, eid);
        const char *ne = json_object_get_string(e, "next_event", NULL);
        if (ne && !coll_has(root, "events", ne))
            warn(&warnings, "event '%s' の next_event '%s' が存在しません", eid, ne);
        JsonValue *choices = json_object_get(e, "choices");
        for (size_t j = 0; j < json_array_size(choices); j++) {
            JsonValue *ch = json_array_get(choices, j);
            const char *g = json_object_get_string(ch, "goto", NULL);
            if (g && !coll_has(root, "events", g))
                warn(&warnings, "event '%s' の choices.goto '%s' が存在しません", eid, g);
            const char *jp = json_object_get_string(ch, "join_party", NULL);
            if (jp && !coll_has(root, "actors", jp))
                warn(&warnings, "event '%s' の join_party '%s' が存在しません", eid, jp);
        }
    }

    if (warnings == 0)
        printf("  問題は見つかりませんでした。\n");
    else
        printf("  %d 件の警告があります(保存はブロックされません)。\n", warnings);
}

/* ==========================================================================
 * Top-level editor loop (spec 4.1)
 * ========================================================================== */

int htg_editor_run(const char *path) {
    const char *err = NULL;
    HtgProject *p = htg_project_load(path, &err);
    if (!p) {
        fprintf(stderr, "エラー: '%s' の読み込みに失敗しました: %s\n",
                path, err ? err : "不明なエラー");
        return 1;
    }
    JsonValue *root = p->source; /* mutate this; it is what we serialize */
    g_root_for_room = root;

    char title[256];
    snprintf(title, sizeof(title), "HyperTxPG Editor: %s", path);

    const char *opts[] = {
        "部屋(Room)を編集",
        "キャラクター(Actor)を編集",
        "アイテムを編集",
        "スキル/魔法を編集",
        "イベント(会話・分岐)を編集",
        "ゲーム設定(タイトル/開始部屋/フラグ/変数)",
        "保存して終了",
        "保存せず終了"
    };

    int dirty = 0;
    for (;;) {
        int c = ui_menu(title, opts, 8);
        if (c == 0) { /* EOF: behave like save-less exit */
            printf("\n(入力終了。保存せずに終了します)\n");
            break;
        }
        switch (c) {
            case 1: category_menu(root, "rooms",  "部屋",     room_form_adapter); dirty = 1; break;
            case 2: category_menu(root, "actors", "キャラ",   actor_form);        dirty = 1; break;
            case 3: category_menu(root, "items",  "アイテム", item_form);         dirty = 1; break;
            case 4: category_menu(root, "skills", "スキル",   skill_form);        dirty = 1; break;
            case 5: category_menu(root, "events", "イベント", event_form);        dirty = 1; break;
            case 6: settings_menu(root); dirty = 1; break;
            case 7: {
                validate_project(root);
                if (json_serialize_file(root, path, 1) == 0)
                    printf("→ '%s' に保存しました\n", path);
                else
                    fprintf(stderr, "エラー: 保存に失敗しました。\n");
                htg_project_free(p);
                return 0;
            }
            case 8: {
                if (dirty && !ui_read_yesno("変更があります。保存せず終了しますか?(y/n): ", 1)) {
                    break;
                }
                printf("保存せずに終了しました。\n");
                htg_project_free(p);
                return 0;
            }
        }
    }
    htg_project_free(p);
    return 0;
}
