/*
 * model.c - Load/build/save the HyperTxPG data model (spec section 2).
 * Standard C only.
 */
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

/* strdup is POSIX but not ISO C; provide a portable local version. */
static char *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

/* Deep-clone a JSON value (used to keep flags/vars independent of source). */
static JsonValue *json_clone(const JsonValue *v) {
    if (!v) return NULL;
    switch (v->type) {
        case JSON_NULL:   return json_new_null();
        case JSON_BOOL:   return json_new_bool(v->as.boolean);
        case JSON_NUMBER: return json_new_number(v->as.number);
        case JSON_STRING: return json_new_string(v->as.string);
        case JSON_ARRAY: {
            JsonValue *a = json_new_array();
            if (!a) return NULL;
            for (size_t i = 0; i < v->as.array.count; i++) {
                JsonValue *c = json_clone(v->as.array.items[i]);
                if (!c || json_array_add(a, c) != 0) { json_free(c); json_free(a); return NULL; }
            }
            return a;
        }
        case JSON_OBJECT: {
            JsonValue *o = json_new_object();
            if (!o) return NULL;
            for (JsonMember *m = v->as.object.head; m; m = m->next) {
                JsonValue *c = json_clone(m->value);
                if (!c || json_object_set(o, m->key, c) != 0) { json_free(c); json_free(o); return NULL; }
            }
            return o;
        }
    }
    return NULL;
}

/* Copy a JSON array-of-strings into a newly allocated char** (ids/skills/etc).
 * Non-string elements are skipped. Sets *out_count. Returns NULL if empty. */
static char **dup_string_array(const JsonValue *arr, size_t *out_count) {
    *out_count = 0;
    size_t n = json_array_size(arr);
    if (n == 0) return NULL;
    char **out = (char **)calloc(n, sizeof(char *));
    if (!out) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        JsonValue *e = json_array_get(arr, i);
        if (e && e->type == JSON_STRING) out[k++] = dupstr(e->as.string);
    }
    *out_count = k;
    if (k == 0) { free(out); return NULL; }
    return out;
}

static void free_string_array(char **arr, size_t count) {
    if (!arr) return;
    for (size_t i = 0; i < count; i++) free(arr[i]);
    free(arr);
}

static int obj_int(const JsonValue *obj, const char *key, int def) {
    return (int)json_object_get_number(obj, key, (double)def);
}

/* ==========================================================================
 * Parse each schema entry
 * ========================================================================== */

static int parse_meta(HtgMeta *m, const JsonValue *root) {
    const JsonValue *meta = json_object_get(root, "meta");
    m->title                = dupstr(json_object_get_string(meta, "title", ""));
    m->version              = dupstr(json_object_get_string(meta, "version", "1.0"));
    m->start_room           = dupstr(json_object_get_string(meta, "start_room", ""));
    m->player_default_actor = dupstr(json_object_get_string(meta, "player_default_actor", ""));
    m->max_party_size       = obj_int(meta, "max_party_size", 0);
    return 0;
}

static void parse_room(HtgRoom *r, const char *id, const JsonValue *o) {
    r->id          = dupstr(id);
    r->name        = dupstr(json_object_get_string(o, "name", ""));
    r->description = dupstr(json_object_get_string(o, "description", ""));
    r->raw         = o;

    /* exits: object of direction -> (string | {to, locked_by_flag}) */
    const JsonValue *exits = json_object_get(o, "exits");
    if (exits && exits->type == JSON_OBJECT) {
        r->exits = (HtgExit *)calloc(exits->as.object.count, sizeof(HtgExit));
        size_t k = 0;
        for (JsonMember *m = exits->as.object.head; m; m = m->next) {
            HtgExit *e = &r->exits[k++];
            e->direction = dupstr(m->key);
            if (m->value && m->value->type == JSON_STRING) {
                e->to = dupstr(m->value->as.string);
                e->locked_by_flag = NULL;
            } else if (m->value && m->value->type == JSON_OBJECT) {
                e->to = dupstr(json_object_get_string(m->value, "to", ""));
                const char *lb = json_object_get_string(m->value, "locked_by_flag", NULL);
                e->locked_by_flag = lb ? dupstr(lb) : NULL;
            } else {
                e->to = dupstr("");
                e->locked_by_flag = NULL;
            }
        }
        r->exit_count = k;
    }

    r->items = dup_string_array(json_object_get(o, "items"), &r->item_count);

    const char *ev = json_object_get_string(o, "on_enter_event", NULL);
    r->on_enter_event = ev ? dupstr(ev) : NULL;

    const JsonValue *enc = json_object_get(o, "encounter");
    if (enc && enc->type == JSON_OBJECT) {
        r->has_encounter = 1;
        r->encounter_chance = obj_int(enc, "chance", 0);
        const char *actor = json_object_get_string(enc, "actor", NULL);
        r->encounter_actor = actor ? dupstr(actor) : NULL;
    }
}

