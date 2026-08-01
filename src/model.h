/*
 * model.h - HyperTxPG (htg) data model (spec section 2).
 *
 * These structs mirror the .htgp JSON schema. The model owns all of its
 * strings and child collections; free the whole thing with htg_project_free().
 *
 * Design notes:
 *  - Collections (rooms/items/actors/skills/events) are simple dynamic arrays
 *    of entries keyed by their string id, preserving the insertion/file order.
 *  - To stay resilient against future schema additions handled by later
 *    sections (battle, party, events runtime, etc.), every entry also keeps a
 *    borrowed pointer `raw` into the original parsed JSON so that fields not
 *    yet promoted to typed struct members can still be round-tripped or read.
 *    The typed fields below cover exactly what spec sections 1-2 describe.
 */
#ifndef HTG_MODEL_H
#define HTG_MODEL_H

#include "json.h"

/* ---- meta ---- */
typedef struct {
    char *title;
    char *version;
    char *start_room;
    char *player_default_actor;
    int   max_party_size;      /* 0 if unspecified */
} HtgMeta;

/* ---- flags / vars are kept as generic JSON objects (name->value) ---- */

/* ---- room exit ---- */
typedef struct {
    char *direction;           /* e.g. "north" */
    char *to;                  /* destination room id */
    char *locked_by_flag;      /* NULL if unconditional */
} HtgExit;

typedef struct {
    char *id;
    char *name;
    char *description;
    HtgExit *exits;
    size_t   exit_count;
    char   **items;            /* array of item ids */
    size_t   item_count;
    char    *on_enter_event;   /* event id or NULL */
    int      has_encounter;
    int      encounter_chance; /* 0-100 */
    char    *encounter_actor;  /* actor id or NULL */
    const JsonValue *raw;      /* borrowed */
} HtgRoom;

/* ---- item ---- */
typedef struct {
    int has_equip;
    char *slot;                /* weapon/armor/accessory */
    int atk_bonus, def_bonus, spd_bonus, hp_bonus, mp_bonus;
} HtgEquip;

typedef struct {
    int has_effect;
    char *type;                /* heal_hp / heal_mp / buff_atk ... */
    int   amount;
} HtgEffect;

typedef struct {
    char *id;
    char *name;
    char *description;
    int   usable;
    int   consumable;
    char *on_use_event;        /* event id or NULL */
    HtgEquip  equip;
    HtgEffect effect;
    const JsonValue *raw;      /* borrowed */
} HtgItem;

/* ---- actor ---- */
typedef struct {
    char *id;
    char *name;
    int   hp, mp, atk, def, spd;
    char **skills;
    size_t skill_count;
    char **drop;
    size_t drop_count;
    int   is_enemy;
    int   recruitable;
    int   has_equipment;
    char *equip_weapon;        /* item id or NULL */
    char *equip_armor;
    char *equip_accessory;
    const JsonValue *raw;      /* borrowed */
} HtgActor;

/* ---- skill ---- */
typedef struct {
    char *id;
    char *name;
    char *type;                /* e.g. magic / physical */
    int   power;
    int   mp_cost;
    int   accuracy;
    char *element;             /* or NULL */
    const JsonValue *raw;      /* borrowed */
} HtgSkill;

/* ---- event ---- */
typedef struct {
    char *speaker;
    char *text;
    char *condition;           /* or NULL */
} HtgEventLine;

typedef struct {
    char *text;
    char *goto_event;          /* or NULL */
    char *set_flag;            /* or NULL */
    char *set_var;             /* raw "name=value" style string or NULL */
    char *join_party;          /* actor id or NULL */
} HtgChoice;

typedef struct {
    char *id;
    char *type;                /* "auto" / "choice" */
    HtgEventLine *lines;
    size_t line_count;
    HtgChoice   *choices;
    size_t choice_count;
    char *next_event;          /* or NULL */
    const JsonValue *raw;      /* borrowed */
} HtgEvent;

/* ---- project ---- */
typedef struct {
    HtgMeta meta;

    /* flags/vars preserved as JSON objects for faithful round-trip. */
    JsonValue *flags;          /* owned clone (JSON object) or NULL */
    JsonValue *vars;           /* owned clone (JSON object) or NULL */

    HtgRoom  *rooms;   size_t room_count;
    HtgItem  *items;   size_t item_count;
    HtgActor *actors;  size_t actor_count;
    HtgSkill *skills;  size_t skill_count;
    HtgEvent *events;  size_t event_count;

    /* The parsed source tree is retained so `raw` borrows stay valid and so
     * that unmodelled fields survive a load/save cycle. Owned. */
    JsonValue *source;
} HtgProject;

/* ---- API ---- */

/*
 * Load a project from a .htgp (JSON) file.
 * Returns a heap-allocated HtgProject (free with htg_project_free), or NULL
 * on error (err, if non-NULL, receives a short message).
 */
HtgProject *htg_project_load(const char *path, const char **err);

/* Build a project directly from an already-parsed JSON tree. Takes ownership
 * of `root` on success (it becomes project->source). On failure returns NULL
 * and does NOT free root. */
HtgProject *htg_project_from_json(JsonValue *root, const char **err);

/* Serialize the project's source tree back to a .htgp file (pretty-printed).
 * Returns 0 on success. */
int htg_project_save(const HtgProject *p, const char *path);

/* Serialize the project's source tree to a heap string (pretty). */
char *htg_project_to_string(const HtgProject *p);

void htg_project_free(HtgProject *p);

/* Lookups by id (linear scan). Return NULL when not found. */
HtgRoom  *htg_find_room(HtgProject *p, const char *id);
HtgItem  *htg_find_item(HtgProject *p, const char *id);
HtgActor *htg_find_actor(HtgProject *p, const char *id);
HtgSkill *htg_find_skill(HtgProject *p, const char *id);
HtgEvent *htg_find_event(HtgProject *p, const char *id);

/* Create a minimal, valid, empty project skeleton in JSON form (used by
 * `htg new`). Caller owns the returned tree. */
JsonValue *htg_new_skeleton(const char *title, const char *start_room);

#endif /* HTG_MODEL_H */
