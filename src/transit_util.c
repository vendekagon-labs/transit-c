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

/* Buffers, errors, numbers, times, base64, uuids and the cache. */

#include "transit_internal.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Buffers */

void transit_buffer_free(transit_buffer *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = buf->cap = 0;
}

int buf_put(transit_buffer *b, const void *data, size_t len) {
    if (b->cap - b->len < len) {
        size_t cap = b->cap ? b->cap : 256;
        unsigned char *p;
        while (cap - b->len < len) cap *= 2;
        p = (unsigned char *)realloc(b->data, cap);
        if (!p) return -1;
        b->data = p;
        b->cap = cap;
    }
    if (len) memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

int buf_byte(transit_buffer *b, unsigned char c) {
    if (b->len < b->cap) {
        b->data[b->len++] = c;
        return 0;
    }
    return buf_put(b, &c, 1);
}

int buf_str(transit_buffer *b, const char *s) { return buf_put(b, s, strlen(s)); }

/* Errors */

void set_error(transit_error *err, transit_error_code code, size_t offset, const char *fmt, ...) {
    va_list ap;
    if (!err || err->code != TRANSIT_OK) return;   /* keep the first error */
    err->code = code;
    err->offset = offset;
    va_start(ap, fmt);
    vsnprintf(err->message, sizeof(err->message), fmt, ap);
    va_end(ap);
}

void clear_error(transit_error *err) {
    if (!err) return;
    err->code = TRANSIT_OK;
    err->offset = 0;
    err->message[0] = '\0';
}

/* Numbers. strtod and printf are exact on current C libraries, but use the
 * locale's decimal point, so text is translated to and from it. */

static char decimal_point(void) {
    const struct lconv *lc = localeconv();
    return (lc && lc->decimal_point && lc->decimal_point[0]) ? lc->decimal_point[0] : '.';
}

int parse_double(const char *s, size_t len, double *out) {
    char small[64];
    char *text = len < sizeof(small) ? small : (char *)malloc(len + 1);
    char point = decimal_point();
    char *end;
    size_t i;
    int ok;
    if (!text) return -1;
    memcpy(text, s, len);
    text[len] = '\0';
    if (point != '.')
        for (i = 0; i < len; i++) if (text[i] == '.') text[i] = point;
    *out = strtod(text, &end);
    ok = len > 0 && end == text + len;
    if (text != small) free(text);
    return ok ? 0 : -1;
}

size_t format_double(double d, char out[DOUBLE_TEXT_MAX]) {
    char point = decimal_point();
    int precision;
    size_t n = 0, i;
    int has_point = 0;
    for (precision = 15; precision <= 17; precision++) {
        double back;
        n = (size_t)snprintf(out, DOUBLE_TEXT_MAX, "%.*g", precision, d);
        for (i = 0; i < n; i++) if (out[i] == point) out[i] = '.';
        if (parse_double(out, n, &back) == 0 && back == d) break;
    }
    for (i = 0; i < n; i++) if (out[i] == '.' || out[i] == 'e') has_point = 1;
    if (!has_point) {
        out[n++] = '.';
        out[n++] = '0';
        out[n] = '\0';
    }
    return n;
}

int is_integer_text(const char *s, size_t len) {
    size_t i = 0;
    if (len && s[0] == '-') i = 1;
    if (i == len) return 0;
    for (; i < len; i++) if (s[i] < '0' || s[i] > '9') return 0;
    return 1;
}

int parse_int64(const char *s, size_t len, int64_t *out) {
    uint64_t v = 0, limit;
    int neg = len && s[0] == '-';
    size_t i = neg ? 1 : 0;
    if (!is_integer_text(s, len)) return -1;
    limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    for (; i < len; i++) {
        unsigned d = (unsigned)(s[i] - '0');
        if (v > (limit - d) / 10) return 1;
        v = v * 10 + d;
    }
    if (neg) *out = v == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)v;
    else *out = (int64_t)v;
    return 0;
}

/* Times, as milliseconds since the epoch, using the proleptic Gregorian
 * calendar (days_from_civil and civil_from_days are from
 * http://howardhinnant.github.io/date_algorithms.html). */

