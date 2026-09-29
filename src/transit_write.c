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

/* Writing transit values as JSON, JSON-verbose or msgpack.
 *
 * Values with one character tags are written as "~x..." strings where the
 * encoding prefers strings (JSON, and map keys), and as tagged values
 * otherwise; msgpack output matches transit-java's byte for byte. Values
 * are written strictly in document order, which keeps the cache in step
 * with readers. */

#include "transit_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAX_DEPTH 10000
#define JSON_MAX_INT 9007199254740991LL   /* 2^53 - 1 */

typedef struct {
    int verbose, msgpack;
    transit_buffer *out;
    transit_cache cache;
    transit_doc *scratch;   /* strings the cache refers to */
    transit_error *err;
    int depth;
} writer;

static int fail(writer *w, transit_error_code code, const char *message) {
    set_error(w->err, code, 0, "%s", message);
    return -1;
}

static int put(writer *w, const void *data, size_t len) {
    return buf_put(w->out, data, len) ? fail(w, TRANSIT_ERROR_MEMORY, "out of memory") : 0;
}

static int put_byte(writer *w, unsigned char c) {
    return buf_byte(w->out, c) ? fail(w, TRANSIT_ERROR_MEMORY, "out of memory") : 0;
}

static int put_cstr(writer *w, const char *s) { return put(w, s, strlen(s)); }

/* msgpack */

static int put_be(writer *w, uint64_t v, int bytes) {
    unsigned char b[8];
    int i;
    for (i = bytes - 1; i >= 0; i--) {
        b[i] = (unsigned char)(v & 0xff);
        v >>= 8;
    }
    return put(w, b, (size_t)bytes);
}

/* The smallest encoding, as transit-java (msgpack-java) writes it. */
static int pack_int(writer *w, int64_t i) {
    if (i >= 0) {
        if (i <= 127) return put_byte(w, (unsigned char)i);
        if (i <= 0xff) return put_byte(w, 0xcc) || put_be(w, (uint64_t)i, 1);
        if (i <= 0xffff) return put_byte(w, 0xcd) || put_be(w, (uint64_t)i, 2);
        if (i <= 0xffffffffLL) return put_byte(w, 0xce) || put_be(w, (uint64_t)i, 4);
        return put_byte(w, 0xcf) || put_be(w, (uint64_t)i, 8);
    }
    if (i >= -32) return put_byte(w, (unsigned char)(i + 256));
    if (i >= -128) return put_byte(w, 0xd0) || put_be(w, (uint64_t)(i + 256), 1);
    if (i >= -32768) return put_byte(w, 0xd1) || put_be(w, (uint64_t)(i + 65536), 2);
    if (i >= -2147483647LL - 1) return put_byte(w, 0xd2) || put_be(w, (uint64_t)(i + 4294967296LL), 4);
    return put_byte(w, 0xd3) || put_be(w, (uint64_t)i, 8);
}

static int pack_header(writer *w, size_t n, unsigned char fix, unsigned char c16, unsigned char c32) {
    if (n <= 15) return put_byte(w, (unsigned char)(fix | n));
    if (n <= 0xffff) return put_byte(w, c16) || put_be(w, n, 2);
    return put_byte(w, c32) || put_be(w, n, 4);
}

static int pack_str(writer *w, const char *s, size_t n) {
    int r;
    if (n <= 31) r = put_byte(w, (unsigned char)(0xa0 | n));
    else if (n <= 0xff) r = put_byte(w, 0xd9) || put_be(w, n, 1);
    else if (n <= 0xffff) r = put_byte(w, 0xda) || put_be(w, n, 2);
    else r = put_byte(w, 0xdb) || put_be(w, n, 4);
    return r || put(w, s, n);
}

static int pack_double(writer *w, double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return put_byte(w, 0xcb) || put_be(w, u, 8);
}

/* JSON */

static int json_quote(writer *w, const char *s, size_t n) {
    size_t i, run = 0;
    if (put_byte(w, '"')) return -1;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[8];
        if (c == '"') esc = "\\\"";
        else if (c == '\\') esc = "\\\\";
        else if (c < 0x20) {
            switch (c) {
            case '\b': esc = "\\b"; break;
            case '\t': esc = "\\t"; break;
            case '\n': esc = "\\n"; break;
            case '\f': esc = "\\f"; break;
            case '\r': esc = "\\r"; break;
            default:
                snprintf(u, sizeof(u), "\\u%04x", c);
                esc = u;
            }
        }
        if (!esc) continue;
        if (put(w, s + run, i - run) || put_cstr(w, esc)) return -1;
        run = i + 1;
    }
    return put(w, s + run, n - run) || put_byte(w, '"');
}

