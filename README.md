# cJSON — Lightweight JSON Parser with Arena Allocator

A lightweight, RFC 8259-compliant JSON parser written in C, built on top of a custom **arena (region) allocator**. Zero individual `free` calls during parsing — all memory is released in one `arena_destroy()` call.

## Features

- **`cjson_parse`** — recursive descent parser, strict mode (trailing comma rejected, etc.)
- **`cjson_display`** — pretty-print parsed JSON tree to stdout
- **`cjson_stringify`** — two-pass serialization back to JSON text
- **`arena_create`** — create a new arena instance for all allocations
- **`arena_malloc`** — fast pointer-bump allocation, no zero-initialization
- **`arena_calloc`** — allocation + zero-init
- **`arena_reset`** — O(1) bulk reclaim, invalidates old pointers but reuses blocks
- **`arena_destroy`** — free all blocks in one call, zero individual `free` during use
- **Full UTF-8 support** — surrogate pair (`\uD834\uDD1E`) → UTF-8 conversion
- **Escape sequence handling** — `\n`, `\t`, `\\`, `\/`, `\"`, `\uXXXX`
- **Strict error reporting** — 15 distinct error codes covering structural, semantic, and memory failures
- **Nesting depth limit** — `PARSE_NEST_LIMIT` (default 1000) prevents stack overflow
- **Multi-instance arena** — no global state, thread-safe by design
- **8-byte alignment** — safe for `double`, pointers, SIMD
- **Opaque types** — clean C API, internal details not exposed

## Build
bash
GCC / Clang / MinGW-w64
gcc -std=c11 -Wall -O2 -I. -o cjson_demo main.c cjson.c arena.c
With AddressSanitizer (debug)
gcc -std=c11 -Wall -g -fsanitize=address -I. -o cjson_demo main.c cjson.c arena.c

## Usage
c
include "cjson.h"
int main(void) {
const char* json_text = "{"name":"suixinz","age":25,"skills":["C","JSON"]}";

arena* arena_pool = arena_create();
if (!arena_pool) return 1;

cjson_t* root = NULL;
cjson_parse(json_text, arena_pool, &root);

cjson_display(root);

/* All memory released in one call */
arena_destroy(arena_pool);
return 0;
}

### Parse and Stringify Round-Trip
c
cjson_t* root = NULL;
cjson_parse(json_text, arena_pool, &root);
/* Serialize back to JSON string */
unsigned char* output = cjson_stringify(root, arena_pool);
printf("%s\n", output);
arena_destroy(arena_pool);

## API

| Function | Description |
|----------|-------------|
| **`arena_create()`** | Create a new arena instance |
| **`arena_malloc(arena_pool, size)`** | Allocate `size` bytes, uninitialized |
| **`arena_calloc(arena_pool, count, size)`** | Allocate `count * size` bytes, zero-initialized |
| **`arena_reset(arena_pool)`** | Reset all block offsets to 0, reuse memory |
| **`arena_destroy(arena_pool)`** | Free all blocks and the arena itself |
| **`cjson_parse(src, arena_pool, &root)`** | Parse JSON text into a `cjson_t` tree |
| **`cjson_display(root)`** | Pretty-print the parsed tree to stdout |
| **`cjson_stringify(root, arena_pool)`** | Serialize tree back to JSON string |

## Error Codes

| Code | Meaning |
|------|---------|
| **`PARSE_OK`** | Parse succeeded |
| **`PARSE_ERROR_UNEXPECTED_CHAR`** | Unexpected character in input |
| **`PARSE_ERROR_UNEXPECTED_EOF`** | Input ended prematurely |
| **`PARSE_ERROR_MISSING_COLON`** | No `:` after object key |
| **`PARSE_ERROR_EMPTY_INPUT`** | Empty source string |
| **`PARSE_ERROR_TRAILING_COMMA`** | Trailing comma before `]` or `}` |
| **`PARSE_ERROR_INVALID_NUMBER`** | Malformed number |
| **`PARSE_ERROR_INVALID_STRING`** | Unterminated or invalid string |
| **`PARSE_ERROR_INVALID_ESCAPE`** | Bad escape sequence |
| **`PARSE_ERROR_INVALID_UNICODE`** | Bad `\uXXXX` or surrogate pair |
| **`PARSE_ERROR_UNMATCHED_BRACKET`** | `[` without matching `]` |
| **`PARSE_ERROR_UNMATCHED_BRACE`** | `{` without matching `}` |
| **`PARSE_ERROR_NEST_TOO_DEEP`** | Nesting exceeds `PARSE_NEST_LIMIT` |
| **`PARSE_ERROR_OOM`** | Arena allocation failed |

## Design Decisions

### Why arena allocator for JSON parsing?

JSON parsing typically involves hundreds to thousands of small allocations (strings, nodes, keys). Using `malloc`/`free` per node incurs significant overhead from system calls, bookkeeping, and fragmentation. With an arena:

- **Allocation is O(1) pointer bump** — no search for free blocks
- **No individual `free`** — the entire parse tree is discarded together
- **Cache-friendly** — nodes are allocated sequentially in contiguous blocks
- **Zero fragmentation** — memory is never freed individually, only reset or destroyed

### Why recursive descent?

Recursive descent is the most straightforward and debuggable parsing strategy for JSON. Each grammar rule maps to exactly one function (`parse_object`, `parse_array`, `parse_number`, etc.). The trade-off is stack depth proportional to nesting, but this is bounded by `PARSE_NEST_LIMIT`.

### Why two-pass stringify?

The arena allocator does not support `realloc`. To produce a correctly sized output string without over-allocating:

1. **Pass 1**: walk the tree to compute exact byte count needed
2. **Pass 2**: allocate once from arena, then write into the buffer

This avoids both the `realloc` problem and wasted memory.

### Why strict mode?

Many JSON parsers silently accept trailing commas (`[1, 2,]`), but RFC 8259 does not permit them. Strict mode catches more bugs at parse time. If lenient mode is needed in the future, it can be controlled by a parse flag.

## Testing

The project includes a comprehensive test JSON payload (`main.c`) covering:

- **Nested objects and arrays** — GitHub API-style deep nesting
- **All JSON types** — `null`, `true`, `false`, numbers, strings, arrays, objects
- **UTF-16 surrogate pairs** — `\uD834\uDD1E` → musical G clef
- **Escape sequences** — `\\`, `\"`, `\n`, `\t`, `\r`, `\b`, `\f`
- **Stringify round-trip** — parse → display → stringify → re-parse

Run the tests:
bash
gcc -std=c11 -Wall -O2 -I. -o cjson_demo main.c cjson.c arena.c
./cjson_demo

Expected output: `parse success.` followed by pretty-printed JSON.

## Project Structure
.
├── main.c # Demo/test entry point with GitHub API-style JSON
├── cjson.h # Public API and type definitions
├── cjson.c # Parser implementation (parse, display, stringify)
├── arena.h # Arena allocator public API
├── arena.c # Arena allocator implementation
└── README.md

## License

MIT License. See source files for full text.

---

## Future Work

- [x] **`arena_reset`** — zero all block offsets without freeing blocks ✅
- [x] **`cjson_stringify`** — two-pass serialization ✅
- [ ] **`arena_checkpoint`** / **`arena_rollback`** — scoped allocation with revert
- [ ] **Benchmark suite** — compare against cJSON and simdjson
- [ ] **Streaming parser** — parse from FILE* without loading entire input
- [ ] **Lenient mode** — optional trailing comma and comment support

---

> Built from scratch with a deep understanding of memory management and parsing, not copied from tutorials. 💪