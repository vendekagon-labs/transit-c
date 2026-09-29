/*
 * Copyright 2026 Vendekagon Labs LLC.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Documents (arenas), value constructors and equality. */

#include "transit_internal.h"

#include <stdlib.h>
#include <string.h>

typedef union {
    long double ld;
    void *p;
    int64_t i;
    double d;
} max_align;

typedef struct chunk {
    struct chunk *next;
    size_t used, size;
    max_align data[1];
} chunk;

struct transit_doc {
    chunk *head;
};

#define CHUNK_SIZE 65536

transit_doc *transit_doc_new(void) {
    return (transit_doc *)calloc(1, sizeof(transit_doc));
}

void transit_doc_free(transit_doc *doc) {
    chunk *c;
    if (!doc) return;
    c = doc->head;
    while (c) {
        chunk *next = c->next;
        free(c);
        c = next;
    }
    free(doc);
}

void *doc_alloc(transit_doc *doc, size_t size) {
    chunk *c = doc->head;
    void *p;
    size = (size + sizeof(max_align) - 1) / sizeof(max_align) * sizeof(max_align);
    if (size == 0) size = sizeof(max_align);
    if (!c || c->size - c->used < size) {
        size_t n = size > CHUNK_SIZE ? size : CHUNK_SIZE;
        c = (chunk *)malloc(offsetof(chunk, data) + n);
        if (!c) return NULL;
        c->used = 0;
        c->size = n;
        if (size > CHUNK_SIZE && doc->head) {
            /* keep filling the current chunk; put the big one behind it */
            c->next = doc->head->next;
            doc->head->next = c;
        } else {
            c->next = doc->head;
            doc->head = c;
        }
    }
    p = (unsigned char *)c->data + c->used;
    c->used += size;
    return p;
}