/* Strings, going through the cache (except in JSON-verbose). s must stay
 * valid until the write is done. */

static int emit_str(writer *w, const char *s, size_t n, int as_key) {
    char code[4];
    if (!w->verbose) {
        size_t k = cache_write(&w->cache, s, n, as_key, code);
        if (k) {
            s = code;
            n = k;
        }
    }
    return w->msgpack ? pack_str(w, s, n) : json_quote(w, s, n);
}

/* Emits prefix followed by s, as a string. */
static int emit_prefixed(writer *w, const char *prefix, const char *s, size_t n, int as_key) {
    size_t p = strlen(prefix);
    char *joined = (char *)doc_alloc(w->scratch, p + n + 1);
    if (!joined) return fail(w, TRANSIT_ERROR_MEMORY, "out of memory");
    memcpy(joined, prefix, p);
    if (n) memcpy(joined + p, s, n);
    joined[p + n] = '\0';
    return emit_str(w, joined, p + n, as_key);
}

static int emit_prefixed_cstr(writer *w, const char *prefix, const char *s, int as_key) {
    return emit_prefixed(w, prefix, s, strlen(s), as_key);
}

static int marshal(writer *w, const transit_value *v, int as_key);

static int tagged_start(writer *w, const char *tag, size_t tag_len) {
    int r;
    if (w->msgpack) r = put_byte(w, 0x92);
    else r = put_byte(w, w->verbose ? '{' : '[');
    /* the tag goes through the cache before the representation */
    if (r || emit_prefixed(w, "~#", tag, tag_len, 0)) return -1;
    return w->msgpack ? 0 : put_byte(w, w->verbose ? ':' : ',');
}

static int tagged_end(writer *w) {
    return w->msgpack ? 0 : put_byte(w, w->verbose ? '}' : ']');
}

static int array_start(writer *w, size_t n) {
    return w->msgpack ? pack_header(w, n, 0x90, 0xdc, 0xdd) : put_byte(w, '[');
}

static int array_sep(writer *w, size_t i) { return (w->msgpack || i == 0) ? 0 : put_byte(w, ','); }

static int array_end(writer *w) { return w->msgpack ? 0 : put_byte(w, ']'); }

static int emit_items(writer *w, transit_value *const *items, size_t n) {
    size_t i;
    if (array_start(w, n)) return -1;
    for (i = 0; i < n; i++)
        if (array_sep(w, i) || marshal(w, items[i], 0)) return -1;
    return array_end(w);
}

static size_t utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xe0) == 0xc0) return 2;
    if ((c & 0xf0) == 0xe0) return 3;
    if ((c & 0xf8) == 0xf0) return 4;
    return 1;
}

/* A tagged value written as a "~x..." string: a one character tag and a
 * string representation. */
static int is_scalar_tagged(const transit_value *v) {
    return v->type == TRANSIT_TAGGED && v->u.tagged.tag_len > 0 &&
           v->u.tagged.tag_len == utf8_char_len((unsigned char)v->u.tagged.tag[0]) &&
           v->u.tagged.rep->type == TRANSIT_STRING;
}

/* Whether v is written as a string (as a map key), so can be a key in a
 * plain map rather than a cmap. */
static int is_stringable(const transit_value *v) {
    switch (v->type) {
    case TRANSIT_ARRAY: case TRANSIT_LIST: case TRANSIT_SET: case TRANSIT_MAP: return 0;
    case TRANSIT_TAGGED: return is_scalar_tagged(v);
    default: return 1;
    }
}

