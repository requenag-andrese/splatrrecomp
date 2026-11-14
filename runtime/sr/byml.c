// sr-recomp: BYML tables read into a tree and written back (byml.h).
//
// The file: "YB", a 16-bit version, then three offsets: the key table, the string table, the root container. A string
// table is 0xC2, a 24-bit count, count + 1 offsets from its start, then the strings, sorted, each NUL-terminated. An
// array is its type byte, a 24-bit count, a type byte per element (padded to 4) and a 32-bit value per element; a
// dictionary is its type byte, a 24-bit count and per entry a 24-bit key index, a type byte and a 32-bit value, sorted
// by key. A value is the scalar's bits, a string's index, or the file offset of a container, of a binary (a 32-bit
// length and the bytes) or of the 8 bytes of a 64-bit number. A hash map (0x20: 32-bit keys, 0x21: 64-bit) is its type
// byte, a count, count pairs of a 32-bit value and the key, then a type byte per pair.
#include <stdlib.h>
#include <string.h>

#include "byml.h"

typedef struct Chunk { struct Chunk *next; size_t used, cap; } Chunk;

struct Byml {
    const uint8_t *d;
    size_t n;
    uint32_t keys, strs;       // the file's two string tables
    BymlNode **at;             // container read from offset 4 * i
    BymlNode *root;
    Chunk *chunks;
    int failed;
};

static void *take(Byml *b, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!b->chunks || b->chunks->used + n > b->chunks->cap) {
        size_t cap = n > (1u << 16) ? n : (1u << 16);
        Chunk *c = malloc(sizeof *c + cap);
        if (!c) {
            b->failed = 1;
            return NULL;
        }
        c->next = b->chunks, c->used = 0, c->cap = cap;
        b->chunks = c;
    }
    void *p = (char *)(b->chunks + 1) + b->chunks->used;
    b->chunks->used += n;
    memset(p, 0, n);
    return p;
}

static uint32_t u32(const Byml *b, size_t o) {
    return o + 4 <= b->n ? (uint32_t)b->d[o] | (uint32_t)b->d[o + 1] << 8 | (uint32_t)b->d[o + 2] << 16 | (uint32_t)b->d[o + 3] << 24 : 0;
}

static const char *table_str(Byml *b, uint32_t table, uint32_t i) {
    if (table >= b->n || b->d[table] != 0xC2 || i >= (u32(b, table) >> 8)) return NULL;
    size_t at = (size_t)table + u32(b, (size_t)table + 4 + 4 * (size_t)i);
    return at < b->n && memchr(b->d + at, 0, b->n - at) ? (const char *)b->d + at : NULL;
}

static int plain(uint8_t t) { return t == BYML_BOOL || t == BYML_INT || t == BYML_FLOAT || t == BYML_UINT || t == BYML_NULL; }

static BymlNode *read_container(Byml *b, size_t off, int depth);

static BymlNode *read_value(Byml *b, uint8_t type, uint32_t raw, int depth) {
    if (type == BYML_ARRAY || type == BYML_DICT || type == 0x20 || type == 0x21) return read_container(b, raw, depth + 1);
    BymlNode *v = take(b, sizeof *v);
    if (!v) return NULL;
    v->type = type, v->raw = raw;
    if (type == BYML_STRING) {
        v->str = table_str(b, b->strs, raw);
        if (!v->str) return NULL;
    } else if (type == 0xD4 || type == 0xD5 || type == 0xD6) {
        if ((size_t)raw + 8 > b->n) return NULL;
        v->blob = b->d + raw, v->blob_n = 8;
    } else if (type == BYML_BINARY) {
        if ((size_t)raw + 4 > b->n || (size_t)raw + 4 + u32(b, raw) > b->n) return NULL;
        v->blob = b->d + raw, v->blob_n = 4 + (size_t)u32(b, raw);
    } else if (!plain(type)) {
        return NULL;   // a type this does not know
    }
    return v;
}

