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

/* Decoding wire values into transit values, and reading from memory and
 * streams. */

#include "transit_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    transit_doc *doc;
    transit_error *err;
    transit_cache cache;
} decoder;

/* "~#tag" strings decode to a tag marker: a tagged value with this rep. */
static transit_value TAG_MARKER;

static int is_marker(const transit_value *v) { return v && v->type == TRANSIT_TAGGED && v->u.tagged.rep == &TAG_MARKER; }

static transit_value *decode(decoder *d, transit_value *w, int as_key);

static transit_value *invalid(decoder *d, const char *what, const char *s, size_t len) {
    set_error(d->err, TRANSIT_ERROR_TRANSIT, 0, "%s: %.*s", what, (int)(len > 60 ? 60 : len), s);
    return NULL;
}

static transit_value *memory(decoder *d) {
    set_error(d->err, TRANSIT_ERROR_MEMORY, 0, "out of memory");
    return NULL;
}

/* A string value sharing data held in the document. */
static transit_value *str_ref_value(decoder *d, transit_type type, const char *s, size_t len) {
    transit_value *v = doc_value(d->doc, type);
    if (!v) return memory(d);
    v->u.str.data = s;
    v->u.str.len = len;
    return v;
}

static int is_tag(const char *tag, size_t tag_len, const char *name) {
    return strlen(name) == tag_len && memcmp(tag, name, tag_len) == 0;
}

static transit_value *int_from_text(decoder *d, const char *s, size_t len) {
    int64_t i;
    int r = parse_int64(s, len, &i);
    if (r == 0) return transit_int(d->doc, i);
    if (r == 1) return transit_bigint(d->doc, s, len);
    return invalid(d, "not an integer", s, len);
}

/* Decodes the representation of a one character tag given as a string, as
 * in "~xrep". */
static transit_value *decode_scalar(decoder *d, const char *tag, size_t tag_len, const char *s, size_t len) {
    transit_value *v;
    if (tag_len == 1) switch (tag[0]) {
    case '_': return transit_nil(d->doc);
    case '?': return transit_bool(d->doc, len == 1 && s[0] == 't');
    case 'i': return int_from_text(d, s, len);
    case 'n': return is_integer_text(s, len) ? str_ref_value(d, TRANSIT_BIGINT, s, len) : invalid(d, "not an integer", s, len);
    case 'd': {
        double x;
        if (parse_double(s, len, &x)) return invalid(d, "not a number", s, len);
        return transit_float(d->doc, x);
    }
    case 'f': return str_ref_value(d, TRANSIT_DECIMAL, s, len);
    case ':': return str_ref_value(d, TRANSIT_KEYWORD, s, len);
    case '$': return str_ref_value(d, TRANSIT_SYMBOL, s, len);
    case 'r': return str_ref_value(d, TRANSIT_URI, s, len);
    case '\'': return str_ref_value(d, TRANSIT_STRING, s, len);
    case 'z':
        if (len == 3 && memcmp(s, "NaN", 3) == 0) return transit_float(d->doc, NAN);
        if (len == 3 && memcmp(s, "INF", 3) == 0) return transit_float(d->doc, INFINITY);
        if (len == 4 && memcmp(s, "-INF", 4) == 0) return transit_float(d->doc, -INFINITY);
        return invalid(d, "not a special number", s, len);
    case 'u': {
        unsigned char u[16];
        if (parse_uuid(s, len, u)) return invalid(d, "not a uuid", s, len);
        return transit_uuid(d->doc, u);
    }
    case 't': {
        int64_t ms;
        if (parse_rfc3339(s, len, &ms)) return invalid(d, "not a date/time", s, len);
        return transit_time(d->doc, ms);
    }
    case 'm': {
        int64_t ms;
        if (parse_int64(s, len, &ms)) return invalid(d, "not a time", s, len);
        return transit_time(d->doc, ms);
    }
    case 'b': {
        const unsigned char *bytes;
        size_t n;
        if (base64_decode(d->doc, s, len, &bytes, &n)) return invalid(d, "not base64", s, len);
        return str_ref_value(d, TRANSIT_BYTES, (const char *)bytes, n);
    }
    }
    v = str_ref_value(d, TRANSIT_STRING, s, len);
    return v ? transit_tagged(d->doc, tag, tag_len, v) : NULL;
}

