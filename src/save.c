/*
 * save.c - Save / load of live gameplay state (spec section 6). Standard C only.
 *
 * The save document is built with the project's own JSON builders so it stays
 * a plain, human-readable JSON file independent of the .htgp project.
 */
#include "save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

static char *dupstr(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    if (r) memcpy(r, s, n + 1);
    return r;
}

/* Deep-clone a JSON value (objects/arrays/scalars). Returns NULL on NULL in. */
static JsonValue *json_clone(const JsonValue *v) {
    if (!v) return NULL;
    switch (v->type) {
        case JSON_NULL:   return json_new_null();
        case JSON_BOOL:   return json_new_bool(v->as.boolean);
        case JSON_NUMBER: return json_new_number(v->as.number);
        case JSON_STRING: return json_new_string(v->as.string);
        case JSON_ARRAY: {
            JsonValue *a = json_new_array();
            for (size_t i = 0; i < v->as.array.count; i++)
                json_array_add(a, json_clone(v->as.array.items[i]));
            return a;
        }
        case JSON_OBJECT: {
            JsonValue *o = json_new_object();
            for (JsonMember *m = v->as.object.head; m; m = m->next)
                json_object_set(o, m->key, json_clone(m->value));
            return o;
        }
    }
    return json_new_null();
}

/* ==========================================================================
 * Save
 * ========================================================================== */

int htg_save_game(const Game *g, const char *path) {
    if (!g || !path) return 1;

    JsonValue *root = json_new_object();
    if (!root) return 1;

    json_object_set(root, "htg_save", json_new_number(1));
    json_object_set(root, "project_title",
                    json_new_string(g->p->meta.title ? g->p->meta.title : ""));
    json_object_set(root, "current_room",
                    json_new_string(g->current_room ? g->current_room : ""));

    /* inventory */
    JsonValue *inv = json_new_array();
    for (size_t i = 0; i < g->inv_count; i++)
        json_array_add(inv, json_new_string(g->inventory[i]));
    json_object_set(root, "inventory", inv);

    /* flags / vars snapshots */
    json_object_set(root, "flags",
                    g->p->flags ? json_clone(g->p->flags) : json_new_object());
    json_object_set(root, "vars",
                    g->p->vars ? json_clone(g->p->vars) : json_new_object());

    /* party */
    JsonValue *party = json_new_array();
    for (size_t i = 0; i < g->party_count; i++) {
        const HtgMember *m = &g->party[i];
        JsonValue *pm = json_new_object();
        json_object_set(pm, "actor", json_new_string(m->actor_id ? m->actor_id : ""));
        json_object_set(pm, "hp", json_new_number(m->hp));
        json_object_set(pm, "mp", json_new_number(m->mp));
        json_object_set(pm, "weapon",
                        m->equip_weapon ? json_new_string(m->equip_weapon) : json_new_null());
        json_object_set(pm, "armor",
                        m->equip_armor ? json_new_string(m->equip_armor) : json_new_null());
        json_object_set(pm, "accessory",
                        m->equip_accessory ? json_new_string(m->equip_accessory) : json_new_null());
        json_array_add(party, pm);
    }
    json_object_set(root, "party", party);

    int rc = json_serialize_file(root, path, 1);
    json_free(root);
    return rc;
}

/* ==========================================================================
 * Load
 * ========================================================================== */

/* Free the owned strings held by one party member. */
static void member_free(HtgMember *m) {
    free(m->actor_id);
    free(m->equip_weapon);
    free(m->equip_armor);
    free(m->equip_accessory);
    memset(m, 0, sizeof(*m));
}

/* Copy a nullable string field from a save object member. */
static char *opt_str(const JsonValue *obj, const char *key) {
    JsonValue *v = json_object_get(obj, key);
    if (!v || v->type != JSON_STRING) return NULL;
    return dupstr(v->as.string);
}

int htg_load_game(Game *g, const char *path) {
    if (!g || !path) return 1;

    const char *err = NULL;
    JsonValue *root = json_parse_file(path, &err);
    if (!root || root->type != JSON_OBJECT) {
        if (root) json_free(root);
        return 1;
    }

    const char *room = json_object_get_string(root, "current_room", NULL);
    if (!room || !*room || !htg_find_room(g->p, room)) {
        json_free(root);
        return 1;
    }

    /* ---- point of no return: clear existing mutable state ---- */
    for (size_t i = 0; i < g->inv_count; i++) free(g->inventory[i]);
    g->inv_count = 0;
    for (size_t i = 0; i < g->party_count; i++) member_free(&g->party[i]);
    g->party_count = 0;

    /* current room: match against a room struct id for a stable borrow. */
    HtgRoom *rr = htg_find_room(g->p, room);
    g->current_room = rr ? rr->id : g->p->meta.start_room;

    /* inventory */
    JsonValue *inv = json_object_get(root, "inventory");
    if (inv && inv->type == JSON_ARRAY) {
        for (size_t i = 0; i < inv->as.array.count && g->inv_count < HTG_MAX_INV; i++) {
            JsonValue *e = inv->as.array.items[i];
            if (e && e->type == JSON_STRING)
                g->inventory[g->inv_count++] = dupstr(e->as.string);
        }
    }

    /* flags / vars: replace the live snapshots wholesale. */
    JsonValue *flags = json_object_get(root, "flags");
    if (flags && flags->type == JSON_OBJECT) {
        json_free(g->p->flags);
        g->p->flags = json_clone(flags);
    }
    JsonValue *vars = json_object_get(root, "vars");
    if (vars && vars->type == JSON_OBJECT) {
        json_free(g->p->vars);
        g->p->vars = json_clone(vars);
    }

    /* party */
    JsonValue *party = json_object_get(root, "party");
    if (party && party->type == JSON_ARRAY) {
        for (size_t i = 0; i < party->as.array.count && g->party_count < HTG_MAX_PARTY; i++) {
            JsonValue *pm = party->as.array.items[i];
            if (!pm || pm->type != JSON_OBJECT) continue;
            const char *actor = json_object_get_string(pm, "actor", NULL);
            if (!actor || !*actor) continue;
            HtgMember *m = &g->party[g->party_count++];
            memset(m, 0, sizeof(*m));
            m->actor_id = dupstr(actor);
            m->hp = (int)json_object_get_number(pm, "hp", 0);
            m->mp = (int)json_object_get_number(pm, "mp", 0);
            m->equip_weapon    = opt_str(pm, "weapon");
            m->equip_armor     = opt_str(pm, "armor");
            m->equip_accessory = opt_str(pm, "accessory");
        }
    }

    json_free(root);
    return 0;
}