static BymlNode *read_container(Byml *b, size_t off, int depth) {
    if (depth > 48 || (off & 3) || off + 4 > b->n) return NULL;
    BymlNode *v = b->at[off / 4];
    if (v) return v->busy ? NULL : v;
    v = take(b, sizeof *v);
    if (!v) return NULL;
    b->at[off / 4] = v;
    v->busy = 1;
    v->type = b->d[off];
    uint32_t n = u32(b, off) >> 8;
    if (v->type == 0x20 || v->type == 0x21) {   // kept whole; only with values that are not offsets or indices
        size_t pair = v->type == 0x20 ? 8 : 12, types = off + 4 + pair * (size_t)n;
        if (types + n > b->n) return NULL;
        for (uint32_t i = 0; i < n; i++)
            if (!plain(b->d[types + i])) return NULL;
        v->blob = b->d + off, v->blob_n = 4 + pair * (size_t)n + (((size_t)n + 3) & ~(size_t)3);
        if (off + v->blob_n > b->n) v->blob_n = b->n - off;   // the last node's padding may be cut
    } else if (v->type == BYML_ARRAY || v->type == BYML_DICT) {
        size_t values = v->type == BYML_ARRAY ? off + 4 + (((size_t)n + 3) & ~(size_t)3) : 0;
        if ((v->type == BYML_ARRAY ? values + 4 * (size_t)n : off + 4 + 8 * (size_t)n) > b->n) return NULL;
        v->n = n;
        v->items = take(b, sizeof *v->items * (n ? n : 1));
        if (v->type == BYML_DICT) v->keys = take(b, sizeof *v->keys * (n ? n : 1));
        if (!v->items || (v->type == BYML_DICT && !v->keys)) return NULL;
        for (uint32_t i = 0; i < n; i++) {
            size_t e = off + 4 + 8 * (size_t)i;
            uint8_t type = v->type == BYML_ARRAY ? b->d[off + 4 + i] : b->d[e + 3];
            uint32_t raw = v->type == BYML_ARRAY ? u32(b, values + 4 * (size_t)i) : u32(b, e + 4);
            if (v->type == BYML_DICT && !(v->keys[i] = table_str(b, b->keys, u32(b, e) & 0xFFFFFF))) return NULL;
            if (!(v->items[i] = read_value(b, type, raw, depth))) return NULL;
        }
    } else {
        return NULL;
    }
    v->busy = 0;
    return v;
}

Byml *byml_parse(const uint8_t *data, size_t n) {
    if (n < 16 || memcmp(data, "YB", 2)) return NULL;
    Byml *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->d = data, b->n = n;
    b->keys = u32(b, 4), b->strs = u32(b, 8);
    b->at = calloc(n / 4 + 1, sizeof *b->at);
    b->root = b->at ? read_container(b, u32(b, 12), 0) : NULL;
    if (!b->root || b->failed || (b->root->type != BYML_ARRAY && b->root->type != BYML_DICT)) {
        byml_free(b);
        return NULL;
    }
    return b;
}

void byml_free(Byml *b) {
    if (!b) return;
    for (Chunk *c = b->chunks, *next; c; c = next) {
        next = c->next;
        free(c);
    }
    free(b->at);
    free(b);
}

BymlNode *byml_root(Byml *b) { return b->root; }

BymlNode *byml_get(const BymlNode *dict, const char *key) {
    if (!dict || dict->type != BYML_DICT) return NULL;
    for (uint32_t i = 0; i < dict->n; i++)
        if (!strcmp(dict->keys[i], key)) return dict->items[i];
    return NULL;
}

BymlNode *byml_scalar(Byml *b, uint8_t type, uint32_t raw) {
    BymlNode *v = take(b, sizeof *v);
    if (v) v->type = type, v->raw = raw;
    return v;
}

BymlNode *byml_string(Byml *b, const char *s) {
    BymlNode *v = take(b, sizeof *v);
    char *copy = take(b, strlen(s) + 1);
    if (!v || !copy) return NULL;
    strcpy(copy, s);
    v->type = BYML_STRING, v->str = copy;
    return v;
}

BymlNode *byml_array(Byml *b, BymlNode *const *items, uint32_t n) {
    BymlNode *v = take(b, sizeof *v);
    BymlNode **copy = take(b, sizeof *copy * (n ? n : 1));
    if (!v || !copy) return NULL;
    for (uint32_t i = 0; i < n; i++) copy[i] = items[i];
    v->type = BYML_ARRAY, v->n = n, v->items = copy;
    return v;
}

BymlNode *byml_clone(Byml *b, const BymlNode *c) {
    if (!c || (c->type != BYML_ARRAY && c->type != BYML_DICT)) return NULL;
    BymlNode *v = byml_array(b, c->items, c->n);
    if (v && c->type == BYML_DICT) {
        v->type = BYML_DICT;
        v->keys = take(b, sizeof *v->keys * (c->n ? c->n : 1));
        if (!v->keys) return NULL;
        for (uint32_t i = 0; i < c->n; i++) v->keys[i] = c->keys[i];
    }
    return v;
}

