/*
 * json.c - Implementation of the minimal JSON parser/serializer (see json.h).
 * Standard C only (C11), no external dependencies.
 */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ==========================================================================
 * Value constructors / destructor
 * ========================================================================== */

static JsonValue *alloc_value(JsonType t) {
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));
    if (v) v->type = t;
    return v;
}

JsonValue *json_new_null(void)        { return alloc_value(JSON_NULL); }

JsonValue *json_new_bool(int b) {
    JsonValue *v = alloc_value(JSON_BOOL);
    if (v) v->as.boolean = b ? 1 : 0;
    return v;
}

JsonValue *json_new_number(double n) {
    JsonValue *v = alloc_value(JSON_NUMBER);
    if (v) v->as.number = n;
    return v;
}

JsonValue *json_new_string(const char *s) {
    JsonValue *v = alloc_value(JSON_STRING);
    if (!v) return NULL;
    if (!s) s = "";
    size_t n = strlen(s);
    v->as.string = (char *)malloc(n + 1);
    if (!v->as.string) { free(v); return NULL; }
    memcpy(v->as.string, s, n + 1);
    return v;
}

JsonValue *json_new_array(void)  { return alloc_value(JSON_ARRAY); }
JsonValue *json_new_object(void) { return alloc_value(JSON_OBJECT); }

void json_free(JsonValue *v) {
    if (!v) return;
    switch (v->type) {
        case JSON_STRING:
            free(v->as.string);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < v->as.array.count; i++)
                json_free(v->as.array.items[i]);
            free(v->as.array.items);
            break;
        case JSON_OBJECT: {
            JsonMember *m = v->as.object.head;
            while (m) {
                JsonMember *next = m->next;
                free(m->key);
                json_free(m->value);
                free(m);
                m = next;
            }
            break;
        }
        default:
            break;
    }
    free(v);
}

/* ==========================================================================
 * Builders
 * ========================================================================== */

int json_array_add(JsonValue *arr, JsonValue *item) {
    if (!arr || arr->type != JSON_ARRAY || !item) return -1;
    if (arr->as.array.count == arr->as.array.cap) {
        size_t ncap = arr->as.array.cap ? arr->as.array.cap * 2 : 4;
        JsonValue **ni = (JsonValue **)realloc(arr->as.array.items,
                                               ncap * sizeof(JsonValue *));
        if (!ni) return -1;
        arr->as.array.items = ni;
        arr->as.array.cap = ncap;
    }
    arr->as.array.items[arr->as.array.count++] = item;
    return 0;
}

int json_object_set(JsonValue *obj, const char *key, JsonValue *value) {
    if (!obj || obj->type != JSON_OBJECT || !key || !value) return -1;

    /* Replace existing key if present. */
    for (JsonMember *m = obj->as.object.head; m; m = m->next) {
        if (strcmp(m->key, key) == 0) {
            json_free(m->value);
            m->value = value;
            return 0;
        }
    }

    JsonMember *m = (JsonMember *)calloc(1, sizeof(JsonMember));
    if (!m) return -1;
    size_t klen = strlen(key);
    m->key = (char *)malloc(klen + 1);
    if (!m->key) { free(m); return -1; }
    memcpy(m->key, key, klen + 1);
    m->value = value;
    m->next = NULL;

    if (obj->as.object.tail)
        obj->as.object.tail->next = m;
    else
        obj->as.object.head = m;
    obj->as.object.tail = m;
    obj->as.object.count++;
    return 0;
}

/* ==========================================================================
 * Accessors
 * ========================================================================== */

JsonValue *json_object_get(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    for (JsonMember *m = obj->as.object.head; m; m = m->next)
        if (strcmp(m->key, key) == 0) return m->value;
    return NULL;
}

