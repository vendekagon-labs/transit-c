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

/* Tests. The exemplar tests read transit-format's example files from
 * $TRANSIT_FORMAT_DIR (which CMake sets to a checkout next to this one). */

#include "transit.h"

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, failures;

#define CHECK(cond, ...)                                        \
    do {                                                        \
        checks++;                                               \
        if (!(cond)) {                                          \
            failures++;                                         \
            fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);     \
            fprintf(stderr, __VA_ARGS__);                       \
            fputc('\n', stderr);                                \
        }                                                       \
    } while (0)

static transit_doc *doc;   /* holds the values the tests build and read */

/* Building values */

#define S(s) (s), strlen(s)

static transit_value *str(const char *s) { return transit_string(doc, S(s)); }
static transit_value *kw(const char *s) { return transit_keyword(doc, S(s)); }
static transit_value *sym(const char *s) { return transit_symbol(doc, S(s)); }
static transit_value *num(int64_t i) { return transit_int(doc, i); }
static transit_value *flt(double d) { return transit_float(doc, d); }

static transit_value *array_of(int n, ...) {
    transit_value *a = transit_array(doc);
    va_list ap;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) transit_push(doc, a, va_arg(ap, transit_value *));
    va_end(ap);
    return a;
}

static transit_value *as(transit_type type, transit_value *coll) {
    coll->type = type;
    return coll;
}

static transit_value *map_of(int n, ...) {
    transit_value *m = transit_map(doc);
    va_list ap;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        transit_value *k = va_arg(ap, transit_value *);
        transit_map_put(doc, m, k, va_arg(ap, transit_value *));
    }
    va_end(ap);
    return m;
}

/* Byte buffers */

static void append(transit_buffer *b, const void *data, size_t n) {
    if (b->cap - b->len < n + 1) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap - b->len < n + 1) cap *= 2;
        b->data = (unsigned char *)realloc(b->data, cap);
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void append_str(transit_buffer *b, const char *s) { append(b, s, strlen(s)); }

/* Writing and reading */

/* v written in format, NUL terminated; free()d by the caller. */
static char *written(const transit_value *v, transit_format format, size_t *len) {
    transit_buffer b = {0};
    transit_error err;
    char *s;
    if (transit_write(v, format, &b, &err) != 0) {
        fprintf(stderr, "write failed: %s\n", err.message);
        transit_buffer_free(&b);
        return NULL;
    }
    s = (char *)malloc(b.len + 1);
    if (b.len) memcpy(s, b.data, b.len);
    s[b.len] = '\0';
    if (len) *len = b.len;
    transit_buffer_free(&b);
    return s;
}

static char *json(const transit_value *v) { return written(v, TRANSIT_JSON, NULL); }

static transit_value *roundtrip(const transit_value *v, transit_format format) {
    size_t len;
    char *s = written(v, format, &len);
    transit_value *back;
    transit_error err;
    if (!s) return NULL;
    back = transit_read(doc, s, len, format, &err);
    if (!back) fprintf(stderr, "read back failed: %s\n", err.message);
    free(s);
    return back;
}

static void check_written(const transit_value *v, transit_format format, const char *expected, int line) {
    char *s = written(v, format, NULL);
    checks++;
    if (!s || strcmp(s, expected) != 0) {
        failures++;
        fprintf(stderr, "%s:%d: wrote %s\n  expected %s\n", __FILE__, line, s ? s : "(error)", expected);
    }
    free(s);
}
#define CHECK_JSON(v, expected) check_written((v), TRANSIT_JSON, (expected), __LINE__)
#define CHECK_VERBOSE(v, expected) check_written((v), TRANSIT_JSON_VERBOSE, (expected), __LINE__)

static transit_value *read_json(const char *s) {
    transit_error err;
    transit_value *v = transit_read(doc, S(s), TRANSIT_JSON, &err);
    if (!v) fprintf(stderr, "reading %s: %s\n", s, err.message);
    return v;
}

static transit_value *uuid(const char *text) {
    char text_json[64];
    snprintf(text_json, sizeof(text_json), "[\"~#'\",\"~u%s\"]", text);
    return read_json(text_json);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    char *data;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char *)malloc((size_t)n + 1);
    if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) {
        data[n] = '\0';
        *len = (size_t)n;
    }
    return data;
}

/* The value transit reads for integer text: an int, or a bigint beyond 64 bits. */
static transit_value *int_text(const char *text) {
    char text_json[64];
    snprintf(text_json, sizeof(text_json), "[\"~#'\",\"~i%s\"]", text);
    return read_json(text_json);
}

