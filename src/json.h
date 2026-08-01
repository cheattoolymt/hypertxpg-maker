/*
 * json.h - Minimal self-contained JSON parser/serializer for HyperTxPG (htg).
 *
 * No external dependencies; uses only POSIX standard C library.
 * This is not a fully spec-compliant JSON implementation, but it is enough
 * to read and write the .htgp project schema described in the htg spec:
 *   - objects, arrays, strings, numbers (int/double), booleans, null
 *   - UTF-8 passthrough (strings are stored/emitted verbatim byte-for-byte)
 *   - \uXXXX escapes are decoded to UTF-8 on parse and re-emitted as raw UTF-8
 *
 * Ownership model: json_parse() returns a heap-allocated tree. Free it with
 * json_free(). Builder helpers (json_new_*, json_object_set, json_array_add)
 * transfer ownership of children into their parent; freeing the root frees all.
 */
#ifndef HTG_JSON_H
#define HTG_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

/* A key/value member of a JSON object (kept in insertion order). */
typedef struct JsonMember {
    char *key;               /* NUL-terminated, heap-owned */
    JsonValue *value;        /* heap-owned */
    struct JsonMember *next; /* linked list preserves order */
} JsonMember;

struct JsonValue {
    JsonType type;
    union {
        int boolean;          /* JSON_BOOL: 0/1 */
        double number;        /* JSON_NUMBER */
        char *string;         /* JSON_STRING: heap-owned, NUL-terminated */
        struct {              /* JSON_ARRAY */
            JsonValue **items;
            size_t count;
            size_t cap;
        } array;
        struct {              /* JSON_OBJECT */
            JsonMember *head;
            JsonMember *tail;
            size_t count;
        } object;
    } as;
};

/* ---- Parsing ---- */

/*
 * Parse a NUL-terminated JSON text buffer.
 * On success returns a heap-allocated tree (free with json_free).
 * On failure returns NULL; if err is non-NULL it is filled with a short
 * human-readable message (points into a static buffer, do not free).
 */
JsonValue *json_parse(const char *text, const char **err);

/* Parse a file's contents. Returns NULL on read or parse failure. */
JsonValue *json_parse_file(const char *path, const char **err);

/* ---- Serializing ---- */

/*
 * Serialize a tree to a newly heap-allocated NUL-terminated string.
 * If pretty is non-zero, output is indented with two spaces per level.
 * Returns NULL on allocation failure. Caller frees the returned buffer.
 */
char *json_serialize(const JsonValue *v, int pretty);

/* Serialize and write to a file. Returns 0 on success, non-zero on error. */
int json_serialize_file(const JsonValue *v, const char *path, int pretty);

/* ---- Memory ---- */

void json_free(JsonValue *v);

/* ---- Builders ---- */

JsonValue *json_new_null(void);
JsonValue *json_new_bool(int b);
JsonValue *json_new_number(double n);
JsonValue *json_new_string(const char *s);   /* copies s */
JsonValue *json_new_array(void);
JsonValue *json_new_object(void);

/* Append item to array; array takes ownership. Returns 0 on success. */
int json_array_add(JsonValue *arr, JsonValue *item);

/*
 * Set object[key] = value; object takes ownership of value and copies key.
 * If key already exists its old value is freed and replaced.
 * Returns 0 on success.
 */
int json_object_set(JsonValue *obj, const char *key, JsonValue *value);

/* ---- Accessors (return NULL / defaults when type mismatches) ---- */

/* Object member lookup by key; returns NULL if absent or not an object. */
JsonValue *json_object_get(const JsonValue *obj, const char *key);

/*
 * Remove object[key], freeing the stored value. Returns 0 if a member was
 * removed, non-zero if the key was absent or obj is not an object. Preserves
 * the insertion order of the remaining members.
 */
int json_object_remove(JsonValue *obj, const char *key);

/*
 * Return the key of the object member at position `index` (0-based, in
 * insertion order), or NULL if out of range / not an object. The returned
 * pointer is owned by the object; do not free it.
 */
const char *json_object_key_at(const JsonValue *obj, size_t index);

/* Number of members in an object (0 if not an object). */
size_t json_object_size(const JsonValue *obj);

/* Array element by index; returns NULL if out of range or not an array. */
JsonValue *json_array_get(const JsonValue *arr, size_t index);
size_t json_array_size(const JsonValue *arr);

/* Convenience typed getters with defaults for missing/mismatched values. */
const char *json_get_string(const JsonValue *v, const char *def);
double      json_get_number(const JsonValue *v, double def);
int         json_get_bool(const JsonValue *v, int def);

/* Convenience: fetch object[key] then coerce, with default fallback. */
const char *json_object_get_string(const JsonValue *obj, const char *key, const char *def);
double      json_object_get_number(const JsonValue *obj, const char *key, double def);
int         json_object_get_bool(const JsonValue *obj, const char *key, int def);

#endif /* HTG_JSON_H */