int json_object_remove(JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return -1;
    JsonMember *prev = NULL;
    for (JsonMember *m = obj->as.object.head; m; prev = m, m = m->next) {
        if (strcmp(m->key, key) == 0) {
            if (prev) prev->next = m->next;
            else obj->as.object.head = m->next;
            if (obj->as.object.tail == m) obj->as.object.tail = prev;
            free(m->key);
            json_free(m->value);
            free(m);
            obj->as.object.count--;
            return 0;
        }
    }
    return -1;
}

const char *json_object_key_at(const JsonValue *obj, size_t index) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    size_t i = 0;
    for (JsonMember *m = obj->as.object.head; m; m = m->next, i++)
        if (i == index) return m->key;
    return NULL;
}

size_t json_object_size(const JsonValue *obj) {
    if (!obj || obj->type != JSON_OBJECT) return 0;
    return obj->as.object.count;
}

JsonValue *json_array_get(const JsonValue *arr, size_t index) {
    if (!arr || arr->type != JSON_ARRAY || index >= arr->as.array.count)
        return NULL;
    return arr->as.array.items[index];
}

size_t json_array_size(const JsonValue *arr) {
    if (!arr || arr->type != JSON_ARRAY) return 0;
    return arr->as.array.count;
}

const char *json_get_string(const JsonValue *v, const char *def) {
    if (v && v->type == JSON_STRING) return v->as.string;
    return def;
}

double json_get_number(const JsonValue *v, double def) {
    if (v && v->type == JSON_NUMBER) return v->as.number;
    if (v && v->type == JSON_BOOL)   return v->as.boolean ? 1.0 : 0.0;
    return def;
}

int json_get_bool(const JsonValue *v, int def) {
    if (v && v->type == JSON_BOOL)   return v->as.boolean;
    if (v && v->type == JSON_NUMBER) return v->as.number != 0.0;
    return def;
}

const char *json_object_get_string(const JsonValue *obj, const char *key, const char *def) {
    return json_get_string(json_object_get(obj, key), def);
}
double json_object_get_number(const JsonValue *obj, const char *key, double def) {
    return json_get_number(json_object_get(obj, key), def);
}
int json_object_get_bool(const JsonValue *obj, const char *key, int def) {
    return json_get_bool(json_object_get(obj, key), def);
}

/* ==========================================================================
 * Parser
 * ========================================================================== */

typedef struct {
    const char *p;      /* current position */
    const char *end;    /* one past last byte */
    const char *err;    /* error message, or NULL */
} Parser;

static void set_err(Parser *ps, const char *msg) {
    if (!ps->err) ps->err = msg;
}

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ps->p++;
        else
            break;
    }
}

static JsonValue *parse_value(Parser *ps);

/* Encode a Unicode code point as UTF-8 into out (up to 4 bytes).
 * Returns number of bytes written. */
static int utf8_encode(unsigned int cp, char *out) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Read 4 hex digits at ps->p (which must point just past \u); advances p. */
static int parse_hex4(Parser *ps, unsigned int *out) {
    if (ps->end - ps->p < 4) return -1;
    unsigned int v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_val(ps->p[i]);
        if (h < 0) return -1;
        v = (v << 4) | (unsigned int)h;
    }
    ps->p += 4;
    *out = v;
    return 0;
}