const char *doc_copy(transit_doc *doc, const char *s, size_t len) {
    char *p = (char *)doc_alloc(doc, len + 1);
    if (!p) return NULL;
    if (len) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

transit_value *doc_value(transit_doc *doc, transit_type type) {
    transit_value *v = (transit_value *)doc_alloc(doc, sizeof(transit_value));
    if (!v) return NULL;
    memset(v, 0, sizeof(*v));
    v->type = type;
    return v;
}

transit_value *doc_str_value(transit_doc *doc, transit_type type, const char *s, size_t len) {
    transit_value *v = doc_value(doc, type);
    if (!v) return NULL;
    v->u.str.data = doc_copy(doc, s, len);
    if (!v->u.str.data) return NULL;
    v->u.str.len = len;
    return v;
}

transit_value *transit_nil(transit_doc *doc) { return doc_value(doc, TRANSIT_NIL); }

transit_value *transit_bool(transit_doc *doc, int b) {
    transit_value *v = doc_value(doc, TRANSIT_BOOL);
    if (v) v->u.boolean = b ? 1 : 0;
    return v;
}

transit_value *transit_int(transit_doc *doc, int64_t i) {
    transit_value *v = doc_value(doc, TRANSIT_INT);
    if (v) v->u.integer = i;
    return v;
}

transit_value *transit_float(transit_doc *doc, double d) {
    transit_value *v = doc_value(doc, TRANSIT_FLOAT);
    if (v) v->u.number = d;
    return v;
}

transit_value *transit_string(transit_doc *doc, const char *s, size_t len) { return doc_str_value(doc, TRANSIT_STRING, s, len); }
transit_value *transit_keyword(transit_doc *doc, const char *s, size_t len) { return doc_str_value(doc, TRANSIT_KEYWORD, s, len); }
transit_value *transit_symbol(transit_doc *doc, const char *s, size_t len) { return doc_str_value(doc, TRANSIT_SYMBOL, s, len); }
transit_value *transit_uri(transit_doc *doc, const char *s, size_t len) { return doc_str_value(doc, TRANSIT_URI, s, len); }
transit_value *transit_bigint(transit_doc *doc, const char *s, size_t len) { return doc_str_value(doc, TRANSIT_BIGINT, s, len); }
transit_value *transit_decimal(transit_doc *doc, const char *s, size_t len) { return doc_str_value(doc, TRANSIT_DECIMAL, s, len); }
transit_value *transit_bytes(transit_doc *doc, const void *data, size_t len) { return doc_str_value(doc, TRANSIT_BYTES, (const char *)data, len); }

transit_value *transit_time(transit_doc *doc, int64_t ms) {
    transit_value *v = doc_value(doc, TRANSIT_TIME);
    if (v) v->u.integer = ms;
    return v;
}

transit_value *transit_uuid(transit_doc *doc, const unsigned char bytes[16]) {
    transit_value *v = doc_value(doc, TRANSIT_UUID);
    if (v) memcpy(v->u.uuid, bytes, 16);
    return v;
}

transit_value *transit_array(transit_doc *doc) { return doc_value(doc, TRANSIT_ARRAY); }
transit_value *transit_list(transit_doc *doc) { return doc_value(doc, TRANSIT_LIST); }
transit_value *transit_set(transit_doc *doc) { return doc_value(doc, TRANSIT_SET); }
transit_value *transit_map(transit_doc *doc) { return doc_value(doc, TRANSIT_MAP); }

transit_value *transit_tagged(transit_doc *doc, const char *tag, size_t tag_len, transit_value *rep) {
    transit_value *v = doc_value(doc, TRANSIT_TAGGED);
    if (!v) return NULL;
    v->u.tagged.tag = doc_copy(doc, tag, tag_len);
    if (!v->u.tagged.tag) return NULL;
    v->u.tagged.tag_len = tag_len;
    v->u.tagged.rep = rep;
    return v;
}

/* Grows an array of value pointers held in the document. */
static int grow(transit_doc *doc, transit_value ***items, size_t count, size_t *cap) {
    size_t n = *cap ? *cap * 2 : 8;
    transit_value **p = (transit_value **)doc_alloc(doc, n * sizeof(transit_value *));
    if (!p) return -1;
    if (count) memcpy(p, *items, count * sizeof(transit_value *));
    *items = p;
    *cap = n;
    return 0;
}

int transit_push(transit_doc *doc, transit_value *coll, transit_value *item) {
    if (coll->u.coll.count == coll->u.coll.cap &&
        grow(doc, &coll->u.coll.items, coll->u.coll.count, &coll->u.coll.cap) != 0) return -1;
    coll->u.coll.items[coll->u.coll.count++] = item;
    return 0;
}

int transit_map_put(transit_doc *doc, transit_value *map, transit_value *key, transit_value *value) {
    if (map->u.map.count == map->u.map.cap) {
        size_t cap = map->u.map.cap;
        if (grow(doc, &map->u.map.keys, map->u.map.count, &cap) != 0) return -1;
        cap = map->u.map.cap;
        if (grow(doc, &map->u.map.values, map->u.map.count, &cap) != 0) return -1;
        map->u.map.cap = cap;
    }
    map->u.map.keys[map->u.map.count] = key;
    map->u.map.values[map->u.map.count] = value;
    map->u.map.count++;
    return 0;
}

transit_value *transit_map_get(const transit_value *map, const transit_value *key) {
    size_t i;
    for (i = 0; i < map->u.map.count; i++)
        if (transit_equal(map->u.map.keys[i], key)) return map->u.map.values[i];
    return NULL;
}

static int same_str(const transit_value *a, const transit_value *b) {
    return a->u.str.len == b->u.str.len && (a->u.str.len == 0 || memcmp(a->u.str.data, b->u.str.data, a->u.str.len) == 0);
}

int transit_equal(const transit_value *a, const transit_value *b) {
    size_t i, j;
    if (a == b) return 1;
    if (!a || !b || a->type != b->type) return 0;
    switch (a->type) {
    case TRANSIT_NIL:
        return 1;
    case TRANSIT_BOOL:
        return a->u.boolean == b->u.boolean;
    case TRANSIT_INT:
    case TRANSIT_TIME:
        return a->u.integer == b->u.integer;
    case TRANSIT_FLOAT:
        return a->u.number == b->u.number || (a->u.number != a->u.number && b->u.number != b->u.number);
    case TRANSIT_BIGINT:
    case TRANSIT_DECIMAL:
    case TRANSIT_STRING:
    case TRANSIT_KEYWORD:
    case TRANSIT_SYMBOL:
    case TRANSIT_BYTES:
    case TRANSIT_URI:
        return same_str(a, b);
    case TRANSIT_UUID:
        return memcmp(a->u.uuid, b->u.uuid, 16) == 0;
    case TRANSIT_ARRAY:
    case TRANSIT_LIST:
        if (a->u.coll.count != b->u.coll.count) return 0;
        for (i = 0; i < a->u.coll.count; i++)
            if (!transit_equal(a->u.coll.items[i], b->u.coll.items[i])) return 0;
        return 1;
    case TRANSIT_SET:
        if (a->u.coll.count != b->u.coll.count) return 0;
        for (i = 0; i < a->u.coll.count; i++) {
            for (j = 0; j < b->u.coll.count; j++)
                if (transit_equal(a->u.coll.items[i], b->u.coll.items[j])) break;
            if (j == b->u.coll.count) return 0;
        }
        return 1;
    case TRANSIT_MAP:
        if (a->u.map.count != b->u.map.count) return 0;
        for (i = 0; i < a->u.map.count; i++) {
            /* fast path: entries in the same order */
            if (transit_equal(a->u.map.keys[i], b->u.map.keys[i])) {
                if (!transit_equal(a->u.map.values[i], b->u.map.values[i])) return 0;
                continue;
            }
            for (j = 0; j < b->u.map.count; j++)
                if (transit_equal(a->u.map.keys[i], b->u.map.keys[j])) break;
            if (j == b->u.map.count || !transit_equal(a->u.map.values[i], b->u.map.values[j])) return 0;
        }
        return 1;
    case TRANSIT_TAGGED:
        return a->u.tagged.tag_len == b->u.tagged.tag_len &&
               memcmp(a->u.tagged.tag, b->u.tagged.tag, a->u.tagged.tag_len) == 0 &&
               transit_equal(a->u.tagged.rep, b->u.tagged.rep);
    }
    return 0;
}