static int64_t days_from_civil(int64_t y, int m, int d) {
    int64_t era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d) {
    int64_t era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2);
}

static int digits(const char *s, size_t len, size_t at, size_t n, int *out) {
    size_t i;
    int v = 0;
    if (at + n > len) return -1;
    for (i = at; i < at + n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    *out = v;
    return 0;
}

/* RFC 3339: YYYY-MM-DDTHH:MM:SS[.fraction][Z|+HH:MM|-HH:MM]. A missing
 * offset is taken to be UTC; precision beyond milliseconds is dropped. */
int parse_rfc3339(const char *s, size_t len, int64_t *ms) {
    int y, mo, d, h, mi, sec, oh = 0, om = 0, frac = 0, scale = 100;
    size_t i = 19;
    int64_t total;
    if (len < 19 || digits(s, len, 0, 4, &y) || s[4] != '-' || digits(s, len, 5, 2, &mo) || s[7] != '-' ||
        digits(s, len, 8, 2, &d) || (s[10] != 'T' && s[10] != 't' && s[10] != ' ') ||
        digits(s, len, 11, 2, &h) || s[13] != ':' || digits(s, len, 14, 2, &mi) || s[16] != ':' ||
        digits(s, len, 17, 2, &sec))
        return -1;
    if (i < len && s[i] == '.') {
        i++;
        if (i >= len || s[i] < '0' || s[i] > '9') return -1;
        for (; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
            frac += (s[i] - '0') * scale;
            scale /= 10;
        }
    }
    total = ((days_from_civil(y, mo, d) * 24 + h) * 60 + mi) * 60 + sec;
    total = total * 1000 + frac;
    if (i < len && (s[i] == 'Z' || s[i] == 'z')) {
        i++;
    } else if (i < len && (s[i] == '+' || s[i] == '-')) {
        int sign = s[i] == '-' ? -1 : 1;
        if (digits(s, len, i + 1, 2, &oh) || i + 3 >= len || s[i + 3] != ':' || digits(s, len, i + 4, 2, &om)) return -1;
        total -= (int64_t)sign * (oh * 60 + om) * 60000;
        i += 6;
    }
    if (i != len) return -1;
    *ms = total;
    return 0;
}

size_t format_rfc3339(int64_t ms, char out[48]) {
    int64_t secs = ms / 1000, frac = ms % 1000, days, rem, y;
    int m, d;
    if (frac < 0) {
        frac += 1000;
        secs -= 1;
    }
    days = secs / 86400;
    rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    civil_from_days(days, &y, &m, &d);
    return (size_t)snprintf(out, 48, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", (long long)y, m, d,
                            (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60), (int)frac);
}

/* Base64 */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_encode(transit_buffer *out, const unsigned char *data, size_t len) {
    size_t i;
    for (i = 0; i + 2 < len; i += 3) {
        unsigned long n = ((unsigned long)data[i] << 16) | ((unsigned long)data[i + 1] << 8) | data[i + 2];
        char q[4] = {B64[(n >> 18) & 63], B64[(n >> 12) & 63], B64[(n >> 6) & 63], B64[n & 63]};
        if (buf_put(out, q, 4)) return -1;
    }
    if (i < len) {
        unsigned long n = (unsigned long)data[i] << 16;
        char q[4];
        if (i + 1 < len) n |= (unsigned long)data[i + 1] << 8;
        q[0] = B64[(n >> 18) & 63];
        q[1] = B64[(n >> 12) & 63];
        q[2] = i + 1 < len ? B64[(n >> 6) & 63] : '=';
        q[3] = '=';
        if (buf_put(out, q, 4)) return -1;
    }
    return 0;
}

static int b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int base64_decode(transit_doc *doc, const char *s, size_t len, const unsigned char **out, size_t *out_len) {
    unsigned char *p = (unsigned char *)doc_alloc(doc, len / 4 * 3 + 3);
    size_t i, n = 0;
    unsigned long acc = 0;
    int bits = 0;
    if (!p) return -1;
    for (i = 0; i < len; i++) {
        int v;
        char c = s[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        v = b64_value(c);
        if (v < 0) return -1;
        acc = (acc << 6) | (unsigned long)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            p[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    *out = p;
    *out_len = n;
    return 0;
}

/* UUIDs */

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int parse_uuid(const char *s, size_t len, unsigned char out[16]) {
    size_t i, n = 0;
    if (len != 36) return -1;
    for (i = 0; i < len; i++) {
        int hi, lo;
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return -1;
            continue;
        }
        hi = hex_value(s[i]);
        lo = i + 1 < len ? hex_value(s[i + 1]) : -1;
        if (hi < 0 || lo < 0) return -1;
        out[n++] = (unsigned char)(hi * 16 + lo);
        i++;
    }
    return n == 16 ? 0 : -1;
}

void format_uuid(const unsigned char uuid[16], char out[37]) {
    static const char hex[] = "0123456789abcdef";
    size_t i, n = 0;
    for (i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out[n++] = '-';
        out[n++] = hex[uuid[i] >> 4];
        out[n++] = hex[uuid[i] & 15];
    }
    out[n] = '\0';
}

/* The cache */

int cache_init(transit_cache *c, int for_writing) {
    c->index = 0;
    c->table = NULL;
    if (for_writing) {
        c->table = (int *)calloc(CACHE_TABLE_SIZE, sizeof(int));
        if (!c->table) return -1;
    }
    return 0;
}

void cache_free(transit_cache *c) {
    free(c->table);
    c->table = NULL;
}

int is_cacheable(const char *s, size_t len, int as_key) {
    return len >= MIN_SIZE_CACHEABLE &&
           (as_key || (s[0] == '~' && (s[1] == '#' || s[1] == '$' || s[1] == ':')));
}

int is_cache_key(const char *s, size_t len) {
    return len > 0 && s[0] == '^' && !(len == 2 && s[1] == ' ');
}

static size_t encode_code(int i, char code[4]) {
    int hi = i / CACHE_CODE_DIGITS, lo = i % CACHE_CODE_DIGITS;
    code[0] = '^';
    if (hi == 0) {
        code[1] = (char)(lo + 48);
        return 2;
    }
    code[1] = (char)(hi + 48);
    code[2] = (char)(lo + 48);
    return 3;
}

static uint32_t hash(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < len; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h;
}

static void cache_clear(transit_cache *c) {
    c->index = 0;
    if (c->table) memset(c->table, 0, CACHE_TABLE_SIZE * sizeof(int));
}

static void cache_store(transit_cache *c, const char *s, size_t len) {
    if (c->index >= CACHE_SIZE) cache_clear(c);
    c->entries[c->index].data = s;
    c->entries[c->index].len = len;
    if (c->table) {
        uint32_t slot = hash(s, len) & (CACHE_TABLE_SIZE - 1);
        while (c->table[slot]) slot = (slot + 1) & (CACHE_TABLE_SIZE - 1);
        c->table[slot] = c->index + 1;
    }
    c->index++;
}

void cache_add(transit_cache *c, const char *s, size_t len) { cache_store(c, s, len); }

int cache_lookup(const transit_cache *c, const char *code, size_t len, str_ref *out) {
    int i;
    if (len == 2) i = code[1] - 48;
    else if (len == 3) i = (code[1] - 48) * CACHE_CODE_DIGITS + (code[2] - 48);
    else return 0;
    if (i < 0 || i >= c->index) return 0;
    *out = c->entries[i];
    return 1;
}

size_t cache_write(transit_cache *c, const char *s, size_t len, int as_key, char code[4]) {
    uint32_t slot;
    if (!is_cacheable(s, len, as_key)) return 0;
    slot = hash(s, len) & (CACHE_TABLE_SIZE - 1);
    while (c->table[slot]) {
        const str_ref *e = &c->entries[c->table[slot] - 1];
        if (e->len == len && memcmp(e->data, s, len) == 0) return encode_code(c->table[slot] - 1, code);
        slot = (slot + 1) & (CACHE_TABLE_SIZE - 1);
    }
    cache_store(c, s, len);
    return 0;
}