/* Decimal text of 2^k + d, for small d, possibly negated. */
static void pow2_text(int k, int d, int negate, char *out) {
    unsigned char digits[40];   /* least significant first */
    int n = 1, i, carry, v;
    if (k <= 62) {
        int64_t x = ((int64_t)1 << k) + d;
        snprintf(out, 48, "%lld", (long long)(negate ? -x : x));
        return;
    }
    memset(digits, 0, sizeof(digits));
    digits[0] = 1;
    while (k--) {
        carry = 0;
        for (i = 0; i < n; i++) {
            v = digits[i] * 2 + carry;
            digits[i] = (unsigned char)(v % 10);
            carry = v / 10;
        }
        if (carry) digits[n++] = (unsigned char)carry;
    }
    /* 2^k for k > 62 ends in 2, 4, 6 or 8, so adding d carries at most once */
    v = digits[0] + d;
    if (v >= 10) {
        digits[0] = (unsigned char)(v - 10);
        for (i = 1; digits[i] == 9; i++) digits[i] = 0;
        digits[i]++;
        if (i == n) n++;
    } else {
        digits[0] = (unsigned char)v;
    }
    i = 0;
    if (negate) out[i++] = '-';
    while (n) out[i++] = (char)('0' + digits[--n]);
    out[i] = '\0';
}

/* Exemplars */

static const char *EXEMPLARS[] = {
    "cmap_null_key", "cmap_pathological", "dates_interesting", "doubles_interesting", "doubles_small",
    "false", "ints", "ints_interesting", "ints_interesting_neg", "keywords", "list_empty", "list_mixed",
    "list_nested", "list_simple", "map_10_items", "map_10_nested", "map_1935_nested", "map_1936_nested",
    "map_1937_nested", "map_mixed", "map_nested", "map_numeric_keys", "map_simple", "map_string_keys",
    "map_unrecognized_vals", "map_vector_keys", "maps_four_char_keyword_keys", "maps_four_char_string_keys",
    "maps_four_char_sym_keys", "maps_three_char_keyword_keys", "maps_three_char_string_keys",
    "maps_three_char_sym_keys", "maps_two_char_keyword_keys", "maps_two_char_string_keys",
    "maps_two_char_sym_keys", "maps_unrecognized_keys", "nil", "one", "one_date", "one_keyword",
    "one_string", "one_symbol", "one_uri", "one_uuid", "set_empty", "set_mixed", "set_nested",
    "set_simple", "small_ints", "small_strings", "strings_hash", "strings_hat", "strings_tilde",
    "symbols", "true", "uris", "uuids", "vector_1935_keywords_repeated_twice",
    "vector_1936_keywords_repeated_twice", "vector_1937_keywords_repeated_twice", "vector_empty",
    "vector_mixed", "vector_nested", "vector_simple", "vector_special_numbers",
    "vector_unrecognized_vals", "zero"};
#define N_EXEMPLARS (sizeof(EXEMPLARS) / sizeof(EXEMPLARS[0]))

static const char *format_dir(void) {
    const char *dir = getenv("TRANSIT_FORMAT_DIR");
    return dir && *dir ? dir : "../transit-format";
}

static char *exemplar_file(const char *name, const char *ext, size_t *len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/examples/0.8/simple/%s%s", format_dir(), name, ext);
    return read_file(path, len);
}

static transit_value *read_exemplar(const char *name, const char *ext, transit_format format) {
    size_t len;
    char *data = exemplar_file(name, ext, &len);
    transit_value *v;
    transit_error err;
    if (!data) return NULL;
    v = transit_read(doc, data, len, format, &err);
    if (!v) fprintf(stderr, "reading %s%s: %s\n", name, ext, err.message);
    free(data);
    return v;
}

static transit_value *small_strings(const char *prefix) {
    static const char *strings[] = {"", "a", "ab", "abc", "abcd", "abcde", "abcdef"};
    transit_value *a = transit_array(doc);
    size_t i;
    for (i = 0; i < 7; i++) {
        char s[16];
        snprintf(s, sizeof(s), "%s%s", prefix, strings[i]);
        transit_push(doc, a, str(s));
    }
    return a;
}

static transit_value *key_map(int n) {
    transit_value *m = transit_map(doc);
    int i;
    for (i = 0; i < n; i++) {
        char k[16];
        snprintf(k, sizeof(k), "key%04d", i);
        transit_map_put(doc, m, kw(k), num(i));
    }
    return m;
}

static transit_value *interesting_ints(int negate) {
    transit_value *a = transit_array(doc);
    int k, d;
    for (k = 0; k <= 65; k++)
        for (d = -2; d <= 2; d++) {
            char text[48];
            pow2_text(k, d, negate, text);
            transit_push(doc, a, int_text(text));
        }
    return a;
}

static transit_value *ints_from(int from, int to) {
    transit_value *a = transit_array(doc);
    int i;
    for (i = from; i <= to; i++) transit_push(doc, a, num(i));
    return a;
}

/* Expected values, built independently of reading, for most of the
 * exemplars (all are also checked for agreement between the encodings and
 * with transit-java's msgpack, and by the verify harness). */