int byml_set(BymlNode *dict, const char *key, BymlNode *value) {
    if (!dict || dict->type != BYML_DICT || !value) return 0;
    for (uint32_t i = 0; i < dict->n; i++)
        if (!strcmp(dict->keys[i], key)) {
            dict->items[i] = value;
            return 1;
        }
    return 0;
}

// ---- writing
typedef struct { const char **v; uint32_t n, cap; } Names;
typedef struct { uint32_t hash, off, len; } Seen;
typedef struct {
    Byml *b;
    uint8_t *o;
    size_t n, cap;
    Names keys, strs;
    Seen *seen;   // what is written, by the hash of its bytes: an equal container or blob is written once
    uint32_t nseen, seen_cap;
    int failed;
} Out;

static int by_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void name_add(Out *w, Names *t, const char *s) {
    if (t->n == t->cap) {
        const char **more = realloc(t->v, sizeof *more * (t->cap = t->cap ? t->cap * 2 : 256));
        if (!more) {
            w->failed = 1;
            return;
        }
        t->v = more;
    }
    t->v[t->n++] = s;
}

static void names_sort(Names *t) {
    if (!t->n) return;
    qsort(t->v, t->n, sizeof *t->v, by_name);
    uint32_t k = 1;
    for (uint32_t i = 1; i < t->n; i++)
        if (strcmp(t->v[i], t->v[k - 1])) t->v[k++] = t->v[i];
    t->n = k;
}

static uint32_t name_index(const Names *t, const char *s) {
    const char **p = bsearch(&s, t->v, t->n, sizeof *t->v, by_name);
    return p ? (uint32_t)(p - t->v) : 0;
}

// out: 1 while collecting (so a shared container is walked once), cleared again before the nodes are written.
static void collect(Out *w, BymlNode *v) {
    if (v->type == BYML_STRING) name_add(w, &w->strs, v->str);
    if ((v->type != BYML_ARRAY && v->type != BYML_DICT) || v->out) return;
    v->out = 1;
    for (uint32_t i = 0; i < v->n; i++) {
        if (v->type == BYML_DICT) name_add(w, &w->keys, v->keys[i]);
        collect(w, v->items[i]);
    }
}

static void uncollect(BymlNode *v) {
    if ((v->type != BYML_ARRAY && v->type != BYML_DICT) || !v->out) return;
    v->out = 0;
    for (uint32_t i = 0; i < v->n; i++) uncollect(v->items[i]);
}

static uint8_t *room(Out *w, size_t n) {
    if (w->n + n > w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 1 << 16;
        while (cap < w->n + n) cap *= 2;
        uint8_t *more = realloc(w->o, cap);
        if (!more) {
            w->failed = 1;
            return NULL;
        }
        w->o = more, w->cap = cap;
    }
    uint8_t *p = w->o + w->n;
    memset(p, 0, n);
    w->n += n;
    return p;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

static uint32_t put_table(Out *w, const Names *t) {
    uint32_t at = (uint32_t)w->n;
    size_t text = 0;
    for (uint32_t i = 0; i < t->n; i++) text += strlen(t->v[i]) + 1;
    size_t head = 4 + 4 * ((size_t)t->n + 1), size = (head + text + 3) & ~(size_t)3;
    uint8_t *p = room(w, size);
    if (!p) return 0;
    put32(p, 0xC2 | t->n << 8);
    size_t o = head;
    for (uint32_t i = 0; i < t->n; i++) {
        put32(p + 4 + 4 * i, (uint32_t)o);
        strcpy((char *)p + o, t->v[i]);
        o += strlen(t->v[i]) + 1;
    }
    put32(p + 4 + 4 * t->n, (uint32_t)o);
    return at;
}

// bytes[0..len) (a multiple of 4) at the end of the output, or the offset of the same bytes written before.
static uint32_t put_once(Out *w, const uint8_t *bytes, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ bytes[i]) * 16777619u;
    if (w->seen_cap)
        for (uint32_t s = h & (w->seen_cap - 1); w->seen[s].len; s = (s + 1) & (w->seen_cap - 1))
            if (w->seen[s].hash == h && w->seen[s].len == len && !memcmp(w->o + w->seen[s].off, bytes, len)) return w->seen[s].off;
    if (w->nseen * 2 >= w->seen_cap) {   // grow and place again
        uint32_t cap = w->seen_cap ? w->seen_cap * 2 : 4096;
        Seen *more = calloc(cap, sizeof *more);
        if (!more) {
            w->failed = 1;
            return 0;
        }
        for (uint32_t i = 0; i < w->seen_cap; i++)
            if (w->seen[i].len) {
                uint32_t s = w->seen[i].hash & (cap - 1);
                while (more[s].len) s = (s + 1) & (cap - 1);
                more[s] = w->seen[i];
            }
        free(w->seen);
        w->seen = more, w->seen_cap = cap;
    }
    uint32_t at = (uint32_t)w->n;
    uint8_t *p = room(w, len);
    if (!p) return 0;
    memcpy(p, bytes, len);
    uint32_t s = h & (w->seen_cap - 1);
    while (w->seen[s].len) s = (s + 1) & (w->seen_cap - 1);
    w->seen[s] = (Seen){h, at, (uint32_t)len};
    w->nseen++;
    return at;
}