/* Parse a JSON string (ps->p must point at opening quote). */
static char *parse_string_raw(Parser *ps) {
    if (ps->p >= ps->end || *ps->p != '"') {
        set_err(ps, "expected string");
        return NULL;
    }
    ps->p++; /* skip opening quote */

    size_t cap = 16, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { set_err(ps, "out of memory"); return NULL; }

#define APPEND_BYTE(b) do {                              \
        if (len + 1 >= cap) {                            \
            size_t nc = cap * 2;                         \
            char *nb = (char *)realloc(buf, nc);         \
            if (!nb) { free(buf); set_err(ps,"out of memory"); return NULL; } \
            buf = nb; cap = nc;                          \
        }                                                \
        buf[len++] = (char)(b);                          \
    } while (0)

    while (ps->p < ps->end) {
        char c = *ps->p++;
        if (c == '"') {
            buf[len] = '\0';
            return buf;
        } else if (c == '\\') {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            switch (e) {
                case '"':  APPEND_BYTE('"');  break;
                case '\\': APPEND_BYTE('\\'); break;
                case '/':  APPEND_BYTE('/');  break;
                case 'b':  APPEND_BYTE('\b'); break;
                case 'f':  APPEND_BYTE('\f'); break;
                case 'n':  APPEND_BYTE('\n'); break;
                case 'r':  APPEND_BYTE('\r'); break;
                case 't':  APPEND_BYTE('\t'); break;
                case 'u': {
                    unsigned int cp;
                    if (parse_hex4(ps, &cp) != 0) {
                        free(buf); set_err(ps, "bad \\u escape"); return NULL;
                    }
                    /* Handle surrogate pairs. */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (ps->end - ps->p >= 2 &&
                            ps->p[0] == '\\' && ps->p[1] == 'u') {
                            ps->p += 2;
                            unsigned int lo;
                            if (parse_hex4(ps, &lo) != 0) {
                                free(buf); set_err(ps, "bad \\u escape"); return NULL;
                            }
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 +
                                     (((cp - 0xD800) << 10) | (lo - 0xDC00));
                            } else {
                                /* invalid low surrogate; emit replacement */
                                cp = 0xFFFD;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    }
                    char tmp[4];
                    int n = utf8_encode(cp, tmp);
                    for (int i = 0; i < n; i++) APPEND_BYTE(tmp[i]);
                    break;
                }
                default:
                    free(buf); set_err(ps, "bad escape"); return NULL;
            }
        } else {
            /* Raw byte (includes UTF-8 multibyte sequences). */
            APPEND_BYTE(c);
        }
    }
#undef APPEND_BYTE
    free(buf);
    set_err(ps, "unterminated string");
    return NULL;
}

static JsonValue *parse_string(Parser *ps) {
    char *s = parse_string_raw(ps);
    if (!s) return NULL;
    JsonValue *v = alloc_value(JSON_STRING);
    if (!v) { free(s); set_err(ps, "out of memory"); return NULL; }
    v->as.string = s;
    return v;
}

static JsonValue *parse_number(Parser *ps) {
    const char *start = ps->p;
    if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) ps->p++;
    while (ps->p < ps->end &&
           (isdigit((unsigned char)*ps->p) || *ps->p == '.' ||
            *ps->p == 'e' || *ps->p == 'E' ||
            *ps->p == '+' || *ps->p == '-')) {
        ps->p++;
    }
    size_t n = (size_t)(ps->p - start);
    if (n == 0) { set_err(ps, "expected number"); return NULL; }

    char tmp[64];
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, start, n);
    tmp[n] = '\0';

    char *endp = NULL;
    double d = strtod(tmp, &endp);
    if (endp == tmp) { set_err(ps, "bad number"); return NULL; }
    return json_new_number(d);
}

static int match_literal(Parser *ps, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n) return 0;
    if (memcmp(ps->p, lit, n) != 0) return 0;
    ps->p += n;
    return 1;
}

static JsonValue *parse_array(Parser *ps) {
    ps->p++; /* skip '[' */
    JsonValue *arr = json_new_array();
    if (!arr) { set_err(ps, "out of memory"); return NULL; }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') { ps->p++; return arr; }
    while (1) {
        skip_ws(ps);
        JsonValue *item = parse_value(ps);
        if (!item) { json_free(arr); return NULL; }
        if (json_array_add(arr, item) != 0) {
            json_free(item); json_free(arr);
            set_err(ps, "out of memory");
            return NULL;
        }
        skip_ws(ps);
        if (ps->p >= ps->end) { json_free(arr); set_err(ps, "unterminated array"); return NULL; }
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ']') { ps->p++; return arr; }
        json_free(arr);
        set_err(ps, "expected ',' or ']' in array");
        return NULL;
    }
}