static void int64_bytes(int64_t i, unsigned char *out) {
    uint64_t u = (uint64_t)i;
    int k;
    for (k = 7; k >= 0; k--) {
        out[k] = (unsigned char)(u & 0xff);
        u >>= 8;
    }
}

/* Decodes a tagged value, given its decoded representation. */
static transit_value *decode_tag(decoder *d, const char *tag, size_t tag_len, transit_value *rep) {
    if (rep->type == TRANSIT_STRING && tag_len == 1)
        return decode_scalar(d, tag, tag_len, rep->u.str.data, rep->u.str.len);
    if (is_tag(tag, tag_len, "'")) return rep;
    if (rep->type == TRANSIT_ARRAY && (is_tag(tag, tag_len, "set") || is_tag(tag, tag_len, "list"))) {
        rep->type = tag_len == 3 ? TRANSIT_SET : TRANSIT_LIST;
        return rep;
    }
    if (rep->type == TRANSIT_ARRAY && is_tag(tag, tag_len, "cmap")) {
        transit_value *m = transit_map(d->doc);
        size_t i;
        if (!m) return memory(d);
        if (rep->u.coll.count % 2) return invalid(d, "cmap with an odd number of elements", tag, tag_len);
        for (i = 0; i < rep->u.coll.count; i += 2)
            if (transit_map_put(d->doc, m, rep->u.coll.items[i], rep->u.coll.items[i + 1])) return memory(d);
        return m;
    }
    if (tag_len == 1) {
        /* the non-string representations transit-java uses in msgpack */
        if ((tag[0] == 'm' || tag[0] == 'i') && rep->type == TRANSIT_INT) {
            if (tag[0] == 'i') return rep;
            rep->type = TRANSIT_TIME;
            return rep;
        }
        if (tag[0] == 'd' && rep->type == TRANSIT_FLOAT) return rep;
        if (tag[0] == '?' && rep->type == TRANSIT_BOOL) return rep;
        if (tag[0] == 'u' && rep->type == TRANSIT_ARRAY && rep->u.coll.count == 2 &&
            rep->u.coll.items[0]->type == TRANSIT_INT && rep->u.coll.items[1]->type == TRANSIT_INT) {
            unsigned char u[16];
            int64_bytes(rep->u.coll.items[0]->u.integer, u);
            int64_bytes(rep->u.coll.items[1]->u.integer, u + 8);
            return transit_uuid(d->doc, u);
        }
    }
    return transit_tagged(d->doc, tag, tag_len, rep);
}

static size_t utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 1;
}

static transit_value *decode_string(decoder *d, transit_value *w, int as_key) {
    const char *s = w->u.str.data;
    size_t len = w->u.str.len, n;
    if (is_cache_key(s, len)) {
        str_ref r;
        if (!cache_lookup(&d->cache, s, len, &r)) return invalid(d, "unknown cache code", s, len);
        s = r.data;
        len = r.len;
    } else if (is_cacheable(s, len, as_key)) {
        cache_add(&d->cache, s, len);
    }
    if (len < 2 || s[0] != '~') return s == w->u.str.data ? w : str_ref_value(d, TRANSIT_STRING, s, len);
    if (s[1] == '#') {
        transit_value *m = doc_value(d->doc, TRANSIT_TAGGED);
        if (!m) return memory(d);
        m->u.tagged.tag = s + 2;
        m->u.tagged.tag_len = len - 2;
        m->u.tagged.rep = &TAG_MARKER;
        return m;
    }
    if (s[1] == '~' || s[1] == '^' || s[1] == '`') return str_ref_value(d, TRANSIT_STRING, s + 1, len - 1);
    n = utf8_char_len((unsigned char)s[1]);
    if (1 + n > len) n = len - 1;
    return decode_scalar(d, s + 1, n, s + 1 + n, len - 1 - n);
}

