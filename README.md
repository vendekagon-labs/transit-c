# transit-format-c

A C library that reads and writes [Transit](https://github.com/cognitect/transit-format)
in all three of its encodings: JSON, JSON-verbose and MessagePack.

* C99, with no dependencies beyond the C standard library
* Built with CMake (3.15 or later)
* Passes transit-format's verify harness in all three encodings, including
  generated data
* Writes msgpack byte for byte as transit-java does (every transit-format
  msgpack exemplar re-encodes identically), and assigns cache codes as
  transit-clj does, including when the cache fills up and starts over
* Numbers are exact: integers are told apart from floats by how they're
  written, 64 bit integers are kept whole, doubles read back to the same
  bits, and output doesn't depend on the C locale's decimal point
* Streaming: reads each value of a sequence as it arrives, from a pipe or
  socket, never reading past the end of the value

It's intended as a native core for the Python, R and Julia transit
libraries, and for anything else that wants transit from C.

This implementation's major.minor version number corresponds to the version
of the Transit specification it supports.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

This builds `libtransit`, the `transit-roundtrip` tool and the tests. The
exemplar tests read the example files from a checkout of
[transit-format](https://github.com/cognitect/transit-format) next to this
one (or pass `-DTRANSIT_FORMAT_DIR=...`). `cmake --install build` installs the
library, `transit.h` and a CMake package, so other CMake projects can
`find_package(transit)` and link `transit::transit`.

To use it from another CMake project straight from GitHub, with
[FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html):

```cmake
include(FetchContent)
FetchContent_Declare(transit
  GIT_REPOSITORY https://github.com/vendekagon-labs/transit-c.git
  GIT_TAG main)   # or a tag or commit
FetchContent_MakeAvailable(transit)

target_link_libraries(your_target PRIVATE transit::transit)
```

(Use `git@github.com:vendekagon-labs/transit-c.git` to fetch over SSH.) As a
subproject it builds just the library; the tests and `transit-roundtrip` are
built only when it's the top level project.

The library is a handful of C files and one header, so it can also simply be
compiled into another project's build (as an R package would, from `src/`).

## Usage

```c
#include <stdio.h>
#include <string.h>
#include "transit.h"

int main(void) {
    const char *text = "[\"^ \",\"~:name\",\"transit\",\"~:tags\",[\"~#set\",[\"c\",\"json\"]]]";
    transit_doc *doc = transit_doc_new();   /* owns everything read or built */
    transit_error err;
    transit_buffer out = {0};
    transit_value *v, *key, *copy;

    /* read */
    v = transit_read(doc, text, strlen(text), TRANSIT_JSON, &err);
    if (!v) {
        fprintf(stderr, "%s\n", err.message);
        return 1;
    }
    key = transit_keyword(doc, "tags", 4);
    printf("%d tags\n", (int)transit_map_get(v, key)->u.coll.count);

    /* build and write */
    copy = transit_map(doc);
    transit_map_put(doc, copy, transit_keyword(doc, "count", 5), transit_int(doc, 2));
    transit_write(copy, TRANSIT_MSGPACK, &out, &err);
    printf("%d bytes of msgpack\n", (int)out.len);

    transit_buffer_free(&out);
    transit_doc_free(doc);
    return 0;
}
```

To read a sequence of values as they arrive, use a stream. Either give it
data in chunks as you receive it:

```c
transit_stream *s = transit_stream_new(TRANSIT_JSON, NULL, NULL);
/* for each chunk read from a socket, file or pipe: */
transit_stream_feed(s, chunk, chunk_len);
while ((v = transit_stream_read(s, doc, &err)) != NULL) {
    /* use v */
}
/* NULL with err.code == TRANSIT_OK: no complete value yet, feed more */
/* at the end of the input: */
transit_stream_end(s);
/* then read any last values; a value cut off part way is an error */
```

or create it with a function that returns the next byte (or -1 at the end of
the input), in which case input is read only as far as the end of each value;
see `tools/transit_roundtrip.c`.

## Types

| Transit type | `transit_type` | Representation |
|:-------------|:---------------|:---------------|
| null | `TRANSIT_NIL` | |
| boolean | `TRANSIT_BOOL` | `u.boolean` |
| integer | `TRANSIT_INT` | `u.integer` (`int64_t`) |
| arbitrary precision integer | `TRANSIT_BIGINT` | `u.str`, decimal digits |
| decimal (float), special numbers | `TRANSIT_FLOAT` | `u.number` (`double`) |
| arbitrary precision decimal | `TRANSIT_DECIMAL` | `u.str`, as written |
| string | `TRANSIT_STRING` | `u.str`, UTF-8 (may contain NUL) |
| keyword | `TRANSIT_KEYWORD` | `u.str`, the name |
| symbol | `TRANSIT_SYMBOL` | `u.str` |
| bytes | `TRANSIT_BYTES` | `u.str`, the bytes |
| time | `TRANSIT_TIME` | `u.integer`, milliseconds since the epoch |
| uuid | `TRANSIT_UUID` | `u.uuid`, 16 bytes |
| uri | `TRANSIT_URI` | `u.str` |
| array | `TRANSIT_ARRAY` | `u.coll` |
| list | `TRANSIT_LIST` | `u.coll` |
| set | `TRANSIT_SET` | `u.coll` |
| map | `TRANSIT_MAP` | `u.map`: keys of any type, in order |
| char, ratio, link and unknown tags | `TRANSIT_TAGGED` | `u.tagged`: tag and representation, written back out unchanged |

Strings in values are not NUL terminated; use their lengths.

## Testing

`ctest` runs the test suite: every transit-format exemplar in all three
encodings (checked against each other, against independently built values,
and against transit-java's msgpack byte for byte), the cache's rollover
against transit-clj's output, 100,000 random doubles, streaming, errors, a
non-C locale, and thousands of corrupted inputs. It's worth running under
sanitizers too:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"
cmake --build build-asan && ctest --test-dir build-asan
```

transit-format's verify harness drives `bin/roundtrip`, which builds
`transit-roundtrip` the first time it runs.

## Contributing

transit-c is maintained by [Vendekagon Labs](https://github.com/vendekagon-labs).
Report problems and suggest changes with GitHub
[issues](https://github.com/vendekagon-labs/transit-c/issues).

Pull requests are welcome, but make sure they're well tested first: add tests
for what you change, and check that the tests pass (`ctest`, also in a
sanitizer build, as above) and that transit-format's verify harness
(`bin/verify`, which needs the
[clojure CLI](https://clojure.org/guides/install_clojure)) passes.

## Copyright and License

Copyright © 2026 Vendekagon Labs LLC

Based on the Java, Python, Julia and R implementations: Copyright © 2014
Cognitect, Copyright © 2016 Russ Olsen, Ben Kamphaus.

Licensed under the Apache License, Version 2.0 (the "License"); you may not
use this file except in compliance with the License. You may obtain a copy of
the License at http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
License for the specific language governing permissions and limitations under
the License.