static int marshal_map(writer *w, const transit_value *m) {
    size_t n = m->u.map.count, i;
    for (i = 0; i < n; i++) if (!is_stringable(m->u.map.keys[i])) break;
    if (i < n) {
        /* a cmap: ["~#cmap", [k1, v1, k2, v2, ...]] */
        if (tagged_start(w, "cmap", 4) || array_start(w, 2 * n)) return -1;
        for (i = 0; i < n; i++)
            if (array_sep(w, 2 * i) || marshal(w, m->u.map.keys[i], 0) ||
                array_sep(w, 1) || marshal(w, m->u.map.values[i], 0)) return -1;
        return array_end(w) || tagged_end(w);
    }
    if (w->msgpack) {
        if (pack_header(w, n, 0x80, 0xde, 0xdf)) return -1;
        for (i = 0; i < n; i++)
            if (marshal(w, m->u.map.keys[i], 1) || marshal(w, m->u.map.values[i], 0)) return -1;
        return 0;
    }
    if (w->verbose) {
        if (put_byte(w, '{')) return -1;
        for (i = 0; i < n; i++)
            if ((i && put_byte(w, ',')) || marshal(w, m->u.map.keys[i], 1) || put_byte(w, ':') ||
                marshal(w, m->u.map.values[i], 0)) return -1;
        return put_byte(w, '}');
    }
    if (put_cstr(w, "[\"^ \"")) return -1;
    for (i = 0; i < n; i++)
        if (put_byte(w, ',') || marshal(w, m->u.map.keys[i], 1) || put_byte(w, ',') ||
            marshal(w, m->u.map.values[i], 0)) return -1;
    return put_byte(w, ']');
}

static int marshal_int(writer *w, int64_t i, int as_key) {
    char digits[24];
    snprintf(digits, sizeof(digits), "%lld", (long long)i);
    if (as_key && !w->msgpack) return emit_prefixed_cstr(w, "~i", digits, 1);
    if (w->msgpack) return pack_int(w, i);
    if (i >= -JSON_MAX_INT && i <= JSON_MAX_INT) return put_cstr(w, digits);
    return emit_prefixed_cstr(w, "~i", digits, 0);
}

static int marshal_float(writer *w, double d, int as_key) {
    char text[DOUBLE_TEXT_MAX];
    if (d != d) return emit_prefixed_cstr(w, "~z", "NaN", as_key);
    if (d == INFINITY) return emit_prefixed_cstr(w, "~z", "INF", as_key);
    if (d == -INFINITY) return emit_prefixed_cstr(w, "~z", "-INF", as_key);
    if (w->msgpack) return pack_double(w, d);
    format_double(d, text);
    if (as_key) return emit_prefixed_cstr(w, "~d", text, 1);
    return put_cstr(w, text);
}

static int marshal_bytes(writer *w, const transit_value *v, int as_key) {
    transit_buffer b = {0};
    int r;
    if (base64_encode(&b, (const unsigned char *)v->u.str.data, v->u.str.len))
        r = fail(w, TRANSIT_ERROR_MEMORY, "out of memory");
    else
        r = emit_prefixed(w, "~b", (const char *)b.data, b.len, as_key);
    transit_buffer_free(&b);
    return r;
}

static int marshal_time(writer *w, int64_t ms, int as_key) {
    if (w->verbose) {
        char text[48];
        format_rfc3339(ms, text);
        return emit_prefixed_cstr(w, "~t", text, as_key);
    }
    if (as_key || !w->msgpack) {
        char digits[24];
        snprintf(digits, sizeof(digits), "%lld", (long long)ms);
        return emit_prefixed_cstr(w, "~m", digits, as_key);
    }
    return tagged_start(w, "m", 1) || pack_int(w, ms) || tagged_end(w);
}

static int64_t int64_from_bytes(const unsigned char *b) {
    uint64_t u = 0;
    int i;
    for (i = 0; i < 8; i++) u = (u << 8) | b[i];
    return (int64_t)u;
}

static int marshal_uuid(writer *w, const unsigned char *u, int as_key) {
    if (as_key || !w->msgpack) {
        char text[37];
        format_uuid(u, text);
        return emit_prefixed_cstr(w, "~u", text, as_key);
    }
    /* two signed 64 bit ints, most significant first */
    return tagged_start(w, "u", 1) || pack_header(w, 2, 0x90, 0xdc, 0xdd) ||
           pack_int(w, int64_from_bytes(u)) || pack_int(w, int64_from_bytes(u + 8)) || tagged_end(w);
}