static void parse_item(HtgItem *it, const char *id, const JsonValue *o) {
    it->id          = dupstr(id);
    it->name        = dupstr(json_object_get_string(o, "name", ""));
    it->description = dupstr(json_object_get_string(o, "description", ""));
    it->usable      = json_object_get_bool(o, "usable", 0);
    it->consumable  = json_object_get_bool(o, "consumable", 0);
    it->raw         = o;

    const char *ev = json_object_get_string(o, "on_use_event", NULL);
    it->on_use_event = ev ? dupstr(ev) : NULL;

    const JsonValue *eq = json_object_get(o, "equip");
    if (eq && eq->type == JSON_OBJECT) {
        it->equip.has_equip = 1;
        it->equip.slot      = dupstr(json_object_get_string(eq, "slot", ""));
        it->equip.atk_bonus = obj_int(eq, "atk_bonus", 0);
        it->equip.def_bonus = obj_int(eq, "def_bonus", 0);
        it->equip.spd_bonus = obj_int(eq, "spd_bonus", 0);
        it->equip.hp_bonus  = obj_int(eq, "hp_bonus", 0);
        it->equip.mp_bonus  = obj_int(eq, "mp_bonus", 0);
    }

    const JsonValue *ef = json_object_get(o, "effect");
    if (ef && ef->type == JSON_OBJECT) {
        it->effect.has_effect = 1;
        it->effect.type   = dupstr(json_object_get_string(ef, "type", ""));
        it->effect.amount = obj_int(ef, "amount", 0);
    }
}

static void parse_actor(HtgActor *a, const char *id, const JsonValue *o) {
    a->id   = dupstr(id);
    a->name = dupstr(json_object_get_string(o, "name", ""));
    a->hp   = obj_int(o, "hp", 0);
    a->mp   = obj_int(o, "mp", 0);
    a->atk  = obj_int(o, "atk", 0);
    a->def  = obj_int(o, "def", 0);
    a->spd  = obj_int(o, "spd", 0);
    a->is_enemy    = json_object_get_bool(o, "is_enemy", 0);
    a->recruitable = json_object_get_bool(o, "recruitable", 0);
    a->raw = o;

    a->skills = dup_string_array(json_object_get(o, "skills"), &a->skill_count);
    a->drop   = dup_string_array(json_object_get(o, "drop"), &a->drop_count);

    const JsonValue *eq = json_object_get(o, "equipment");
    if (eq && eq->type == JSON_OBJECT) {
        a->has_equipment = 1;
        const char *w = json_object_get_string(eq, "weapon", NULL);
        const char *r = json_object_get_string(eq, "armor", NULL);
        const char *c = json_object_get_string(eq, "accessory", NULL);
        a->equip_weapon    = w ? dupstr(w) : NULL;
        a->equip_armor     = r ? dupstr(r) : NULL;
        a->equip_accessory = c ? dupstr(c) : NULL;
    }
}