static transit_value *expected_value(const char *name) {
    transit_value *mixed = array_of(10, num(0), num(1), flt(2.0), transit_bool(doc, 1), transit_bool(doc, 0),
                                    str("five"), kw("six"), sym("seven"), str("~eight"), transit_nil(doc));
    transit_value *simple = array_of(3, num(1), num(2), num(3));
    if (!strcmp(name, "nil")) return transit_nil(doc);
    if (!strcmp(name, "true")) return transit_bool(doc, 1);
    if (!strcmp(name, "false")) return transit_bool(doc, 0);
    if (!strcmp(name, "zero")) return num(0);
    if (!strcmp(name, "one")) return num(1);
    if (!strcmp(name, "one_string")) return str("hello");
    if (!strcmp(name, "one_keyword")) return kw("hello");
    if (!strcmp(name, "one_symbol")) return sym("hello");
    if (!strcmp(name, "one_date")) return transit_time(doc, 946728000000LL);
    if (!strcmp(name, "one_uuid")) return uuid("5a2cbea3-e8c6-428b-b525-21239370dd55");
    if (!strcmp(name, "one_uri")) return transit_uri(doc, S("http://example.com"));
    if (!strcmp(name, "vector_simple")) return simple;
    if (!strcmp(name, "vector_empty")) return transit_array(doc);
    if (!strcmp(name, "vector_mixed")) return mixed;
    if (!strcmp(name, "vector_nested")) return array_of(2, simple, mixed);
    if (!strcmp(name, "list_simple")) return as(TRANSIT_LIST, array_of(3, num(1), num(2), num(3)));
    if (!strcmp(name, "list_empty")) return transit_list(doc);
    if (!strcmp(name, "set_simple")) return as(TRANSIT_SET, array_of(3, num(3), num(1), num(2)));
    if (!strcmp(name, "set_empty")) return transit_set(doc);
    if (!strcmp(name, "small_strings")) return small_strings("");
    if (!strcmp(name, "strings_tilde")) return small_strings("~");
    if (!strcmp(name, "strings_hash")) return small_strings("#");
    if (!strcmp(name, "strings_hat")) return small_strings("^");
    if (!strcmp(name, "ints")) return ints_from(0, 127);
    if (!strcmp(name, "small_ints")) return ints_from(-5, 5);
    if (!strcmp(name, "ints_interesting")) return interesting_ints(0);
    if (!strcmp(name, "ints_interesting_neg")) return interesting_ints(1);
    if (!strcmp(name, "doubles_interesting"))
        return array_of(5, flt(-3.14159), flt(3.14159), flt(4E11), flt(2.998E8), flt(6.626E-34));
    if (!strcmp(name, "vector_special_numbers")) return array_of(3, flt(NAN), flt(INFINITY), flt(-INFINITY));
    if (!strcmp(name, "dates_interesting"))
        return array_of(4, transit_time(doc, -6106017600000LL), transit_time(doc, 0),
                        transit_time(doc, 946728000000LL), transit_time(doc, 1396909037000LL));
    if (!strcmp(name, "uris"))
        return array_of(4, transit_uri(doc, S("http://example.com")), transit_uri(doc, S("ftp://example.com")),
                        transit_uri(doc, S("file:///path/to/file.txt")),
                        transit_uri(doc, S("http://www.\xe8\xa9\xb9\xe5\xa7\x86\xe6\x96\xaf.com/")));
    if (!strcmp(name, "keywords"))
        return array_of(9, kw("a"), kw("ab"), kw("abc"), kw("abcd"), kw("abcde"), kw("a1"), kw("b2"), kw("c3"), kw("a_b"));
    if (!strcmp(name, "symbols"))
        return array_of(9, sym("a"), sym("ab"), sym("abc"), sym("abcd"), sym("abcde"), sym("a1"), sym("b2"), sym("c3"), sym("a_b"));
    if (!strcmp(name, "map_simple")) return map_of(3, kw("a"), num(1), kw("b"), num(2), kw("c"), num(3));
    if (!strcmp(name, "map_mixed")) return map_of(3, kw("a"), num(1), kw("b"), str("a string"), kw("c"), transit_bool(doc, 1));
    if (!strcmp(name, "map_string_keys")) return map_of(3, str("first"), num(1), str("second"), num(2), str("third"), num(3));
    if (!strcmp(name, "map_numeric_keys")) return map_of(2, num(1), str("one"), num(2), str("two"));
    if (!strcmp(name, "map_vector_keys"))
        return map_of(2, array_of(2, num(1), num(1)), str("one"), array_of(2, num(2), num(2)), str("two"));
    if (!strcmp(name, "map_unrecognized_vals")) return map_of(1, kw("key"), str("~Unrecognized"));
    if (!strcmp(name, "vector_unrecognized_vals")) return array_of(1, str("~Unrecognized"));
    if (!strcmp(name, "maps_unrecognized_keys"))
        return array_of(2, transit_tagged(doc, S("abcde"), kw("anything")),
                        transit_tagged(doc, S("fghij"), kw("anything-else")));
    if (!strcmp(name, "map_10_items")) return key_map(10);
    if (!strcmp(name, "map_10_nested")) return map_of(2, kw("f"), key_map(10), kw("s"), key_map(10));
    if (!strcmp(name, "map_1937_nested")) return map_of(2, kw("f"), key_map(1937), kw("s"), key_map(1937));
    if (!strcmp(name, "cmap_null_key"))
        return map_of(2, transit_nil(doc), str("null as map key"), array_of(2, num(1), num(2)),
                      str("Array as key to force cmap"));
    if (!strcmp(name, "cmap_pathological"))
        return array_of(2, map_of(1, kw("any-value"),
                                  map_of(2, array_of(1, str("this vector makes this a cmap")), str("any value"),
                                         str("any string"), kw("victim"))),
                        map_of(1, kw("victim"), kw("any-other-value")));
    return NULL;
}