static uint32_t put_blob(Out *w, const BymlNode *v) {
    size_t len = (v->blob_n + 3) & ~(size_t)3;
    uint8_t *tmp = calloc(1, len ? len : 4);
    if (!tmp) {
        w->failed = 1;
        return 0;
    }
    memcpy(tmp, v->blob, v->blob_n);
    uint32_t at = put_once(w, tmp, len);
    free(tmp);
    return at;
}

static uint32_t put_node(Out *w, BymlNode *v);

// The 32-bit value that stands for v in its parent.
static uint32_t value_of(Out *w, BymlNode *v) {
    if (v->type == BYML_STRING) return name_index(&w->strs, v->str);
    if (v->type == BYML_ARRAY || v->type == BYML_DICT) return put_node(w, v);
    if (v->blob) return put_blob(w, v);   // a hash map, a binary, a 64-bit number
    return v->raw;
}

static int by_key(const void *a, const void *b) {
    uint32_t x = ((const uint32_t *)a)[0] & 0xFFFFFF, y = ((const uint32_t *)b)[0] & 0xFFFFFF;
    return x < y ? -1 : x > y;
}

static uint32_t put_node(Out *w, BymlNode *v) {
    if (v->out) return v->out;
    size_t len = v->type == BYML_ARRAY ? 4 + (((size_t)v->n + 3) & ~(size_t)3) + 4 * (size_t)v->n : 4 + 8 * (size_t)v->n;
    uint8_t *tmp = calloc(1, len);
    if (!tmp) {
        w->failed = 1;
        return 0;
    }
    put32(tmp, v->type | v->n << 8);
    for (uint32_t i = 0; i < v->n && !w->failed; i++) {
        uint32_t raw = value_of(w, v->items[i]);   // the children first: their offsets are part of these bytes
        if (v->type == BYML_ARRAY) {
            tmp[4 + i] = v->items[i]->type;
            put32(tmp + 4 + (((size_t)v->n + 3) & ~(size_t)3) + 4 * (size_t)i, raw);
        } else {
            put32(tmp + 4 + 8 * (size_t)i, name_index(&w->keys, v->keys[i]) | (uint32_t)v->items[i]->type << 24);
            put32(tmp + 8 + 8 * (size_t)i, raw);
        }
    }
    if (v->type == BYML_DICT) qsort(tmp + 4, v->n, 8, by_key);   // the game looks a key up by bisection
    v->out = put_once(w, tmp, len);
    free(tmp);
    return v->out;
}

uint8_t *byml_write(Byml *b, size_t *out_n) {
    Out w = {.b = b};
    collect(&w, b->root);
    uncollect(b->root);
    names_sort(&w.keys);
    names_sort(&w.strs);
    uint8_t *head = room(&w, 16);
    uint32_t keys = put_table(&w, &w.keys), strs = put_table(&w, &w.strs);
    uint32_t root = w.failed ? 0 : put_node(&w, b->root);
    uncollect(b->root);   // the offsets are of this write only
    free(w.keys.v);
    free(w.strs.v);
    free(w.seen);
    if (w.failed || !head) {
        free(w.o);
        return NULL;
    }
    memcpy(w.o, b->d, 4);   // the magic and the version
    put32(w.o + 4, keys), put32(w.o + 8, strs), put32(w.o + 12, root);
    *out_n = w.n;
    return w.o;
}