static void parse_skill(HtgSkill *s, const char *id, const JsonValue *o) {
    s->id       = dupstr(id);
    s->name     = dupstr(json_object_get_string(o, "name", ""));
    s->type     = dupstr(json_object_get_string(o, "type", ""));
    s->power    = obj_int(o, "power", 0);
    s->mp_cost  = obj_int(o, "mp_cost", 0);
    s->accuracy = obj_int(o, "accuracy", 100);
    const char *el = json_object_get_string(o, "element", NULL);
    s->element = el ? dupstr(el) : NULL;
    s->raw = o;
}

static void parse_event(HtgEvent *e, const char *id, const JsonValue *o) {
    e->id   = dupstr(id);
    e->type = dupstr(json_object_get_string(o, "type", "auto"));
    e->raw  = o;

    const char *ne = json_object_get_string(o, "next_event", NULL);
    e->next_event = ne ? dupstr(ne) : NULL;

    const JsonValue *lines = json_object_get(o, "lines");
    size_t ln = json_array_size(lines);
    if (ln > 0) {
        e->lines = (HtgEventLine *)calloc(ln, sizeof(HtgEventLine));
        size_t k = 0;
        for (size_t i = 0; i < ln; i++) {
            const JsonValue *l = json_array_get(lines, i);
            if (!l || l->type != JSON_OBJECT) continue;
            HtgEventLine *el = &e->lines[k++];
            el->speaker = dupstr(json_object_get_string(l, "speaker", ""));
            el->text    = dupstr(json_object_get_string(l, "text", ""));
            const char *cond = json_object_get_string(l, "condition", NULL);
            el->condition = cond ? dupstr(cond) : NULL;
        }
        e->line_count = k;
    }

    const JsonValue *choices = json_object_get(o, "choices");
    size_t cn = json_array_size(choices);
    if (cn > 0) {
        e->choices = (HtgChoice *)calloc(cn, sizeof(HtgChoice));
        size_t k = 0;
        for (size_t i = 0; i < cn; i++) {
            const JsonValue *c = json_array_get(choices, i);
            if (!c || c->type != JSON_OBJECT) continue;
            HtgChoice *ch = &e->choices[k++];
            ch->text = dupstr(json_object_get_string(c, "text", ""));
            const char *g  = json_object_get_string(c, "goto", NULL);
            const char *sf = json_object_get_string(c, "set_flag", NULL);
            const char *sv = json_object_get_string(c, "set_var", NULL);
            const char *jp = json_object_get_string(c, "join_party", NULL);
            ch->goto_event = g  ? dupstr(g)  : NULL;
            ch->set_flag   = sf ? dupstr(sf) : NULL;
            ch->set_var    = sv ? dupstr(sv) : NULL;
            ch->join_party = jp ? dupstr(jp) : NULL;
        }
        e->choice_count = k;
    }
}

/* Generic: iterate an object collection and call a per-entry parser.
 * `field` is the plural array member (e.g. rooms); `cnt` is the matching
 * count member (e.g. room_count); `key` is the JSON object key. */
#define PARSE_COLLECTION(field, cnt, jkey, TYPE, parser)                   \
    do {                                                                   \
        const JsonValue *coll = json_object_get(root, jkey);               \
        if (coll && coll->type == JSON_OBJECT && coll->as.object.count) {  \
            p->field = (TYPE *)calloc(coll->as.object.count, sizeof(TYPE));\
            if (!p->field) { if (err) *err = "out of memory"; goto fail; } \
            size_t k = 0;                                                  \
            for (JsonMember *m = coll->as.object.head; m; m = m->next) {   \
                if (m->value && m->value->type == JSON_OBJECT)             \
                    parser(&p->field[k++], m->key, m->value);              \
            }                                                              \
            p->cnt = k;                                                    \
        }                                                                  \
    } while (0)

/* ==========================================================================
 * Build / free project
 * ========================================================================== */