static JsonValue *parse_object(Parser *ps) {
    ps->p++; /* skip '{' */
    JsonValue *obj = json_new_object();
    if (!obj) { set_err(ps, "out of memory"); return NULL; }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') { ps->p++; return obj; }
    while (1) {
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            json_free(obj); set_err(ps, "expected object key"); return NULL;
        }
        char *key = parse_string_raw(ps);
        if (!key) { json_free(obj); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            free(key); json_free(obj); set_err(ps, "expected ':'"); return NULL;
        }
        ps->p++; /* skip ':' */
        skip_ws(ps);
        JsonValue *val = parse_value(ps);
        if (!val) { free(key); json_free(obj); return NULL; }
        if (json_object_set(obj, key, val) != 0) {
            free(key); json_free(val); json_free(obj);
            set_err(ps, "out of memory");
            return NULL;
        }
        free(key);
        skip_ws(ps);
        if (ps->p >= ps->end) { json_free(obj); set_err(ps, "unterminated object"); return NULL; }
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return obj; }
        json_free(obj);
        set_err(ps, "expected ',' or '}' in object");
        return NULL;
    }
}

static JsonValue *parse_value(Parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) { set_err(ps, "unexpected end of input"); return NULL; }
    char c = *ps->p;
    switch (c) {
        case '{': return parse_object(ps);
        case '[': return parse_array(ps);
        case '"': return parse_string(ps);
        case 't':
            if (match_literal(ps, "true"))  return json_new_bool(1);
            set_err(ps, "invalid literal"); return NULL;
        case 'f':
            if (match_literal(ps, "false")) return json_new_bool(0);
            set_err(ps, "invalid literal"); return NULL;
        case 'n':
            if (match_literal(ps, "null"))  return json_new_null();
            set_err(ps, "invalid literal"); return NULL;
        default:
            if (c == '-' || c == '+' || isdigit((unsigned char)c))
                return parse_number(ps);
            set_err(ps, "unexpected character");
            return NULL;
    }
}

JsonValue *json_parse(const char *text, const char **err) {
    if (err) *err = NULL;
    if (!text) { if (err) *err = "null input"; return NULL; }

    Parser ps;
    ps.p = text;
    ps.end = text + strlen(text);
    ps.err = NULL;

    JsonValue *v = parse_value(&ps);
    if (!v) {
        if (err) *err = ps.err ? ps.err : "parse error";
        return NULL;
    }
    skip_ws(&ps);
    if (ps.p != ps.end) {
        /* Trailing garbage: tolerate but not fatal? Treat as error. */
        json_free(v);
        if (err) *err = "trailing characters after JSON value";
        return NULL;
    }
    return v;
}

JsonValue *json_parse_file(const char *path, const char **err) {
    if (err) *err = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) { if (err) *err = "cannot open file"; return NULL; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); if (err) *err = "seek failed"; return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); if (err) *err = "tell failed"; return NULL; }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); if (err) *err = "out of memory"; return NULL; }

    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';

    JsonValue *v = json_parse(buf, err);
    free(buf);
    return v;
}

/* ==========================================================================
 * Serializer
 * ========================================================================== */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    int    ok;
} Sink;

static void sink_ensure(Sink *s, size_t extra) {
    if (!s->ok) return;
    if (s->len + extra + 1 > s->cap) {
        size_t nc = s->cap ? s->cap : 64;
        while (nc < s->len + extra + 1) nc *= 2;
        char *nb = (char *)realloc(s->buf, nc);
        if (!nb) { s->ok = 0; return; }
        s->buf = nb;
        s->cap = nc;
    }
}

static void sink_putc(Sink *s, char c) {
    sink_ensure(s, 1);
    if (!s->ok) return;
    s->buf[s->len++] = c;
}

static void sink_puts(Sink *s, const char *str) {
    size_t n = strlen(str);
    sink_ensure(s, n);
    if (!s->ok) return;
    memcpy(s->buf + s->len, str, n);
    s->len += n;
}

static void sink_indent(Sink *s, int depth) {
    for (int i = 0; i < depth; i++) sink_puts(s, "  ");
}