static void test_exemplars(void) {
    size_t i, len, with_expected = 0;
    char *probe = exemplar_file("nil", ".json", &len);
    if (!probe) {
        printf("SKIP exemplars: transit-format not found at %s\n", format_dir());
        return;
    }
    free(probe);
    for (i = 0; i < N_EXEMPLARS; i++) {
        const char *name = EXEMPLARS[i];
        size_t mp_len, out_len;
        char *mp = exemplar_file(name, ".mp", &mp_len), *out, *edn;
        transit_value *j = read_exemplar(name, ".json", TRANSIT_JSON);
        transit_value *jv = read_exemplar(name, ".verbose.json", TRANSIT_JSON_VERBOSE);
        transit_value *m = mp ? transit_read(doc, mp, mp_len, TRANSIT_MSGPACK, NULL) : NULL;
        transit_value *expected = expected_value(name);
        edn = exemplar_file(name, ".edn", &len);
        CHECK(edn != NULL, "%s: no such exemplar", name);
        free(edn);
        CHECK(j && jv && m, "%s: couldn't read every encoding", name);
        if (!j || !jv || !m) {
            free(mp);
            continue;
        }
        CHECK(transit_equal(j, jv), "%s: json and json-verbose differ", name);
        CHECK(transit_equal(j, m), "%s: json and msgpack differ", name);
        if (expected) {
            with_expected++;
            CHECK(transit_equal(j, expected), "%s: not the expected value", name);
        }
        CHECK(transit_equal(roundtrip(j, TRANSIT_JSON), j), "%s: json roundtrip", name);
        CHECK(transit_equal(roundtrip(j, TRANSIT_JSON_VERBOSE), j), "%s: json-verbose roundtrip", name);
        CHECK(transit_equal(roundtrip(j, TRANSIT_MSGPACK), j), "%s: msgpack roundtrip", name);
        /* entries keep their order, so this is exactly what transit-java wrote */
        out = written(m, TRANSIT_MSGPACK, &out_len);
        CHECK(out && out_len == mp_len && memcmp(out, mp, mp_len) == 0, "%s: msgpack differs from transit-java's", name);
        free(out);
        free(mp);
    }
    CHECK(with_expected >= 45, "only %d exemplars have expected values", (int)with_expected);
    printf("exemplars: %d, %d with independently built expected values\n", (int)N_EXEMPLARS, (int)with_expected);
}

/* The cache: codes must be assigned as transit-clj does, including when the
 * cache fills up and starts over. These payloads are what transit-clj
 * writes, built here by hand. */

#define WRAP_N 1937

static transit_value *wrapping_value(void) {
    char last[16];
    snprintf(last, sizeof(last), "key%04d", WRAP_N - 1);
    return array_of(4, key_map(WRAP_N), map_of(1, kw(last), kw("last")), map_of(1, kw("key0000"), kw("first")),
                    map_of(1, kw(last), kw("last-again")));
}