HtgProject *htg_project_from_json(JsonValue *root, const char **err) {
    if (err) *err = NULL;
    if (!root || root->type != JSON_OBJECT) {
        if (err) *err = "project root must be a JSON object";
        return NULL;
    }

    HtgProject *p = (HtgProject *)calloc(1, sizeof(HtgProject));
    if (!p) { if (err) *err = "out of memory"; return NULL; }
    p->source = root;

    parse_meta(&p->meta, root);

    const JsonValue *flags = json_object_get(root, "flags");
    if (flags && flags->type == JSON_OBJECT) p->flags = json_clone(flags);
    const JsonValue *vars = json_object_get(root, "vars");
    if (vars && vars->type == JSON_OBJECT) p->vars = json_clone(vars);

    PARSE_COLLECTION(rooms,  room_count,  "rooms",  HtgRoom,  parse_room);
    PARSE_COLLECTION(items,  item_count,  "items",  HtgItem,  parse_item);
    PARSE_COLLECTION(actors, actor_count, "actors", HtgActor, parse_actor);
    PARSE_COLLECTION(skills, skill_count, "skills", HtgSkill, parse_skill);
    PARSE_COLLECTION(events, event_count, "events", HtgEvent, parse_event);

    return p;

fail:
    /* p->source is root; caller keeps ownership of root on failure, so detach. */
    p->source = NULL;
    htg_project_free(p);
    return NULL;
}

HtgProject *htg_project_load(const char *path, const char **err) {
    JsonValue *root = json_parse_file(path, err);
    if (!root) return NULL;
    HtgProject *p = htg_project_from_json(root, err);
    if (!p) { json_free(root); return NULL; }
    return p;
}

int htg_project_save(const HtgProject *p, const char *path) {
    if (!p || !p->source) return -1;
    return json_serialize_file(p->source, path, 1);
}

char *htg_project_to_string(const HtgProject *p) {
    if (!p || !p->source) return NULL;
    return json_serialize(p->source, 1);
}

static void free_room(HtgRoom *r) {
    free(r->id); free(r->name); free(r->description);
    for (size_t i = 0; i < r->exit_count; i++) {
        free(r->exits[i].direction);
        free(r->exits[i].to);
        free(r->exits[i].locked_by_flag);
    }
    free(r->exits);
    free_string_array(r->items, r->item_count);
    free(r->on_enter_event);
    free(r->encounter_actor);
}

static void free_item(HtgItem *it) {
    free(it->id); free(it->name); free(it->description);
    free(it->on_use_event);
    free(it->equip.slot);
    free(it->effect.type);
}

static void free_actor(HtgActor *a) {
    free(a->id); free(a->name);
    free_string_array(a->skills, a->skill_count);
    free_string_array(a->drop, a->drop_count);
    free(a->equip_weapon); free(a->equip_armor); free(a->equip_accessory);
}

static void free_skill(HtgSkill *s) {
    free(s->id); free(s->name); free(s->type); free(s->element);
}

static void free_event(HtgEvent *e) {
    free(e->id); free(e->type); free(e->next_event);
    for (size_t i = 0; i < e->line_count; i++) {
        free(e->lines[i].speaker);
        free(e->lines[i].text);
        free(e->lines[i].condition);
    }
    free(e->lines);
    for (size_t i = 0; i < e->choice_count; i++) {
        free(e->choices[i].text);
        free(e->choices[i].goto_event);
        free(e->choices[i].set_flag);
        free(e->choices[i].set_var);
        free(e->choices[i].join_party);
    }
    free(e->choices);
}

void htg_project_free(HtgProject *p) {
    if (!p) return;
    free(p->meta.title);
    free(p->meta.version);
    free(p->meta.start_room);
    free(p->meta.player_default_actor);
    json_free(p->flags);
    json_free(p->vars);

    for (size_t i = 0; i < p->room_count; i++)  free_room(&p->rooms[i]);
    for (size_t i = 0; i < p->item_count; i++)  free_item(&p->items[i]);
    for (size_t i = 0; i < p->actor_count; i++) free_actor(&p->actors[i]);
    for (size_t i = 0; i < p->skill_count; i++) free_skill(&p->skills[i]);
    for (size_t i = 0; i < p->event_count; i++) free_event(&p->events[i]);
    free(p->rooms); free(p->items); free(p->actors);
    free(p->skills); free(p->events);

    json_free(p->source);
    free(p);
}

