#pragma once

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h> //size_t
#include <stdint.h> //SIZE_MAX
#include "ARENA_LIB/arena.h"



#ifndef PARSE_NEST_LIMIT

#define PARSE_NEST_LIMIT 1000

#endif



typedef struct cjson_t cjson_t;

/* Parse error type, returned via parse_buffer->code */
typedef enum error_type{

    PARSE_OK = 0,

    /* ---- structural errors ---- */
    PARSE_ERROR_UNEXPECTED_CHAR,      /* unexpected character in input */
    PARSE_ERROR_UNEXPECTED_EOF,       /* input ended prematurely */
    PARSE_ERROR_MISSING_COLON,        /* no ':' after object key */
    PARSE_ERROR_EMPTY_INPUT,          /* empty source string */
    PARSE_ERROR_TRAILING_COMMA,       /* trailing comma before ] or } */

    /* ---- value errors ---- */
    PARSE_ERROR_INVALID_NUMBER,       /* malformed number */
    PARSE_ERROR_INVALID_STRING,       /* unterminated or invalid string */
    PARSE_ERROR_INVALID_ARRAY,        /* array parse failed */
    PARSE_ERROR_INVALID_OBJECT,       /* object parse failed */
    PARSE_ERROR_INVALID_ESCAPE,       /* bad escape sequence \x */
    PARSE_ERROR_INVALID_UNICODE,      /* bad \uXXXX or surrogate pair */

    /* ---- structural integrity ---- */
    PARSE_ERROR_UNMATCHED_BRACKET,    /* '[' without matching ']' */
    PARSE_ERROR_UNMATCHED_BRACE,      /* '{' without matching '}' */
    PARSE_ERROR_NEST_TOO_DEEP,        /* nesting exceeds PARSE_NEST_LIMIT */

    /* ---- memory ---- */
    PARSE_ERROR_OOM                   /* arena allocation failed */

}error_type;

typedef struct parse_error_t{

    error_type err_type;
    size_t position;

}parse_error_t;

/* Main entry point: parse a JSON string into a cjson_t tree.
 * On success, *cjson points to the root node and buffer->code == PARSE_OK.
 * Caller must provide a valid arena; all nodes are allocated from it. */
parse_error_t cjson_parse(const char* src, arena* const arena_pool, cjson_t** cjson);

/* Pretty-print the parsed JSON tree to stdout */
void cjson_display(cjson_t* cjson);

/* Serialize a cjson_t tree back to a JSON string.
 * Returns a newly allocated string from arena_pool, or NULL on failure. */
unsigned char* cjson_stringify(cjson_t* cjson, arena* arena_pool);

/* Recursively search for a child node by key in an object or array.
 * Returns the first matching node, or NULL if not found. */
cjson_t* cjson_get_item(const char* key, cjson_t* object);