static void test_cache(void) {
    static const unsigned char mp_tail[] = {
        0x81, 0xa2, '^', '0', 0xa6, '~', ':', 'l', 'a', 's', 't',
        0x81, 0xa9, '~', ':', 'k', 'e', 'y', '0', '0', '0', '0', 0xa7, '~', ':', 'f', 'i', 'r', 's', 't',
        0x81, 0xa2, '^', '0', 0xac, '~', ':', 'l', 'a', 's', 't', '-', 'a', 'g', 'a', 'i', 'n'};
    transit_buffer expected_json = {0}, expected_mp = {0};
    transit_value *v = wrapping_value(), *back;
    unsigned char head[] = {0x94, 0xde, WRAP_N >> 8, WRAP_N & 0xff};
    char *out, item[64];
    size_t len;
    int i;

    for (i = 0; i < WRAP_N; i++) {
        snprintf(item, sizeof(item), "%s\"~:key%04d\",%d", i ? "," : "[[\"^ \",", i, i);
        append_str(&expected_json, item);
    }
    append_str(&expected_json, "],[\"^ \",\"^0\",\"~:last\"],[\"^ \",\"~:key0000\",\"~:first\"],[\"^ \",\"^0\",\"~:last-again\"]]");
    out = json(v);
    CHECK(out && strcmp(out, (const char *)expected_json.data) == 0, "cache rollover: JSON differs from transit-clj's");
    free(out);
    back = transit_read(doc, expected_json.data, expected_json.len, TRANSIT_JSON, NULL);
    CHECK(back && transit_equal(back, v), "cache rollover: reading transit-clj's JSON");

    append(&expected_mp, head, sizeof(head));
    for (i = 0; i < WRAP_N; i++) {
        unsigned char b[16];
        size_t n = 0;
        b[n++] = 0xa9;   /* fixstr of 9 */
        snprintf((char *)b + n, 10, "~:key%04d", i);
        n += 9;
        if (i <= 127) b[n++] = (unsigned char)i;
        else if (i <= 255) { b[n++] = 0xcc; b[n++] = (unsigned char)i; }
        else { b[n++] = 0xcd; b[n++] = (unsigned char)(i >> 8); b[n++] = (unsigned char)(i & 0xff); }
        append(&expected_mp, b, n);
    }
    append(&expected_mp, mp_tail, sizeof(mp_tail));
    out = written(v, TRANSIT_MSGPACK, &len);
    CHECK(out && len == expected_mp.len && memcmp(out, expected_mp.data, len) == 0, "cache rollover: msgpack differs from transit-clj's");
    free(out);
    back = transit_read(doc, expected_mp.data, expected_mp.len, TRANSIT_MSGPACK, NULL);
    CHECK(back && transit_equal(back, v), "cache rollover: reading transit-clj's msgpack");
    transit_buffer_free(&expected_json);
    transit_buffer_free(&expected_mp);

    /* the first element of an array is only cached once, though it's
     * inspected for a tag first */
    CHECK(transit_equal(read_json("[[\"~:aaaa\",\"~:bbbb\"],\"^1\"]"),
                        array_of(2, array_of(2, kw("aaaa"), kw("bbbb")), kw("bbbb"))), "first element cached once");
}

/* Numbers */

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_numbers(void) {
    transit_value *v = read_json("[3000000000,3000000000.0,1,1.0,9007199254740993,-0.0,1E3,\"~i-9223372036854775808\",99999999999999999999]");
    transit_value *a, *m;
    int i;
    CHECK(v && v->u.coll.count == 9, "numbers read");
    if (v && v->u.coll.count == 9) {
        CHECK(v->u.coll.items[0]->type == TRANSIT_INT && v->u.coll.items[0]->u.integer == 3000000000LL, "3000000000 is an int");
        CHECK(v->u.coll.items[1]->type == TRANSIT_FLOAT && v->u.coll.items[1]->u.number == 3e9, "3000000000.0 is a float");
        CHECK(v->u.coll.items[4]->type == TRANSIT_INT && v->u.coll.items[4]->u.integer == 9007199254740993LL, "exact beyond 2^53");
        CHECK(v->u.coll.items[5]->type == TRANSIT_FLOAT && signbit(v->u.coll.items[5]->u.number), "-0.0 keeps its sign");
        CHECK(v->u.coll.items[6]->type == TRANSIT_FLOAT, "1E3 is a float");
        CHECK(v->u.coll.items[7]->type == TRANSIT_INT && v->u.coll.items[7]->u.integer == INT64_MIN, "int64 min");
        CHECK(v->u.coll.items[8]->type == TRANSIT_BIGINT, "beyond 64 bits is a bigint");
    }
    CHECK_JSON(array_of(4, num(9007199254740991LL), num(9007199254740992LL), num(INT64_MIN), transit_bigint(doc, S("123"))),
               "[9007199254740991,\"~i9007199254740992\",\"~i-9223372036854775808\",\"~n123\"]");
    CHECK_JSON(array_of(10, flt(0.1), flt(1.0 / 3), flt(2), flt(-0.0), flt(1e16), flt(6.626e-34), flt(4e11), flt(1.5e-5),
                        flt(1.7976931348623157e308), flt(4.9406564584124654e-324)),
               "[0.1,0.3333333333333333,2.0,-0.0,1e+16,6.626e-34,400000000000.0,1.5e-05,1.7976931348623157e+308,4.94065645841247e-324]");
    /* random doubles read back exactly, in every format */
    a = transit_array(doc);
    for (i = 0; i < 100000; i++) {
        uint64_t bits = next_random();
        double d;
        memcpy(&d, &bits, 8);
        if (d != d || d == INFINITY || d == -INFINITY) continue;
        transit_push(doc, a, flt(d));
    }
    CHECK(transit_equal(roundtrip(a, TRANSIT_JSON), a), "random doubles, json");
    CHECK(transit_equal(roundtrip(a, TRANSIT_JSON_VERBOSE), a), "random doubles, json-verbose");
    CHECK(transit_equal(roundtrip(a, TRANSIT_MSGPACK), a), "random doubles, msgpack");
    m = transit_map(doc);
    for (i = 0; i < 200; i++) transit_map_put(doc, m, a->u.coll.items[i], num(i));
    CHECK(transit_equal(roundtrip(m, TRANSIT_JSON), m), "doubles as map keys");
}