/* ==========================================================================
 * Lookups
 * ========================================================================== */

HtgRoom *htg_find_room(HtgProject *p, const char *id) {
    if (!p || !id) return NULL;
    for (size_t i = 0; i < p->room_count; i++)
        if (p->rooms[i].id && strcmp(p->rooms[i].id, id) == 0) return &p->rooms[i];
    return NULL;
}
HtgItem *htg_find_item(HtgProject *p, const char *id) {
    if (!p || !id) return NULL;
    for (size_t i = 0; i < p->item_count; i++)
        if (p->items[i].id && strcmp(p->items[i].id, id) == 0) return &p->items[i];
    return NULL;
}
HtgActor *htg_find_actor(HtgProject *p, const char *id) {
    if (!p || !id) return NULL;
    for (size_t i = 0; i < p->actor_count; i++)
        if (p->actors[i].id && strcmp(p->actors[i].id, id) == 0) return &p->actors[i];
    return NULL;
}
HtgSkill *htg_find_skill(HtgProject *p, const char *id) {
    if (!p || !id) return NULL;
    for (size_t i = 0; i < p->skill_count; i++)
        if (p->skills[i].id && strcmp(p->skills[i].id, id) == 0) return &p->skills[i];
    return NULL;
}
HtgEvent *htg_find_event(HtgProject *p, const char *id) {
    if (!p || !id) return NULL;
    for (size_t i = 0; i < p->event_count; i++)
        if (p->events[i].id && strcmp(p->events[i].id, id) == 0) return &p->events[i];
    return NULL;
}

/* ==========================================================================
 * Skeleton for `htg new`
 * ========================================================================== */

JsonValue *htg_new_skeleton(const char *title, const char *start_room) {
    if (!title || !*title) title = "Untitled";
    if (!start_room || !*start_room) start_room = "room_start";

    JsonValue *root = json_new_object();
    if (!root) return NULL;

    JsonValue *meta = json_new_object();
    json_object_set(meta, "title", json_new_string(title));
    json_object_set(meta, "version", json_new_string("1.0"));
    json_object_set(meta, "start_room", json_new_string(start_room));
    json_object_set(meta, "player_default_actor", json_new_string("player_default"));
    json_object_set(meta, "max_party_size", json_new_number(3));
    json_object_set(root, "meta", meta);

    json_object_set(root, "flags", json_new_object());
    json_object_set(root, "vars", json_new_object());

    /* A single starting room so the project is immediately runnable/editable. */
    JsonValue *rooms = json_new_object();
    JsonValue *room = json_new_object();
    json_object_set(room, "name", json_new_string("はじまりの部屋"));
    json_object_set(room, "description", json_new_string("何もない部屋だ。"));
    json_object_set(room, "exits", json_new_object());
    json_object_set(room, "items", json_new_array());
    json_object_set(rooms, start_room, room);
    json_object_set(root, "rooms", rooms);

    json_object_set(root, "items", json_new_object());

    /* A default player actor with empty equipment slots. */
    JsonValue *actors = json_new_object();
    JsonValue *player = json_new_object();
    json_object_set(player, "name", json_new_string("主人公"));
    json_object_set(player, "hp", json_new_number(100));
    json_object_set(player, "mp", json_new_number(30));
    json_object_set(player, "atk", json_new_number(10));
    json_object_set(player, "def", json_new_number(5));
    json_object_set(player, "spd", json_new_number(6));
    json_object_set(player, "skills", json_new_array());
    json_object_set(player, "is_enemy", json_new_bool(0));
    JsonValue *equipment = json_new_object();
    json_object_set(equipment, "weapon", json_new_null());
    json_object_set(equipment, "armor", json_new_null());
    json_object_set(equipment, "accessory", json_new_null());
    json_object_set(player, "equipment", equipment);
    json_object_set(actors, "player_default", player);
    json_object_set(root, "actors", actors);

    json_object_set(root, "skills", json_new_object());
    json_object_set(root, "events", json_new_object());

    return root;
}
