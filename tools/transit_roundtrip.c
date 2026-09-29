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

/* Reads each transit value from stdin as it arrives and writes it back to
 * stdout. Used by transit-format's verify harness (via bin/roundtrip).
 *
 * usage: transit-roundtrip [json|json-verbose|msgpack] */

#include "transit.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int read_stdin(void *ctx) {
    int c = getc((FILE *)ctx);
    return c == EOF ? -1 : c;
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "json";
    transit_format format;
    transit_stream *stream;
    transit_buffer out = {0};
    int status = 0;

    if (strcmp(name, "json") == 0) format = TRANSIT_JSON;
    else if (strcmp(name, "json-verbose") == 0 || strcmp(name, "json_verbose") == 0) format = TRANSIT_JSON_VERBOSE;
    else if (strcmp(name, "msgpack") == 0) format = TRANSIT_MSGPACK;
    else {
        fprintf(stderr, "usage: %s [json|json-verbose|msgpack]\n", argv[0]);
        return 2;
    }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    stream = transit_stream_new(format, read_stdin, stdin);
    if (!stream) return 1;
    for (;;) {
        transit_error err;
        transit_doc *doc = transit_doc_new();
        transit_value *value;
        if (!doc) {
            status = 1;
            break;
        }
        value = transit_stream_read(stream, doc, &err);
        if (!value) {
            if (err.code != TRANSIT_OK) {
                fprintf(stderr, "read error: %s\n", err.message);
                status = 1;
            }
            transit_doc_free(doc);
            break;
        }
        out.len = 0;
        if (transit_write(value, format, &out, &err) != 0) {
            fprintf(stderr, "write error: %s\n", err.message);
            status = 1;
            transit_doc_free(doc);
            break;
        }
        fwrite(out.data, 1, out.len, stdout);
        fflush(stdout);
        transit_doc_free(doc);
    }
    transit_buffer_free(&out);
    transit_stream_free(stream);
    return status;
}