static void test_msgpack_ints(void) {
    static const struct { int64_t i; const char *hex; } cases[] = {
        {0, "00"}, {127, "7f"}, {128, "cc80"}, {255, "ccff"}, {256, "cd0100"}, {65535, "cdffff"},
        {65536, "ce00010000"}, {4294967295LL, "ceffffffff"}, {4294967296LL, "cf0000000100000000"},
        {-1, "ff"}, {-32, "e0"}, {-33, "d0df"}, {-128, "d080"}, {-129, "d1ff7f"}, {-32768, "d18000"},
        {-32769, "d2ffff7fff"}, {-2147483647LL - 1, "d280000000"}, {-2147483649LL, "d3ffffffff7fffffff"},
        {INT64_MAX, "cf7fffffffffffffff"}, {INT64_MIN, "d38000000000000000"}};
    static const unsigned char u64[] = {0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const unsigned char f32[] = {0xca, 0x3f, 0xc0, 0, 0};
    static const unsigned char bin[] = {0x91, 0xc4, 0x02, 0x01, 0x02};
    size_t i, len, k;
    transit_value *v;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char hex[40];
        unsigned char *out = (unsigned char *)written(array_of(1, num(cases[i].i)), TRANSIT_MSGPACK, &len);
        transit_value *back;
        hex[0] = '\0';
        for (k = 1; k < len; k++) snprintf(hex + 2 * (k - 1), 3, "%02x", out[k]);   /* after the array header */
        CHECK(strcmp(hex, cases[i].hex) == 0, "msgpack %lld: %s, expected %s", (long long)cases[i].i, hex, cases[i].hex);
        back = transit_read(doc, out, len, TRANSIT_MSGPACK, NULL);
        CHECK(back && back->u.coll.items[0]->u.integer == cases[i].i, "msgpack %lld reads back", (long long)cases[i].i);
        free(out);
    }
    /* uint64 beyond int64, float32 and bin, which transit writers don't use */
    v = transit_read(doc, u64, sizeof(u64), TRANSIT_MSGPACK, NULL);
    CHECK(v && v->type == TRANSIT_BIGINT && v->u.str.len == 20 && !memcmp(v->u.str.data, "18446744073709551615", 20), "uint64");
    v = transit_read(doc, f32, sizeof(f32), TRANSIT_MSGPACK, NULL);
    CHECK(v && v->type == TRANSIT_FLOAT && v->u.number == 1.5, "float32");
    v = transit_read(doc, bin, sizeof(bin), TRANSIT_MSGPACK, NULL);
    CHECK(v && v->u.coll.items[0]->type == TRANSIT_BYTES && v->u.coll.items[0]->u.str.len == 2, "bin");
}

/* Strings, times and other types */

static void test_values(void) {
    static const char nul[] = {'a', '\0', 'b'};
    transit_value *v;
    CHECK_JSON(array_of(5, str("~a"), str("^a"), str("`a"), str("^ "), str("#a")), "[\"~~a\",\"~^a\",\"~`a\",\"~^ \",\"#a\"]");
    CHECK(transit_equal(read_json("[\"~~a\",\"~^a\",\"~`a\",\"~^ \"]"), array_of(4, str("~a"), str("^a"), str("`a"), str("^ "))), "unescaping");
    CHECK_JSON(array_of(1, str("a\"b\\c\n\x01")), "[\"a\\\"b\\\\c\\n\\u0001\"]");
    v = read_json("[\"\\u00e9\\ud83d\\ude00\\/\"]");
    CHECK(v && transit_equal(v, array_of(1, str("\xc3\xa9\xf0\x9f\x98\x80/"))), "unicode escapes");
    v = array_of(1, transit_string(doc, nul, 3));
    CHECK(transit_equal(roundtrip(v, TRANSIT_JSON), v) && transit_equal(roundtrip(v, TRANSIT_MSGPACK), v), "NUL in strings");
    CHECK_JSON(transit_bytes(doc, "\x00\x01\xff", 3), "[\"~#'\",\"~bAAH/\"]");
    CHECK_JSON(transit_time(doc, 946728000123LL), "[\"~#'\",\"~m946728000123\"]");
    CHECK_VERBOSE(transit_time(doc, 946728000123LL), "{\"~#'\":\"~t2000-01-01T12:00:00.123Z\"}");
    CHECK_VERBOSE(transit_time(doc, -6106017600000LL), "{\"~#'\":\"~t1776-07-04T12:00:00.000Z\"}");
    CHECK(transit_equal(read_json("[\"~t2000-01-01T12:00:00.000Z\",\"~t2000-01-01T12:00:00Z\",\"~t2000-01-01T07:00:00-05:00\","
                                  "\"~t2000-01-01T13:30:00.123456+01:30\",\"~m946728000000\"]"),
                        array_of(5, transit_time(doc, 946728000000LL), transit_time(doc, 946728000000LL),
                                 transit_time(doc, 946728000000LL), transit_time(doc, 946728000123LL),
                                 transit_time(doc, 946728000000LL))), "times");
    CHECK_JSON(array_of(3, flt(NAN), flt(INFINITY), flt(-INFINITY)), "[\"~zNaN\",\"~zINF\",\"~z-INF\"]");
    CHECK_JSON(map_of(6, transit_nil(doc), num(1), transit_bool(doc, 1), num(2), num(1), num(3), flt(1.5), num(4),
                      kw("k"), num(5), uuid("5a2cbea3-e8c6-428b-b525-21239370dd55"), num(6)),
               "[\"^ \",\"~_\",1,\"~?t\",2,\"~i1\",3,\"~d1.5\",4,\"~:k\",5,\"~u5a2cbea3-e8c6-428b-b525-21239370dd55\",6]");
    CHECK_JSON(map_of(2, array_of(1, num(1)), str("a"), as(TRANSIT_SET, array_of(1, num(1))), str("b")),
               "[\"~#cmap\",[[1],\"a\",[\"~#set\",[1]],\"b\"]]");
    CHECK_JSON(transit_tagged(doc, S("point"), array_of(2, num(1), num(2))), "[\"~#point\",[1,2]]");
    CHECK_VERBOSE(transit_tagged(doc, S("point"), array_of(2, num(1), num(2))), "{\"~#point\":[1,2]}");
    v = read_json("[\"~xfoo\",\"~ca\",[\"~#ratio\",[\"~n1\",\"~n3\"]],[\"~#link\",[\"^ \",\"href\",\"~rhttp://x\",\"rel\",\"self\"]]]");
    CHECK(v && v->u.coll.items[0]->type == TRANSIT_TAGGED && v->u.coll.items[3]->type == TRANSIT_TAGGED, "unknown tags");
    CHECK_JSON(v, "[\"~xfoo\",\"~ca\",[\"~#ratio\",[\"~n1\",\"~n3\"]],[\"~#link\",[\"^ \",\"href\",\"~rhttp://x\",\"rel\",\"self\"]]]");
    /* map lookup and equality */
    v = map_of(3, str("a"), num(1), kw("b"), num(2), array_of(1, num(1)), num(3));
    CHECK(transit_map_get(v, kw("b"))->u.integer == 2 && transit_map_get(v, array_of(1, num(1)))->u.integer == 3 &&
          transit_map_get(v, str("b")) == NULL, "map lookup");
    CHECK(transit_equal(map_of(2, str("a"), num(1), str("b"), num(2)), map_of(2, str("b"), num(2), str("a"), num(1))), "maps ignore order");
    CHECK(!transit_equal(num(1), flt(1)), "1 isn't 1.0");
    CHECK(transit_equal(flt(NAN), flt(NAN)), "NaN equals NaN");
}

