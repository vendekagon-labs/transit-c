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

#ifndef TRANSIT_INTERNAL_H
#define TRANSIT_INTERNAL_H

#include "transit.h"

#include <stdarg.h>

/* Arena allocation (transit_value.c). */
void *doc_alloc(transit_doc *doc, size_t size);
const char *doc_copy(transit_doc *doc, const char *s, size_t len);
transit_value *doc_value(transit_doc *doc, transit_type type);
transit_value *doc_str_value(transit_doc *doc, transit_type type, const char *s, size_t len);

/* Output buffers (transit_util.c). */
int buf_put(transit_buffer *b, const void *data, size_t len);
int buf_byte(transit_buffer *b, unsigned char c);
int buf_str(transit_buffer *b, const char *s);

/* Errors. */
void set_error(transit_error *err, transit_error_code code, size_t offset, const char *fmt, ...);
void clear_error(transit_error *err);

/* Numbers, independent of the C locale. */
#define DOUBLE_TEXT_MAX 40
/* Shortest text that reads back as d exactly; always has a '.' or exponent. */
size_t format_double(double d, char out[DOUBLE_TEXT_MAX]);
int parse_double(const char *s, size_t len, double *out);
/* 0 if s (an optional '-' then digits) fits in 64 bits, 1 if it doesn't, -1 if not an integer. */
int parse_int64(const char *s, size_t len, int64_t *out);
int is_integer_text(const char *s, size_t len);

/* Times. */
int parse_rfc3339(const char *s, size_t len, int64_t *ms);
size_t format_rfc3339(int64_t ms, char out[48]);

/* Base64. */
int base64_encode(transit_buffer *out, const unsigned char *data, size_t len);
/* Decodes into newly allocated doc memory. Returns 0, or -1 if invalid. */
int base64_decode(transit_doc *doc, const char *s, size_t len, const unsigned char **out, size_t *out_len);

/* UUIDs. */
int parse_uuid(const char *s, size_t len, unsigned char out[16]);
void format_uuid(const unsigned char uuid[16], char out[37]);

/* The transit cache. Readers and writers must assign codes the same way,
 * so this follows the spec (and transit-java): codes are assigned in order
 * from "^0", and once CACHE_SIZE entries are in use the cache starts over. */
#define CACHE_CODE_DIGITS 44
#define CACHE_SIZE (CACHE_CODE_DIGITS * CACHE_CODE_DIGITS)
#define MIN_SIZE_CACHEABLE 4
#define CACHE_TABLE_SIZE 4096   /* a power of two over twice CACHE_SIZE */

typedef struct {
    const char *data;
    size_t len;
} str_ref;

typedef struct {
    str_ref entries[CACHE_SIZE];
    int index;
    /* for writing: open addressed table of entry indexes + 1 (0 is empty) */
    int *table;
} transit_cache;

int cache_init(transit_cache *c, int for_writing);
void cache_free(transit_cache *c);
int is_cacheable(const char *s, size_t len, int as_key);
int is_cache_key(const char *s, size_t len);
/* Reading: remember a cacheable string (which must outlive the cache). */
void cache_add(transit_cache *c, const char *s, size_t len);
/* Reading: the string for a code, or 0 if unknown. */
int cache_lookup(const transit_cache *c, const char *code, size_t len, str_ref *out);
/* Writing: writes the code for s into code (returning its length) if s is
 * cached, otherwise remembers s (if cacheable) and returns 0. */
size_t cache_write(transit_cache *c, const char *s, size_t len, int as_key, char code[4]);

/* Parsing JSON and msgpack into "wire" values: nil, bool, int, bigint
 * (integers beyond 64 bits), float, string, bytes (msgpack bin), array and
 * map. Decoding then turns wire values into transit values. */
transit_value *json_parse_wire(transit_doc *doc, const char *data, size_t len, transit_error *err);

typedef struct {
    const unsigned char *p, *end;   /* buffered input */
    transit_getc_fn getc;           /* then more from here, if set */
    void *ctx;
    size_t offset;
} byte_source;

transit_value *msgpack_parse_wire(transit_doc *doc, byte_source *src, transit_error *err);

transit_value *decode_wire(transit_doc *doc, transit_value *wire, transit_error *err);

#endif