/* A decoded value that must not be a tag marker. */
static transit_value *decode_value(decoder *d, transit_value *w, int as_key) {
    transit_value *v = decode(d, w, as_key);
    if (is_marker(v)) return invalid(d, "a tag outside a tagged value", v->u.tagged.tag, v->u.tagged.tag_len);
    return v;
}

static transit_value *decode_array(decoder *d, transit_value *w, int as_key) {
    size_t n = w->u.coll.count, i;
    transit_value *first, *a;
    if (n == 0) return transit_array(d->doc);
    first = w->u.coll.items[0];
    if (first->type == TRANSIT_STRING && first->u.str.len == 2 && memcmp(first->u.str.data, "^ ", 2) == 0) {
        transit_value *m = transit_map(d->doc);
        if (!m) return memory(d);
        if (n % 2 == 0) return invalid(d, "map with an odd number of elements", "", 0);
        for (i = 1; i < n; i += 2) {
            transit_value *k = decode_value(d, w->u.coll.items[i], 1), *v;
            if (!k || !(v = decode_value(d, w->u.coll.items[i + 1], 0))) return NULL;
            if (transit_map_put(d->doc, m, k, v)) return memory(d);
        }
        return m;
    }
    /* Each element must be decoded exactly once, in order, to keep the
     * cache in step with the writer. */
    first = decode(d, first, as_key);
    if (!first) return NULL;
    if (is_marker(first)) {
        transit_value *rep;
        if (n != 2) return invalid(d, "tagged value without exactly one representation", first->u.tagged.tag, first->u.tagged.tag_len);
        if (!(rep = decode_value(d, w->u.coll.items[1], 0))) return NULL;
        return decode_tag(d, first->u.tagged.tag, first->u.tagged.tag_len, rep);
    }
    if (!(a = transit_array(d->doc)) || transit_push(d->doc, a, first)) return memory(d);
    for (i = 1; i < n; i++) {
        transit_value *v = decode_value(d, w->u.coll.items[i], as_key);
        if (!v) return NULL;
        if (transit_push(d->doc, a, v)) return memory(d);
    }
    return a;
}

static transit_value *decode_map(decoder *d, transit_value *w) {
    size_t n = w->u.map.count, i;
    transit_value *m;
    if (n == 1) {
        transit_value *k = decode(d, w->u.map.keys[0], 1), *v;
        if (!k) return NULL;
        if (is_marker(k)) {
            if (!(v = decode_value(d, w->u.map.values[0], 0))) return NULL;
            return decode_tag(d, k->u.tagged.tag, k->u.tagged.tag_len, v);
        }
        if (!(v = decode_value(d, w->u.map.values[0], 0))) return NULL;
        if (!(m = transit_map(d->doc)) || transit_map_put(d->doc, m, k, v)) return memory(d);
        return m;
    }
    if (!(m = transit_map(d->doc))) return memory(d);
    for (i = 0; i < n; i++) {
        transit_value *k = decode_value(d, w->u.map.keys[i], 1), *v;
        if (!k || !(v = decode_value(d, w->u.map.values[i], 0))) return NULL;
        if (transit_map_put(d->doc, m, k, v)) return memory(d);
    }
    return m;
}

static transit_value *decode(decoder *d, transit_value *w, int as_key) {
    switch (w->type) {
    case TRANSIT_STRING: return decode_string(d, w, as_key);
    case TRANSIT_ARRAY: return decode_array(d, w, as_key);
    case TRANSIT_MAP: return decode_map(d, w);
    default: return w;   /* nil, bool, int, bigint, float, bytes */
    }
}