/* Streams */

typedef struct {
    const unsigned char *p, *end;
} mem_input;

static int mem_getc(void *ctx) {
    mem_input *in = (mem_input *)ctx;
    return in->p < in->end ? *in->p++ : -1;
}

static int read_stream(const void *data, size_t len, transit_format format, transit_value **values, int max, transit_error *err) {
    mem_input in;
    transit_stream *s;
    int n = 0;
    in.p = (const unsigned char *)data;
    in.end = in.p + len;
    s = transit_stream_new(format, mem_getc, &in);
    while (n < max && (values[n] = transit_stream_read(s, doc, err)) != NULL) n++;
    transit_stream_free(s);
    return n;
}

static void test_streams(void) {
    transit_value *values[8];
    transit_value *expected = array_of(6, num(1), str("~tilde"), array_of(2, kw("abcd"), kw("abcd")),
                                       map_of(1, kw("abcd"), str("quote \" and backslash \\ \xc3\xa9")),
                                       as(TRANSIT_SET, array_of(2, num(1), num(2))), transit_nil(doc));
    transit_format formats[3];
    transit_error err;
    size_t f, i;
    formats[0] = TRANSIT_JSON;
    formats[1] = TRANSIT_JSON_VERBOSE;
    formats[2] = TRANSIT_MSGPACK;
    for (f = 0; f < 3; f++) {
        transit_buffer all = {0};
        int n;
        for (i = 0; i < expected->u.coll.count; i++) transit_write(expected->u.coll.items[i], formats[f], &all, NULL);
        n = read_stream(all.data, all.len, formats[f], values, 8, &err);
        CHECK(n == 6 && err.code == TRANSIT_OK, "format %d: read %d values", (int)f, n);
        for (i = 0; i < (size_t)n && i < 6; i++)
            CHECK(transit_equal(values[i], expected->u.coll.items[i]), "format %d: value %d", (int)f, (int)i);
        transit_buffer_free(&all);
    }
    CHECK(read_stream(S(" [\"~#'\",1]\n\t{\"~#'\":2}  \n"), TRANSIT_JSON, values, 8, &err) == 2 && err.code == TRANSIT_OK &&
          values[0]->u.integer == 1 && values[1]->u.integer == 2, "whitespace between values");
    CHECK(read_stream("", 0, TRANSIT_JSON, values, 8, &err) == 0 && err.code == TRANSIT_OK, "empty json stream");
    CHECK(read_stream("", 0, TRANSIT_MSGPACK, values, 8, &err) == 0 && err.code == TRANSIT_OK, "empty msgpack stream");
    CHECK(read_stream(S("[1,\"a]"), TRANSIT_JSON, values, 8, &err) == 0 && err.code == TRANSIT_ERROR_TRUNCATED, "truncated json");
    CHECK(read_stream("\x92\x01", 2, TRANSIT_MSGPACK, values, 8, &err) == 0 && err.code == TRANSIT_ERROR_TRUNCATED, "truncated msgpack");
}