static int marshal(writer *w, const transit_value *v, int as_key) {
    int r;
    if (!v) return fail(w, TRANSIT_ERROR_UNSUPPORTED, "a NULL value");
    if (++w->depth > MAX_DEPTH) return fail(w, TRANSIT_ERROR_UNSUPPORTED, "nested too deeply");
    /* msgpack map keys can be any type, so like transit-java, nil,
     * booleans, ints and floats are written as themselves there. */
    if (as_key && w->msgpack && (v->type == TRANSIT_NIL || v->type == TRANSIT_BOOL ||
                                 v->type == TRANSIT_INT || v->type == TRANSIT_FLOAT)) as_key = 0;
    switch (v->type) {
    case TRANSIT_NIL:
        if (as_key) r = emit_prefixed_cstr(w, "~_", "", 1);
        else r = w->msgpack ? put_byte(w, 0xc0) : put_cstr(w, "null");
        break;
    case TRANSIT_BOOL:
        if (as_key) r = emit_prefixed_cstr(w, v->u.boolean ? "~?t" : "~?f", "", 1);
        else if (w->msgpack) r = put_byte(w, v->u.boolean ? 0xc3 : 0xc2);
        else r = put_cstr(w, v->u.boolean ? "true" : "false");
        break;
    case TRANSIT_INT: r = marshal_int(w, v->u.integer, as_key); break;
    case TRANSIT_FLOAT: r = marshal_float(w, v->u.number, as_key); break;
    case TRANSIT_BIGINT: r = emit_prefixed(w, "~n", v->u.str.data, v->u.str.len, as_key); break;
    case TRANSIT_DECIMAL: r = emit_prefixed(w, "~f", v->u.str.data, v->u.str.len, as_key); break;
    case TRANSIT_KEYWORD: r = emit_prefixed(w, "~:", v->u.str.data, v->u.str.len, as_key); break;
    case TRANSIT_SYMBOL: r = emit_prefixed(w, "~$", v->u.str.data, v->u.str.len, as_key); break;
    case TRANSIT_URI: r = emit_prefixed(w, "~r", v->u.str.data, v->u.str.len, as_key); break;
    case TRANSIT_STRING: {
        const char *s = v->u.str.data;
        size_t n = v->u.str.len;
        if (n && (s[0] == '~' || s[0] == '^' || s[0] == '`')) r = emit_prefixed(w, "~", s, n, as_key);
        else r = emit_str(w, s, n, as_key);
        break;
    }
    case TRANSIT_BYTES: r = marshal_bytes(w, v, as_key); break;
    case TRANSIT_TIME: r = marshal_time(w, v->u.integer, as_key); break;
    case TRANSIT_UUID: r = marshal_uuid(w, v->u.uuid, as_key); break;
    case TRANSIT_ARRAY: r = emit_items(w, v->u.coll.items, v->u.coll.count); break;
    case TRANSIT_LIST:
    case TRANSIT_SET:
        r = tagged_start(w, v->type == TRANSIT_SET ? "set" : "list", v->type == TRANSIT_SET ? 3 : 4) ||
            emit_items(w, v->u.coll.items, v->u.coll.count) || tagged_end(w);
        break;
    case TRANSIT_MAP: r = marshal_map(w, v); break;
    case TRANSIT_TAGGED:
        if (is_scalar_tagged(v)) {
            char prefix[8];
            prefix[0] = '~';
            memcpy(prefix + 1, v->u.tagged.tag, v->u.tagged.tag_len);
            prefix[1 + v->u.tagged.tag_len] = '\0';
            r = emit_prefixed(w, prefix, v->u.tagged.rep->u.str.data, v->u.tagged.rep->u.str.len, as_key);
        } else {
            r = tagged_start(w, v->u.tagged.tag, v->u.tagged.tag_len) || marshal(w, v->u.tagged.rep, 0) ||
                tagged_end(w);
        }
        break;
    default:
        r = fail(w, TRANSIT_ERROR_UNSUPPORTED, "unknown value type");
    }
    w->depth--;
    return r;
}

int transit_write(const transit_value *value, transit_format format, transit_buffer *out, transit_error *err) {
    writer w;
    transit_error local;
    int r;
    if (!err) err = &local;
    clear_error(err);
    w.verbose = format == TRANSIT_JSON_VERBOSE;
    w.msgpack = format == TRANSIT_MSGPACK;
    w.out = out;
    w.err = err;
    w.depth = 0;
    w.scratch = transit_doc_new();
    if (!w.scratch || cache_init(&w.cache, 1)) {
        transit_doc_free(w.scratch);
        set_error(err, TRANSIT_ERROR_MEMORY, 0, "out of memory");
        return -1;
    }
    if (value && is_stringable(value)) {
        /* scalars at the top level are quoted: ["~#'", value] */
        r = tagged_start(&w, "'", 1) || marshal(&w, value, 0) || tagged_end(&w);
    } else {
        r = marshal(&w, value, 0);
    }
    cache_free(&w.cache);
    transit_doc_free(w.scratch);
    return r ? -1 : 0;
}
