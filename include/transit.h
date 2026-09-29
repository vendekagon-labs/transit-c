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

/*
 * transit-format-c: reads and writes Transit (https://github.com/cognitect/transit-format)
 * in all three of its encodings: JSON, JSON-verbose and MessagePack.
 *
 * C99, no dependencies beyond the C standard library.
 *
 * Values are trees of transit_value, allocated in a transit_doc (an arena):
 * everything read into or built in a document is freed together with
 * transit_doc_free(). The fields of transit_value can be read directly; use
 * the constructors to build values.
 */

#ifndef TRANSIT_H
#define TRANSIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRANSIT_VERSION_MAJOR 0
#define TRANSIT_VERSION_MINOR 8
#define TRANSIT_VERSION_PATCH 0

typedef enum {
    TRANSIT_JSON,
    TRANSIT_JSON_VERBOSE,
    TRANSIT_MSGPACK
} transit_format;

typedef enum {
    TRANSIT_NIL,
    TRANSIT_BOOL,
    TRANSIT_INT,      /* u.integer: a signed 64 bit integer */
    TRANSIT_BIGINT,   /* u.str: arbitrary precision integer, in decimal */
    TRANSIT_FLOAT,    /* u.number: a double, including NaN and the infinities */
    TRANSIT_DECIMAL,  /* u.str: arbitrary precision decimal, as written */
    TRANSIT_STRING,   /* u.str: UTF-8 */
    TRANSIT_KEYWORD,  /* u.str: the name, without the leading ':' */
    TRANSIT_SYMBOL,   /* u.str */
    TRANSIT_BYTES,    /* u.str: the bytes */
    TRANSIT_TIME,     /* u.integer: milliseconds since 1970-01-01T00:00:00Z */
    TRANSIT_UUID,     /* u.uuid: the 16 bytes, most significant first */
    TRANSIT_URI,      /* u.str */
    TRANSIT_ARRAY,    /* u.coll */
    TRANSIT_LIST,     /* u.coll */
    TRANSIT_SET,      /* u.coll */
    TRANSIT_MAP,      /* u.map: keys can be any value; entries keep their order */
    TRANSIT_TAGGED    /* u.tagged: a value with a tag this library doesn't
                         interpret, such as a char ("c"), ratio or link,
                         written back out unchanged */
} transit_type;

typedef struct transit_value transit_value;

struct transit_value {
    transit_type type;
    union {
        int boolean;
        int64_t integer;
        double number;
        struct { const char *data; size_t len; } str;   /* not NUL terminated */
        unsigned char uuid[16];
        struct { transit_value **items; size_t count, cap; } coll;
        struct { transit_value **keys; transit_value **values; size_t count, cap; } map;
        struct { const char *tag; size_t tag_len; transit_value *rep; } tagged;
    } u;
};

/* An arena that owns values. */
typedef struct transit_doc transit_doc;

transit_doc *transit_doc_new(void);
void transit_doc_free(transit_doc *doc);

/* Errors. message is a NUL terminated description; offset is the position in
 * the input (in bytes) where reading failed, where that is known. */
typedef enum {
    TRANSIT_OK = 0,
    TRANSIT_ERROR_SYNTAX,       /* malformed JSON or msgpack */
    TRANSIT_ERROR_TRANSIT,      /* well formed, but not valid transit */
    TRANSIT_ERROR_TRUNCATED,    /* the input ended in the middle of a value */
    TRANSIT_ERROR_UNSUPPORTED,  /* a value that can't be written, such as a map as a map key */
    TRANSIT_ERROR_MEMORY
} transit_error_code;

typedef struct {
    transit_error_code code;
    size_t offset;
    char message[160];
} transit_error;

/* Constructors. Strings and bytes are copied into the document. All return
 * NULL only when out of memory. */
transit_value *transit_nil(transit_doc *doc);
transit_value *transit_bool(transit_doc *doc, int b);
transit_value *transit_int(transit_doc *doc, int64_t i);
transit_value *transit_float(transit_doc *doc, double d);
transit_value *transit_string(transit_doc *doc, const char *s, size_t len);
transit_value *transit_keyword(transit_doc *doc, const char *s, size_t len);
transit_value *transit_symbol(transit_doc *doc, const char *s, size_t len);
transit_value *transit_uri(transit_doc *doc, const char *s, size_t len);
transit_value *transit_bigint(transit_doc *doc, const char *digits, size_t len);
transit_value *transit_decimal(transit_doc *doc, const char *digits, size_t len);
transit_value *transit_bytes(transit_doc *doc, const void *data, size_t len);
transit_value *transit_time(transit_doc *doc, int64_t ms);
transit_value *transit_uuid(transit_doc *doc, const unsigned char bytes[16]);
transit_value *transit_array(transit_doc *doc);
transit_value *transit_list(transit_doc *doc);
transit_value *transit_set(transit_doc *doc);
transit_value *transit_map(transit_doc *doc);
transit_value *transit_tagged(transit_doc *doc, const char *tag, size_t tag_len, transit_value *rep);

/* Adds an item to an array, list or set; returns 0, or -1 when out of memory. */
int transit_push(transit_doc *doc, transit_value *coll, transit_value *item);
/* Adds an entry to a map (without checking for an existing key); returns 0 or -1. */
int transit_map_put(transit_doc *doc, transit_value *map, transit_value *key, transit_value *value);
/* The value for key in map, or NULL. */
transit_value *transit_map_get(const transit_value *map, const transit_value *key);

/* Whether two values are equal: maps and sets compare regardless of order,
 * and NaN equals NaN. */
int transit_equal(const transit_value *a, const transit_value *b);

/* Reading one value from memory. Returns NULL on error (see err). */
transit_value *transit_read(transit_doc *doc, const void *data, size_t len,
                            transit_format format, transit_error *err);

/* Reading a sequence of values as they arrive, such as from a pipe or
 * socket. A stream gets its input in one of two ways:
 *
 * - Pulling: getc returns the next byte (0-255), or -1 at the end of the
 *   input. Input is read only as far as the end of each value.
 * - Feeding: create the stream with getc NULL, and pass it data in chunks of
 *   any size, as it arrives, with transit_stream_feed; call
 *   transit_stream_end when there is no more. This suits callers that read
 *   in blocks, and language bindings.
 */
typedef int (*transit_getc_fn)(void *ctx);
typedef struct transit_stream transit_stream;

transit_stream *transit_stream_new(transit_format format, transit_getc_fn getc, void *ctx);

/* Feeding: adds data (which is copied). Returns 0, or -1 when out of memory. */
int transit_stream_feed(transit_stream *s, const void *data, size_t len);
/* Feeding: there's no more data. */
void transit_stream_end(transit_stream *s);

/* The next value, or NULL with err->code TRANSIT_OK when there isn't one:
 * at the end of the input, or, when feeding and transit_stream_end hasn't
 * been called, until more data has been fed. NULL with another code is an
 * error (including TRANSIT_ERROR_TRUNCATED if the input ends part way
 * through a value). */
transit_value *transit_stream_read(transit_stream *s, transit_doc *doc, transit_error *err);
void transit_stream_free(transit_stream *s);

/* Output buffers. Start with {0}; free with transit_buffer_free. */
typedef struct {
    unsigned char *data;
    size_t len, cap;
} transit_buffer;

void transit_buffer_free(transit_buffer *buf);

/* Appends value, as transit, to out. Returns 0, or -1 on error (see err). */
int transit_write(const transit_value *value, transit_format format,
                  transit_buffer *out, transit_error *err);

#ifdef __cplusplus
}
#endif

#endif