transit_value *decode_wire(transit_doc *doc, transit_value *wire, transit_error *err) {
    decoder d;
    transit_value *v;
    d.doc = doc;
    d.err = err;
    cache_init(&d.cache, 0);
    v = decode_value(&d, wire, 0);
    cache_free(&d.cache);
    return v;
}

/* Reading from memory */

transit_value *transit_read(transit_doc *doc, const void *data, size_t len, transit_format format, transit_error *err) {
    transit_error local;
    transit_value *wire;
    if (!err) err = &local;
    clear_error(err);
    if (format == TRANSIT_MSGPACK) {
        byte_source src;
        src.p = (const unsigned char *)data;
        src.end = src.p + len;
        src.getc = NULL;
        src.ctx = NULL;
        src.offset = 0;
        wire = msgpack_parse_wire(doc, &src, err);
        if (wire && src.p != src.end) {
            set_error(err, TRANSIT_ERROR_SYNTAX, src.offset, "unexpected data after the value");
            return NULL;
        }
    } else {
        wire = json_parse_wire(doc, (const char *)data, len, err);
    }
    return wire ? decode_wire(doc, wire, err) : NULL;
}

/* Reading from streams */

struct transit_stream {
    transit_format format;
    transit_getc_fn getc;
    void *ctx;
    size_t offset;
    transit_buffer text;
};

transit_stream *transit_stream_new(transit_format format, transit_getc_fn getc, void *ctx) {
    transit_stream *s = (transit_stream *)calloc(1, sizeof(transit_stream));
    if (!s) return NULL;
    s->format = format;
    s->getc = getc;
    s->ctx = ctx;
    return s;
}

void transit_stream_free(transit_stream *s) {
    if (!s) return;
    transit_buffer_free(&s->text);
    free(s);
}

/* Reads the text of the next JSON array, object or string into s->text.
 * Returns 1, 0 at the end of the input, or -1 on error. */
static int next_json_text(transit_stream *s, transit_error *err) {
    int depth = 0, in_string = 0, escaped = 0, started = 0, c;
    s->text.len = 0;
    for (;;) {
        c = s->getc(s->ctx);
        if (c < 0) {
            if (!started) return 0;
            set_error(err, TRANSIT_ERROR_TRUNCATED, s->offset, "JSON ended in the middle of a value");
            return -1;
        }
        s->offset++;
        if (!started) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
            started = 1;
            if (c == '[' || c == '{') depth = 1;
            else if (c == '"') in_string = 1;
            else {
                set_error(err, TRANSIT_ERROR_SYNTAX, s->offset, "expected a JSON array, object or string");
                return -1;
            }
        } else if (in_string) {
            if (escaped) escaped = 0;
            else if (c == '\\') escaped = 1;
            else if (c == '"') in_string = 0;
        } else if (c == '"') {
            in_string = 1;
        } else if (c == '[' || c == '{') {
            depth++;
        } else if (c == ']' || c == '}') {
            depth--;
        }
        if (buf_byte(&s->text, (unsigned char)c)) {
            set_error(err, TRANSIT_ERROR_MEMORY, s->offset, "out of memory");
            return -1;
        }
        if (depth == 0 && !in_string) return 1;
    }
}

transit_value *transit_stream_read(transit_stream *s, transit_doc *doc, transit_error *err) {
    transit_error local;
    transit_value *wire;
    if (!err) err = &local;
    clear_error(err);
    if (s->format == TRANSIT_MSGPACK) {
        unsigned char first;
        byte_source src;
        int c = s->getc(s->ctx);
        if (c < 0) return NULL;
        first = (unsigned char)c;
        src.p = &first;
        src.end = &first + 1;
        src.getc = s->getc;
        src.ctx = s->ctx;
        src.offset = s->offset;
        wire = msgpack_parse_wire(doc, &src, err);
        s->offset = src.offset;
    } else {
        int r = next_json_text(s, err);
        if (r <= 0) return NULL;
        wire = json_parse_wire(doc, (const char *)s->text.data, s->text.len, err);
    }
    return wire ? decode_wire(doc, wire, err) : NULL;
}
