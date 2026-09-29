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

/* Parsing msgpack into wire values. Maps keep their entries in the order
 * they were written, which the transit cache depends on. Input is read
 * exactly as far as the end of the value. */

#include "transit_internal.h"

#include <string.h>

#define MAX_DEPTH 10000

typedef struct {
    byte_source *src;
    transit_doc *doc;
    transit_error *err;
    int depth;
} mp_parser;

static int next_byte(mp_parser *m) {
    byte_source *s = m->src;
    int c;
    if (s->p < s->end) c = *s->p++;
    else if (s->getc) c = s->getc(s->ctx);
    else c = -1;
    if (c < 0) {
        set_error(m->err, TRANSIT_ERROR_TRUNCATED, s->offset, "msgpack ended in the middle of a value");
        return -1;
    }
    s->offset++;
    return c;
}

static int read_bytes(mp_parser *m, unsigned char *out, size_t n) {
    byte_source *s = m->src;
    size_t i = 0;
    size_t avail = (size_t)(s->end - s->p);
    if (avail) {
        size_t k = avail < n ? avail : n;
        memcpy(out, s->p, k);
        s->p += k;
        s->offset += k;
        i = k;
    }
    for (; i < n; i++) {
        int c = next_byte(m);
        if (c < 0) return -1;
        out[i] = (unsigned char)c;
    }
    return 0;
}

static int read_uint(mp_parser *m, int bytes, uint64_t *out) {
    unsigned char b[8];
    uint64_t v = 0;
    int i;
    if (read_bytes(m, b, (size_t)bytes)) return -1;
    for (i = 0; i < bytes; i++) v = (v << 8) | b[i];
    *out = v;
    return 0;
}

static transit_value *memory_error(mp_parser *m) {
    set_error(m->err, TRANSIT_ERROR_MEMORY, m->src->offset, "out of memory");
    return NULL;
}

static transit_value *read_str(mp_parser *m, transit_type type, uint64_t n) {
    transit_value *v = doc_value(m->doc, type);
    char *data;
    if (!v || !(data = (char *)doc_alloc(m->doc, (size_t)n + 1))) return memory_error(m);
    if (read_bytes(m, (unsigned char *)data, (size_t)n)) return NULL;
    data[n] = '\0';
    v->u.str.data = data;
    v->u.str.len = (size_t)n;
    return v;
}

static transit_value *parse(mp_parser *m);

static transit_value *read_array(mp_parser *m, uint64_t n) {
    transit_value *a = transit_array(m->doc);
    uint64_t i;
    if (!a) return memory_error(m);
    for (i = 0; i < n; i++) {
        transit_value *item = parse(m);
        if (!item) return NULL;
        if (transit_push(m->doc, a, item)) return memory_error(m);
    }
    return a;
}

static transit_value *read_map(mp_parser *m, uint64_t n) {
    transit_value *map = transit_map(m->doc);
    uint64_t i;
    if (!map) return memory_error(m);
    for (i = 0; i < n; i++) {
        transit_value *k = parse(m), *v;
        if (!k || !(v = parse(m))) return NULL;
        if (transit_map_put(m->doc, map, k, v)) return memory_error(m);
    }
    return map;
}

static transit_value *read_int(mp_parser *m, int bytes, int is_signed) {
    uint64_t u;
    int64_t i;
    if (read_uint(m, bytes, &u)) return NULL;
    if (!is_signed) {
        if (u > (uint64_t)INT64_MAX) {
            char text[24];
            size_t n = 0, k;
            char rev[24];
            do { rev[n++] = (char)('0' + u % 10); u /= 10; } while (u);
            for (k = 0; k < n; k++) text[k] = rev[n - 1 - k];
            return transit_bigint(m->doc, text, n);
        }
        return transit_int(m->doc, (int64_t)u);
    }
    if (bytes == 8) i = (int64_t)u;   /* two's complement */
    else {
        uint64_t sign = (uint64_t)1 << (bytes * 8 - 1);
        i = (u & sign) ? (int64_t)u - (int64_t)(sign << 1) : (int64_t)u;
    }
    return transit_int(m->doc, i);
}

static transit_value *parse(mp_parser *m) {
    int b;
    uint64_t n;
    transit_value *v = NULL;
    if ((b = next_byte(m)) < 0) return NULL;
    if (++m->depth > MAX_DEPTH) {
        set_error(m->err, TRANSIT_ERROR_SYNTAX, m->src->offset, "msgpack nested too deeply");
        return NULL;
    }
    if (b <= 0x7f) v = transit_int(m->doc, b);
    else if (b >= 0xe0) v = transit_int(m->doc, b - 256);
    else if (b <= 0x8f) v = read_map(m, (uint64_t)(b & 0x0f));
    else if (b <= 0x9f) v = read_array(m, (uint64_t)(b & 0x0f));
    else if (b <= 0xbf) v = read_str(m, TRANSIT_STRING, (uint64_t)(b & 0x1f));
    else switch (b) {
    case 0xc0: v = transit_nil(m->doc); break;
    case 0xc2: v = transit_bool(m->doc, 0); break;
    case 0xc3: v = transit_bool(m->doc, 1); break;
    case 0xc4: case 0xc5: case 0xc6:
        if (read_uint(m, 1 << (b - 0xc4), &n) == 0) v = read_str(m, TRANSIT_BYTES, n);
        break;
    case 0xca: {
        uint64_t u;
        if (read_uint(m, 4, &u) == 0) {
            uint32_t w = (uint32_t)u;
            float f;
            memcpy(&f, &w, 4);
            v = transit_float(m->doc, (double)f);
        }
        break;
    }
    case 0xcb: {
        uint64_t u;
        if (read_uint(m, 8, &u) == 0) {
            double d;
            memcpy(&d, &u, 8);
            v = transit_float(m->doc, d);
        }
        break;
    }
    case 0xcc: case 0xcd: case 0xce: case 0xcf: v = read_int(m, 1 << (b - 0xcc), 0); break;
    case 0xd0: case 0xd1: case 0xd2: case 0xd3: v = read_int(m, 1 << (b - 0xd0), 1); break;
    case 0xd9: case 0xda: case 0xdb:
        if (read_uint(m, 1 << (b - 0xd9), &n) == 0) v = read_str(m, TRANSIT_STRING, n);
        break;
    case 0xdc: case 0xdd:
        if (read_uint(m, b == 0xdc ? 2 : 4, &n) == 0) v = read_array(m, n);
        break;
    case 0xde: case 0xdf:
        if (read_uint(m, b == 0xde ? 2 : 4, &n) == 0) v = read_map(m, n);
        break;
    case 0xc1:
        set_error(m->err, TRANSIT_ERROR_SYNTAX, m->src->offset, "invalid msgpack (0xc1)");
        break;
    default:
        set_error(m->err, TRANSIT_ERROR_SYNTAX, m->src->offset,
                  "msgpack extension types aren't used by transit (type byte 0x%02x)", b);
        break;
    }
    if (!v && m->err->code == TRANSIT_OK) memory_error(m);
    m->depth--;
    return v;
}

transit_value *msgpack_parse_wire(transit_doc *doc, byte_source *src, transit_error *err) {
    mp_parser m;
    m.src = src;
    m.doc = doc;
    m.err = err;
    m.depth = 0;
    return parse(&m);
}