/* Emit a JSON string literal. Non-ASCII UTF-8 bytes are passed through raw. */
static void sink_string(Sink *s, const char *str) {
    sink_putc(s, '"');
    for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '"':  sink_puts(s, "\\\""); break;
            case '\\': sink_puts(s, "\\\\"); break;
            case '\b': sink_puts(s, "\\b");  break;
            case '\f': sink_puts(s, "\\f");  break;
            case '\n': sink_puts(s, "\\n");  break;
            case '\r': sink_puts(s, "\\r");  break;
            case '\t': sink_puts(s, "\\t");  break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned)c);
                    sink_puts(s, tmp);
                } else {
                    sink_putc(s, (char)c);
                }
        }
    }
    sink_putc(s, '"');
}

static void sink_number(Sink *s, double n) {
    char tmp[64];
    /* Emit integers without a trailing ".0" when the value is integral. */
    if (isfinite(n) && n == floor(n) && fabs(n) < 1e15) {
        snprintf(tmp, sizeof(tmp), "%lld", (long long)n);
    } else {
        snprintf(tmp, sizeof(tmp), "%.17g", n);
    }
    sink_puts(s, tmp);
}

static void serialize_rec(Sink *s, const JsonValue *v, int pretty, int depth) {
    if (!v) { sink_puts(s, "null"); return; }
    switch (v->type) {
        case JSON_NULL:   sink_puts(s, "null"); break;
        case JSON_BOOL:   sink_puts(s, v->as.boolean ? "true" : "false"); break;
        case JSON_NUMBER: sink_number(s, v->as.number); break;
        case JSON_STRING: sink_string(s, v->as.string ? v->as.string : ""); break;
        case JSON_ARRAY: {
            if (v->as.array.count == 0) { sink_puts(s, "[]"); break; }
            sink_putc(s, '[');
            if (pretty) sink_putc(s, '\n');
            for (size_t i = 0; i < v->as.array.count; i++) {
                if (pretty) sink_indent(s, depth + 1);
                serialize_rec(s, v->as.array.items[i], pretty, depth + 1);
                if (i + 1 < v->as.array.count) sink_putc(s, ',');
                if (pretty) sink_putc(s, '\n');
            }
            if (pretty) sink_indent(s, depth);
            sink_putc(s, ']');
            break;
        }
        case JSON_OBJECT: {
            if (v->as.object.count == 0) { sink_puts(s, "{}"); break; }
            sink_putc(s, '{');
            if (pretty) sink_putc(s, '\n');
            size_t i = 0;
            for (JsonMember *m = v->as.object.head; m; m = m->next, i++) {
                if (pretty) sink_indent(s, depth + 1);
                sink_string(s, m->key);
                sink_putc(s, ':');
                if (pretty) sink_putc(s, ' ');
                serialize_rec(s, m->value, pretty, depth + 1);
                if (i + 1 < v->as.object.count) sink_putc(s, ',');
                if (pretty) sink_putc(s, '\n');
            }
            if (pretty) sink_indent(s, depth);
            sink_putc(s, '}');
            break;
        }
    }
}

char *json_serialize(const JsonValue *v, int pretty) {
    Sink s;
    s.buf = NULL;
    s.len = 0;
    s.cap = 0;
    s.ok = 1;
    serialize_rec(&s, v, pretty, 0);
    if (!s.ok) { free(s.buf); return NULL; }
    sink_ensure(&s, 1);
    if (!s.ok) { free(s.buf); return NULL; }
    s.buf[s.len] = '\0';
    return s.buf;
}

int json_serialize_file(const JsonValue *v, const char *path, int pretty) {
    char *str = json_serialize(v, pretty);
    if (!str) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { free(str); return -1; }
    size_t len = strlen(str);
    size_t wrote = fwrite(str, 1, len, f);
    fclose(f);
    free(str);
    return (wrote == len) ? 0 : -1;
}
