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

/* Parsing JSON into wire values. Numbers keep their type as written:
 * integers (without a fraction or exponent) are ints, or bigints beyond 64
 * bits, and everything else is a float. */

#include "transit_internal.h"

#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 10000

typedef struct {
    const char *start, *p, *end;
    transit_doc *doc;
    transit_error *err;
    int depth;
    transit_buffer scratch;
} json_parser;

static transit_value *parse_value(json_parser *j);

static size_t offset(const json_parser *j) { return (size_t)(j->p - j->start); }

static void syntax_error(json_parser *j, const char *what) {
    if (j->p >= j->end) set_error(j->err, TRANSIT_ERROR_TRUNCATED, offset(j), "JSON ended unexpectedly: %s", what);
    else set_error(j->err, TRANSIT_ERROR_SYNTAX, offset(j), "invalid JSON: %s", what);
}

static void skip_ws(json_parser *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static int put_utf8(transit_buffer *b, unsigned cp) {
    unsigned char u[4];
    size_t n;
    if (cp < 0x80) { u[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (unsigned char)(0xc0 | (cp >> 6)); u[1] = (unsigned char)(0x80 | (cp & 0x3f)); n = 2; }
    else if (cp < 0x10000) {
        u[0] = (unsigned char)(0xe0 | (cp >> 12)); u[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        u[2] = (unsigned char)(0x80 | (cp & 0x3f)); n = 3;
    } else {
        u[0] = (unsigned char)(0xf0 | (cp >> 18)); u[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
        u[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f)); u[3] = (unsigned char)(0x80 | (cp & 0x3f)); n = 4;
    }
    return buf_put(b, u, n);
}

/* Parses the string at j->p (just after the opening quote). */
static transit_value *parse_string(json_parser *j) {
    const char *s = j->p;
    transit_value *v;
    /* fast path: no escapes */
    while (j->p < j->end && *j->p != '"' && *j->p != '\\' && (unsigned char)*j->p >= 0x20) j->p++;
    if (j->p < j->end && *j->p == '"') {
        v = transit_string(j->doc, s, (size_t)(j->p - s));
        j->p++;
        return v;
    }
    j->scratch.len = 0;
    if (buf_put(&j->scratch, s, (size_t)(j->p - s))) goto memory;
    while (j->p < j->end && *j->p != '"') {
        unsigned char c = (unsigned char)*j->p;
        if (c < 0x20) {
            syntax_error(j, "control character in string");
            return NULL;
        }
        if (c != '\\') {
            if (buf_byte(&j->scratch, c)) goto memory;
            j->p++;
            continue;
        }
        if (++j->p >= j->end) break;
        switch (*j->p) {
        case '"': c = '"'; break;
        case '\\': c = '\\'; break;
        case '/': c = '/'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'u': {
            unsigned cp, lo;
            if (j->end - j->p < 5 || hex4(j->p + 1, &cp)) {
                syntax_error(j, "bad \\u escape");
                return NULL;
            }
            j->p += 5;
            if (cp >= 0xd800 && cp < 0xdc00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u' &&
                hex4(j->p + 2, &lo) == 0 && lo >= 0xdc00 && lo < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                j->p += 6;
            }
            if (put_utf8(&j->scratch, cp)) goto memory;
            continue;
        }
        default:
            syntax_error(j, "bad escape");
            return NULL;
        }
        if (buf_byte(&j->scratch, c)) goto memory;
        j->p++;
    }
    if (j->p >= j->end) {
        syntax_error(j, "unterminated string");
        return NULL;
    }
    j->p++;
    return transit_string(j->doc, (const char *)j->scratch.data, j->scratch.len);
memory:
    set_error(j->err, TRANSIT_ERROR_MEMORY, offset(j), "out of memory");
    return NULL;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static transit_value *parse_number(json_parser *j) {
    const char *s = j->p;
    int is_float = 0;
    int64_t i;
    double d;
    int r;
    if (*j->p == '-') j->p++;
    if (j->p >= j->end || !is_digit(*j->p)) {
        syntax_error(j, "bad number");
        return NULL;
    }
    if (*j->p == '0') j->p++;
    else while (j->p < j->end && is_digit(*j->p)) j->p++;
    if (j->p < j->end && *j->p == '.') {
        is_float = 1;
        j->p++;
        if (j->p >= j->end || !is_digit(*j->p)) {
            syntax_error(j, "bad number");
            return NULL;
        }
        while (j->p < j->end && is_digit(*j->p)) j->p++;
    }
    if (j->p < j->end && (*j->p == 'e' || *j->p == 'E')) {
        is_float = 1;
        j->p++;
        if (j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++;
        if (j->p >= j->end || !is_digit(*j->p)) {
            syntax_error(j, "bad number");
            return NULL;
        }
        while (j->p < j->end && is_digit(*j->p)) j->p++;
    }
    if (is_float) {
        if (parse_double(s, (size_t)(j->p - s), &d)) {
            syntax_error(j, "bad number");
            return NULL;
        }
        return transit_float(j->doc, d);
    }
    r = parse_int64(s, (size_t)(j->p - s), &i);
    if (r == 0) return transit_int(j->doc, i);
    return transit_bigint(j->doc, s, (size_t)(j->p - s));
}

static int literal(json_parser *j, const char *word) {
    size_t n = strlen(word);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0) return -1;
    j->p += n;
    return 0;
}

static transit_value *parse_array(json_parser *j) {
    transit_value *a = transit_array(j->doc);
    if (!a) goto memory;
    skip_ws(j);
    if (j->p < j->end && *j->p == ']') {
        j->p++;
        return a;
    }
    for (;;) {
        transit_value *item = parse_value(j);
        if (!item) return NULL;
        if (transit_push(j->doc, a, item)) goto memory;
        skip_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == ']') {
            j->p++;
            return a;
        }
        syntax_error(j, "expected ',' or ']'");
        return NULL;
    }
memory:
    set_error(j->err, TRANSIT_ERROR_MEMORY, offset(j), "out of memory");
    return NULL;
}

static transit_value *parse_object(json_parser *j) {
    transit_value *m = transit_map(j->doc);
    if (!m) goto memory;
    skip_ws(j);
    if (j->p < j->end && *j->p == '}') {
        j->p++;
        return m;
    }
    for (;;) {
        transit_value *k, *v;
        skip_ws(j);
        if (j->p >= j->end || *j->p != '"') {
            syntax_error(j, "expected a string key");
            return NULL;
        }
        j->p++;
        if (!(k = parse_string(j))) return NULL;
        skip_ws(j);
        if (j->p >= j->end || *j->p != ':') {
            syntax_error(j, "expected ':'");
            return NULL;
        }
        j->p++;
        if (!(v = parse_value(j))) return NULL;
        if (transit_map_put(j->doc, m, k, v)) goto memory;
        skip_ws(j);
        if (j->p < j->end && *j->p == ',') {
            j->p++;
            continue;
        }
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            return m;
        }
        syntax_error(j, "expected ',' or '}'");
        return NULL;
    }
memory:
    set_error(j->err, TRANSIT_ERROR_MEMORY, offset(j), "out of memory");
    return NULL;
}

static transit_value *parse_value(json_parser *j) {
    transit_value *v = NULL;
    skip_ws(j);
    if (j->p >= j->end) {
        syntax_error(j, "expected a value");
        return NULL;
    }
    if (++j->depth > MAX_DEPTH) {
        set_error(j->err, TRANSIT_ERROR_SYNTAX, offset(j), "JSON nested too deeply");
        return NULL;
    }
    switch (*j->p) {
    case '[': j->p++; v = parse_array(j); break;
    case '{': j->p++; v = parse_object(j); break;
    case '"': j->p++; v = parse_string(j); break;
    case 't': v = literal(j, "true") ? NULL : transit_bool(j->doc, 1); break;
    case 'f': v = literal(j, "false") ? NULL : transit_bool(j->doc, 0); break;
    case 'n': v = literal(j, "null") ? NULL : transit_nil(j->doc); break;
    default:
        if (*j->p == '-' || is_digit(*j->p)) v = parse_number(j);
        break;
    }
    if (!v) syntax_error(j, "unexpected character");
    j->depth--;
    return v;
}

transit_value *json_parse_wire(transit_doc *doc, const char *data, size_t len, transit_error *err) {
    json_parser j;
    transit_value *v;
    j.start = j.p = data;
    j.end = data + len;
    j.doc = doc;
    j.err = err;
    j.depth = 0;
    j.scratch.data = NULL;
    j.scratch.len = j.scratch.cap = 0;
    v = parse_value(&j);
    if (v) {
        skip_ws(&j);
        if (j.p != j.end) {
            syntax_error(&j, "unexpected data after the value");
            v = NULL;
        }
    }
    transit_buffer_free(&j.scratch);
    return v;
}