static void test_errors(void) {
    static const unsigned char ext[] = {0xd4, 0x01, 0x00};
    transit_error err;
    CHECK(!transit_read(doc, S("[\"^0\"]"), TRANSIT_JSON, &err) && err.code == TRANSIT_ERROR_TRANSIT, "unknown cache code");
    CHECK(!transit_read(doc, S("[1,]"), TRANSIT_JSON, &err) && err.code == TRANSIT_ERROR_SYNTAX, "bad JSON");
    CHECK(!transit_read(doc, S("[1] 2"), TRANSIT_JSON, &err) && err.code == TRANSIT_ERROR_SYNTAX, "trailing data");
    CHECK(!transit_read(doc, S("[1"), TRANSIT_JSON, &err) && err.code == TRANSIT_ERROR_TRUNCATED, "truncated");
    CHECK(!transit_read(doc, ext, sizeof(ext), TRANSIT_MSGPACK, &err) && err.code == TRANSIT_ERROR_SYNTAX, "msgpack ext");
    CHECK(!transit_read(doc, S("[1,\"~#tag\"]"), TRANSIT_JSON, &err) && err.code == TRANSIT_ERROR_TRANSIT, "stray tag");
    CHECK(!transit_read(doc, S("[\"~#'\",\"~unot-a-uuid\"]"), TRANSIT_JSON, &err) && err.code == TRANSIT_ERROR_TRANSIT, "bad uuid");
    CHECK(!transit_read(doc, S("[\"\x01\"]"), TRANSIT_JSON, &err) && err.code == TRANSIT_ERROR_SYNTAX, "control character");
}

/* Output doesn't depend on the C locale's decimal point. */
static void test_locale(void) {
    static const char *locales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "de_DE", "German"};
    const char *found = NULL;
    transit_value *v;
    size_t i;
    for (i = 0; i < sizeof(locales) / sizeof(locales[0]) && !found; i++)
        if (setlocale(LC_NUMERIC, locales[i]) && localeconv()->decimal_point[0] == ',') found = locales[i];
    if (!found) {
        setlocale(LC_NUMERIC, "C");
        printf("SKIP locale: no locale with a decimal comma available\n");
        return;
    }
    CHECK_JSON(array_of(2, flt(1.5), flt(0.1)), "[1.5,0.1]");
    v = read_json("[1.5,\"~d2.25\"]");
    CHECK(v && v->u.coll.items[0]->u.number == 1.5 && v->u.coll.items[1]->u.number == 2.25, "reading in %s", found);
    setlocale(LC_NUMERIC, "C");
    printf("locale: checked in %s\n", found);
}

/* Corrupted and truncated input is rejected (or read) without crashing:
 * random mutations of every exemplar, in every format. Run the tests under
 * sanitizers to make this meaningful. */
static void test_mutations(void) {
    static const char *exts[] = {".json", ".verbose.json", ".mp"};
    size_t e, i, len, rejected = 0, total = 0;
    int k, round;
    for (i = 0; i < N_EXEMPLARS; i++)
        for (e = 0; e < 3; e++) {
            char *data = exemplar_file(EXEMPLARS[i], exts[e], &len);
            transit_format format = e == 0 ? TRANSIT_JSON : e == 1 ? TRANSIT_JSON_VERBOSE : TRANSIT_MSGPACK;
            if (!data || !len) {
                free(data);
                continue;
            }
            for (round = 0; round < 40; round++) {
                char *copy = (char *)malloc(len);
                size_t n = len;
                transit_doc *scratch = transit_doc_new();
                transit_error err;
                transit_value *v;
                memcpy(copy, data, len);
                if (round % 4 == 0) n = (size_t)(next_random() % len);   /* truncate */
                for (k = 0; k < 1 + round % 3; k++) copy[next_random() % len] = (char)(next_random() & 0xff);
                v = transit_read(scratch, copy, n, format, &err);
                if (!v) {
                    rejected++;
                    CHECK(err.code != TRANSIT_OK && err.message[0], "a rejected read says why");
                } else {
                    /* whatever was read can be written */
                    transit_buffer out = {0};
                    CHECK(transit_write(v, format, &out, NULL) == 0, "writing what was read");
                    transit_buffer_free(&out);
                }
                total++;
                transit_doc_free(scratch);
                free(copy);
            }
            free(data);
        }
    printf("mutations: %d inputs, %d rejected\n", (int)total, (int)rejected);
}

int main(void) {
    doc = transit_doc_new();
    test_exemplars();
    test_cache();
    test_numbers();
    test_msgpack_ints();
    test_values();
    test_streams();
    test_errors();
    test_locale();
    test_mutations();
    transit_doc_free(doc);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
