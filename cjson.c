#include "cjson.h"

/* JSON value types */
enum cjson_type{

    cjson_true,
    cjson_false,
    cjson_null,
    cjson_number,
    cjson_string,
    cjson_array,
    cjson_object

};

/* Union holding the actual value data for a JSON node.
 * Which member is valid depends on the node's type. */
union cjson_data{

    double   number; /* cjson_number */
    char*    string; /* cjson_string */ 
    cjson_t* child;  /* cjson_array / cjson_object: linked list of children */

};

/* A single JSON value node.
 * Arrays and objects are stored as a doubly-linked list of child nodes,
 * each optionally carrying a key (for object members). */
struct cjson_t{

    cjson_type      type;

    unsigned char*  key;    /* non-NULL only for object keys */
    cjson_data      value;

    struct cjson_t* prev;
    struct cjson_t* next;

};

/* Parser state, passed around all parse_* functions */
struct parse_buffer {

    const char*     context;           /* raw JSON text */
    size_t          length;            /* length of context */
    size_t          cur_nesting_depth; /* current nesting level (for depth limit) */
    size_t          cur_pos;           /* current read position */

    arena*          arena_pool;        /* arena for all allocations */
    parse_error_t   code;              /* error code, PARSE_OK if no error */

};

/* ============================================================
 *  Utility / helper functions
 * ============================================================ */

/* Check if the buffer has at least n more bytes readable from current position */
static inline bool buffer_can_lookahead(const parse_buffer* const buffer, size_t n){

    if(buffer == NULL){
        return false;
    }

    return (buffer->cur_pos + n) < buffer->length;

}

/* Return pointer to the character at current read position */
static inline const unsigned char* buffer_at_curpos_pointer(const parse_buffer* const buffer){

    if(buffer == NULL || buffer->context == NULL){
        return NULL;
    }

    return buffer->context + buffer->cur_pos;

}

/* Check if character c can start a JSON number (- or 0-9) */
static inline bool is_number_start(const unsigned char c){

    return (c == '-') || (c >= '0' && c <= '9');

}

/* Check if character c is part of the number body (digits, exponent, sign, decimal point) */
static inline bool is_number_body(const unsigned char c){

    return (c >= '0' && c <= '9') || (c == 'e') || (c == 'E') || (c == '+') || (c == '-') || (c == '.');

}

/* Skip whitespace characters (space, tab, newline, carriage return, etc.) */
static void buffer_skip_whitespace(parse_buffer* const buffer){

    if(buffer == NULL){
        return;
    }

    while(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] <= 32){
        buffer->cur_pos++;
    }

}


/* Check if c is a valid non-unicode escape character (\b \f \n \r \t \\ \/ \") */
static inline bool is_escape_sequence_except_unicode(const unsigned char c){

    switch (c)
    {
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
        case '\\':
        case '/':
        case '\"':
            return true;
    
        default:
            return false;
    }

}

/* Convert a non-unicode escape character to its actual character value */
static inline unsigned char parse_escape_sequence_except_unicode(const unsigned char c){

    switch (c)
    {
        case 'b':
            return '\b';
        case 'f':
            return '\f';
        case 'n':
            return '\n';
        case 'r':
            return '\r';
        case 't':
            return '\t';
        case '\\':
            return '\\';
        case '/':
            return '/';
        case '\"':
            return '\"';
    }

}


/* Check if the string at str starts with \uXXXX or \UXXXX (4 hex digits) */
static inline bool is_utf16(const unsigned char* str){

    if( str[0] != '\\' || (str[1] != 'u' && str[1] != 'U') ){
        return false;
    }

    const unsigned char* hex_start = str + 2;

    for(size_t i = 0; i < 4; i++){

        char c = hex_start[i];

        if( (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ){
            continue;
        }

        return false;

    }

    return true;

}

/* Convert a single hex character (0-9, a-f, A-F) to its 0-15 value */
static uint8_t uchar_to_hex(unsigned char c){

    if(c >= '0' && c <= '9'){
        return (c - '0');
    }

    if(c >= 'a' && c <= 'f'){
        return 10 + (c - 'a');
    }

    if(c >= 'A' && c <= 'F'){
        return 10 + (c - 'A');
    }

    return 0xff;

}

/* Parse 4 hex digits into a uint16_t (big-endian within the 4-digit sequence) */
static uint16_t parse_hex4(const unsigned char* str){

    uint16_t res = 0;

    for(size_t i = 0; i < 4; i++){

        uint8_t digit = uchar_to_hex(str[i]);

        res = ((res << 4) | digit);

    }

    return res;

}

/* Check if codepoint is a valid BMP character (not in surrogate range) */
static bool is_bmp_char(uint16_t c) {
    return c < 0xD800 || c > 0xDFFF;
}

/* Check if codepoint is a high surrogate (U+D800 to U+DBFF) */
static int is_high_surrogate(uint16_t c) {
    return c >= 0xD800 && c <= 0xDBFF;
}

/* Check if codepoint is a low surrogate (U+DC00 to U+DFFF) */
static int is_low_surrogate(uint16_t c) {
    return c >= 0xDC00 && c <= 0xDFFF;
}

/* Convert UTF-16 surrogate pair (high, low) to a Unicode codepoint (U+10000 and above) */
static uint32_t surrogate_pair_to_codepoint(uint16_t high, uint16_t low) {
    return 0x10000
         + ((high - 0xD800) << 10)
         +  (low  - 0xDC00);
}

/* Encode a Unicode codepoint into UTF-8 bytes, writing up to 4 bytes into buf.
 * Returns the number of bytes written. Invalid codepoints emit U+FFFD. */
static size_t parse_unicode_to_utf8(uint32_t codepoint, unsigned char* buf){

    if(buf == NULL){
        return 0;
    }

    if(codepoint <= 0x7F){
        // 1 byte: 0xxxxxxx
        buf[0] = (unsigned char)codepoint;
        return 1;
    }
    else if(codepoint >= 0x80 && codepoint <= 0x7FF){
        // 2 bytes: 110xxxxx 10xxxxxx
        buf[0] = (unsigned char)(0xC0 | (codepoint >> 6));
        buf[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    else if(codepoint >= 0x800 && codepoint <= 0xFFFF){
        // 3 bytes: 1110xxxx 10xxxxxx 10xxxxxx
        buf[0] = (unsigned char)(0xE0 | (codepoint >> 12));
        buf[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
        return 3;
    }
    else if(codepoint >= 0x10000 && codepoint <= 0x10FFFF){
        // 4 bytes: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        buf[0] = (unsigned char)(0xF0 | (codepoint >> 18));
        buf[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
        buf[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
        buf[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    else{
        // Invalid codepoint (> U+10FFFF): emit replacement character U+FFFD
        buf[0] = 0xEF;
        buf[1] = 0xBF;
        buf[2] = 0xBD;
        return 3;
    }

}

/* Parse a JSON string value from the buffer.
 * Handles escape sequences and \uXXXX unicode, including surrogate pairs.
 * Returns allocated UTF-8 string, or NULL on error (buffer->code is set). */
static unsigned char* parse_string(parse_buffer* const buffer){

    buffer_skip_whitespace(buffer);

    size_t read_uchar  = 0;
    size_t valid_uchar = 0;

    while (buffer_can_lookahead(buffer, read_uchar) && buffer_at_curpos_pointer(buffer)[read_uchar] != '\"'){

        if(buffer_at_curpos_pointer(buffer)[read_uchar] == '\\'){

            if(buffer_can_lookahead(buffer, read_uchar + 1) && is_escape_sequence_except_unicode(buffer_at_curpos_pointer(buffer)[read_uchar + 1]) ){
                read_uchar  += 2;
                valid_uchar += 1;
            }
            else if(buffer_can_lookahead(buffer, read_uchar + 6) && is_utf16(buffer_at_curpos_pointer(buffer) + read_uchar)){
                read_uchar  += 6;
                valid_uchar += 4;
            }
            else{
                buffer->code = PARSE_ERROR_INVALID_ESCAPE;
                return NULL;
            }

        }
        else{
            read_uchar  += 1;
            valid_uchar += 1;
        }

    }

    if(buffer_can_lookahead(buffer, read_uchar) == false){
        buffer->code = PARSE_ERROR_INVALID_STRING;
        return NULL;
    }
    
    unsigned char* str = (unsigned char*)arena_calloc(buffer->arena_pool, valid_uchar + 1, sizeof(unsigned char));

    if(str == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    size_t str_pos = 0;
    size_t src_pos = 0;

    while(src_pos < read_uchar){

        if(buffer_at_curpos_pointer(buffer)[src_pos] == '\\'){

            if( is_escape_sequence_except_unicode(buffer_at_curpos_pointer(buffer)[src_pos + 1]) ){

                str[str_pos] = parse_escape_sequence_except_unicode(buffer_at_curpos_pointer(buffer)[src_pos + 1]);

                str_pos += 1;
                src_pos += 2;

            }
            else{

                uint16_t u1 = parse_hex4(buffer_at_curpos_pointer(buffer) + src_pos + 2);

                if(is_bmp_char(u1)){

                    str_pos += parse_unicode_to_utf8(u1, str + str_pos);
                    src_pos += 6;

                }
                else{

                    if(is_high_surrogate(u1) == false){
                        buffer->code = PARSE_ERROR_INVALID_UNICODE;
                        return NULL;
                    }

                    if(buffer_can_lookahead(buffer, src_pos + 7) == false || is_utf16(buffer_at_curpos_pointer(buffer) + src_pos + 6) == false){
                        buffer->code = PARSE_ERROR_INVALID_UNICODE;
                        return NULL;
                    }

                    uint16_t u2 = parse_hex4(buffer_at_curpos_pointer(buffer) + src_pos + 8);

                    if(is_low_surrogate(u2) == false){
                        buffer->code = PARSE_ERROR_INVALID_UNICODE;
                        return NULL;
                    }

                    str_pos += parse_unicode_to_utf8(surrogate_pair_to_codepoint(u1, u2), str + str_pos);
                    src_pos += 12;

                }

            }


        }
        else{

            str[str_pos] = buffer_at_curpos_pointer(buffer)[src_pos];

            str_pos += 1;
            src_pos += 1;

        }

    }

    buffer->cur_pos += (read_uchar + 1);

    return str;

}


/* Parse a JSON null literal. Advances buffer position and validates trailing char. */
cjson_t* parse_null(parse_buffer* const buffer){

    cjson_t* res_item = (cjson_t*)arena_calloc(buffer->arena_pool, 1, sizeof(cjson_t));

    if(res_item == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    res_item->type = cjson_null;

    buffer->cur_pos += 4;

    // Strict mode: character after null must be whitespace, delimiter, or end of input
    if (buffer_can_lookahead(buffer, 1)){

        unsigned char next = buffer_at_curpos_pointer(buffer)[0];

        if (next != ' ' && next != '\t' && next != '\n' && next != '\r'
            && next != '}' && next != ']' && next != ','){

            buffer->code = PARSE_ERROR_UNEXPECTED_CHAR;
            return NULL;

        }

    }

    return res_item;

}

/* Parse a JSON true literal. Advances buffer position and validates trailing char. */
cjson_t* parse_true(parse_buffer* const buffer){

    cjson_t* res_item = (cjson_t*)arena_calloc(buffer->arena_pool , 1, sizeof(cjson_t));

    if(res_item == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    res_item->type = cjson_true;

    buffer->cur_pos += 4;

    // Strict mode: character after true must be whitespace, delimiter, or end of input
    if (buffer_can_lookahead(buffer, 1)){

        unsigned char next = buffer_at_curpos_pointer(buffer)[0];

        if (next != ' ' && next != '\t' && next != '\n' && next != '\r'
            && next != '}' && next != ']' && next != ','){

            buffer->code = PARSE_ERROR_UNEXPECTED_CHAR;
            return NULL;

        }
        
    }

    return res_item;

}

/* Parse a JSON false literal. Advances buffer position and validates trailing char. */
cjson_t* parse_false(parse_buffer* const buffer){

    cjson_t* res_item = (cjson_t*)arena_calloc(buffer->arena_pool , 1, sizeof(cjson_t));

    if(res_item == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    res_item->type = cjson_false;

    buffer->cur_pos += 5;

    // Strict mode: character after false must be whitespace, delimiter, or end of input
    if (buffer_can_lookahead(buffer, 1)){

        unsigned char next = buffer_at_curpos_pointer(buffer)[0];

        if (next != ' ' && next != '\t' && next != '\n' && next != '\r'
            && next != '}' && next != ']' && next != ','){

            buffer->code = PARSE_ERROR_UNEXPECTED_CHAR;
            return NULL;

        }
        
    }

    return res_item;

}

/* Parse a JSON number. Uses strtod for conversion. Returns cjson_number node. */
cjson_t* parse_number(parse_buffer* const buffer){

    size_t cnt = 0;

    while(buffer_can_lookahead(buffer, cnt)){

        if(is_number_body(buffer_at_curpos_pointer(buffer)[cnt]) == false){
            break;
        }

        cnt++;

    }

    if(cnt == 0){
        buffer->code = PARSE_ERROR_INVALID_NUMBER;
        return NULL;
    }

    unsigned char* number_string_copy = NULL;
    unsigned char* return_end = NULL;
    double number = 0;

    number_string_copy = arena_calloc(buffer->arena_pool, cnt + 1, sizeof(unsigned char));

    if(number_string_copy == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    memcpy(number_string_copy, buffer_at_curpos_pointer(buffer), cnt);
    number_string_copy[cnt] = '\0';


    number = strtod((const char*)number_string_copy, (char**)&return_end);

    if(return_end == number_string_copy){
        buffer->code = PARSE_ERROR_INVALID_NUMBER;
        return NULL;
    }

    cjson_t* res_item = (cjson_t*)arena_calloc(buffer->arena_pool, 1, sizeof(cjson_t));

    if(res_item == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    res_item->type = cjson_number;
    res_item->value.number = number;

    buffer->cur_pos += (return_end - number_string_copy);

    return res_item;

}

/* Parse a JSON string value (after the opening quote). Returns cjson_string node. */
cjson_t* parse_valuestring(parse_buffer* const buffer){

    if(buffer == NULL){
        return NULL;
    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '\"'){
        buffer->cur_pos++;
    }
    else{
        buffer->code = PARSE_ERROR_INVALID_STRING;
        return NULL;
    }

    unsigned char* str = parse_string(buffer);

    if(str == NULL){
        return NULL;
    }

    cjson_t* res_item = (cjson_t*)arena_calloc(buffer->arena_pool, 1, sizeof(cjson_t));

    if(res_item == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    res_item->type = cjson_string;
    res_item->value.string = str;

    return res_item;

}

/* Parse a JSON array. Children are linked via next/prev. Enforces nesting depth limit. */
cjson_t* parse_array(parse_buffer* const buffer){

    if(buffer == NULL){
        return NULL;
    }

    buffer->cur_nesting_depth++;

    if(buffer->cur_nesting_depth > PARSE_NEST_LIMIT){
        buffer->code = PARSE_ERROR_NEST_TOO_DEEP;
        return NULL;
    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '['){
        buffer->cur_pos++;
    }
    else{
        buffer->code = PARSE_ERROR_INVALID_ARRAY;
        return NULL;
    }

    cjson_t* head_item      = NULL;
    cjson_t* current_item   = NULL;

    while(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] != ']'){

        buffer_skip_whitespace(buffer);

        cjson_t* new_item = cjson_parse_value(buffer);

        if(new_item == NULL){
            return NULL;
        }

        if(head_item == NULL && current_item == NULL){
            head_item = current_item = new_item;
        }
        else{
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        buffer_skip_whitespace(buffer);

        if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == ','){

            buffer->cur_pos++;

            buffer_skip_whitespace(buffer);

            if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == ']'){
                buffer->code = PARSE_ERROR_TRAILING_COMMA;
                return NULL;
            }

        }

    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == ']'){
        buffer->cur_pos++;
    }
    else{
        buffer->code = PARSE_ERROR_UNMATCHED_BRACKET;
        return NULL;
    }

    buffer->cur_nesting_depth--;

    cjson_t* res_item = (cjson_t*)arena_calloc(buffer->arena_pool, 1, sizeof(cjson_t));

    if(res_item == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    res_item->type = cjson_array;
    res_item->value.child = head_item;

    return res_item;

}

/* Parse a JSON object key (a quoted string). Returns the allocated key string. */
static unsigned char* parse_keystring(parse_buffer* const buffer){

    if(buffer == NULL){
        return NULL;
    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '\"'){
        buffer->cur_pos++;
    }
    else{
        buffer->code = PARSE_ERROR_INVALID_STRING;
        return NULL;
    }

    return parse_string(buffer);

}

/* Parse a JSON object. Key-value pairs are stored as child nodes with ->key set. */
cjson_t* parse_object(parse_buffer* const buffer){

    if(buffer == NULL){
        return NULL;
    }

    buffer->cur_nesting_depth++;

    if(buffer->cur_nesting_depth > PARSE_NEST_LIMIT){
        buffer->code = PARSE_ERROR_NEST_TOO_DEEP;
        return NULL;
    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '{'){
        buffer->cur_pos++;
    }
    else{
        buffer->code = PARSE_ERROR_INVALID_OBJECT;
        return NULL;
    }

    cjson_t* head_item      = NULL;
    cjson_t* current_item   = NULL;

    while(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] != '}'){

        buffer_skip_whitespace(buffer);

        unsigned char* keystring = parse_keystring(buffer);

        if(keystring == NULL){
            return NULL;
        }

        buffer_skip_whitespace(buffer);

        if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == ':'){
            buffer->cur_pos++;
        }
        else{
            buffer->code = PARSE_ERROR_MISSING_COLON;
            return NULL;
        }

        buffer_skip_whitespace(buffer);

        cjson_t* new_item = cjson_parse_value(buffer);

        if(new_item == NULL){
            return NULL; 
        }

        new_item->key = keystring;

        if(head_item == NULL && current_item == NULL){
            head_item = current_item = new_item;
        }
        else{
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        buffer_skip_whitespace(buffer);

        if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == ','){

            buffer->cur_pos++;

            buffer_skip_whitespace(buffer);

            if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '}'){
                buffer->code = PARSE_ERROR_TRAILING_COMMA;
                return NULL;
            }

        }



    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '}'){
        buffer->cur_pos++;
    }
    else{
        buffer->code = PARSE_ERROR_UNMATCHED_BRACE;
        return NULL;
    }

    buffer->cur_nesting_depth--;

    cjson_t* res_item = (cjson_t*)arena_calloc(buffer->arena_pool, 1, sizeof(cjson_t));

    if(res_item == NULL){
        buffer->code = PARSE_ERROR_OOM;
        return NULL;
    }

    res_item->type = cjson_object;
    res_item->value.child = head_item;

    return res_item;

}

/* Dispatch function: examine the next character(s) and call the appropriate parse_* function. */
cjson_t* cjson_parse_value(parse_buffer* const buffer){

    if(buffer == NULL || buffer->context == NULL || buffer->length == 0 || buffer->cur_pos == buffer->length){
        buffer->code = PARSE_ERROR_UNEXPECTED_EOF;
        return NULL;
    }

    if(buffer_can_lookahead(buffer, 4) && strncmp(buffer_at_curpos_pointer(buffer), "null", 4) == 0){
        return parse_null(buffer);
    }

    if(buffer_can_lookahead(buffer, 4) && strncmp(buffer_at_curpos_pointer(buffer), "true", 4) == 0){
        return parse_true(buffer);
    }

    if(buffer_can_lookahead(buffer, 5) && strncmp(buffer_at_curpos_pointer(buffer), "false", 5) == 0){
        return parse_false(buffer);
    }

    if(buffer_can_lookahead(buffer, 1) && is_number_start(buffer_at_curpos_pointer(buffer)[0])){
        return parse_number(buffer);
    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '\"'){
        return parse_valuestring(buffer);
    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '['){
        return parse_array(buffer);
    }

    if(buffer_can_lookahead(buffer, 1) && buffer_at_curpos_pointer(buffer)[0] == '{'){
        return parse_object(buffer);
    }

    buffer->code = PARSE_ERROR_UNEXPECTED_CHAR;
    return NULL;

}

/* Debug helper: print the buffer content up to current position */
static void buffer_display(parse_buffer* buffer){

    if(buffer == NULL){
        return;
    }

    printf("the stop place is %lu\n\n", buffer->cur_pos);
    printf("The parsed string is-->");

    unsigned char* str = (unsigned char*)calloc(buffer->cur_pos + 1, sizeof(unsigned char));

    memcpy(str, buffer->context, buffer->cur_pos);

    printf("%s\n\n", str);

}

/* Public API: parse a JSON text string into a cjson_t tree using the given arena.
 * On success, *cjson points to the root node and "parse success." is printed.
 * On failure, buffer->code indicates the error and a message is printed. */
void cjson_parse(const char* src, arena* const arena_pool, cjson_t** cjson){

    if(src == NULL){
        return;
    }

    if(arena_pool == NULL){
        return;
    }

    parse_buffer src_buffer = {

        .context = (const unsigned char*)src,
        .length  = strlen(src) + sizeof(""),
        .cur_nesting_depth = 0,
        .cur_pos = 0,
        .arena_pool = arena_pool,
        .code = PARSE_OK

    };

    buffer_skip_whitespace(&src_buffer);

    if(src_buffer.length == src_buffer.cur_pos){
        src_buffer.code = PARSE_ERROR_EMPTY_INPUT;
        return;
    }

    *cjson = cjson_parse_value(&src_buffer);

    if(src_buffer.code == PARSE_OK){
        printf("parse success.\n\n");
    }
    else{

        printf("the failed position is %lu.\n\n", src_buffer.cur_pos);

        switch (src_buffer.code)
        {
        case PARSE_ERROR_UNEXPECTED_CHAR:
            printf("Unexpected char.\n\n");
            break;
        
        case PARSE_ERROR_UNEXPECTED_EOF:
            printf("Unexpected EOF.\n\n");
            break;

        case PARSE_ERROR_MISSING_COLON:
            printf("Expected ':' after object key.\n\n");
            break;

        case PARSE_ERROR_EMPTY_INPUT:
            printf("Empty source string.\n\n");
            break;
        
        case PARSE_ERROR_TRAILING_COMMA:
            printf("Trailing comma.\n\n");
            break;

        case PARSE_ERROR_INVALID_NUMBER:
            printf("Invalid number.\n\n");
            break;
        
        case PARSE_ERROR_INVALID_STRING:
            printf("String parsing failed.\n\n");
            break;
        
        case PARSE_ERROR_INVALID_ESCAPE:
            printf("Invaid escape seuqence.\n\n");
            break;

        case PARSE_ERROR_INVALID_UNICODE:
            printf("Invalid unicode.\n\n");
            break;

        case PARSE_ERROR_UNMATCHED_BRACKET:
            printf("Unmatched bracket.\n\n");
            break;
        
        case PARSE_ERROR_UNMATCHED_BRACE:
            printf("Unmatched brace.\n\n");
            break;

        case PARSE_ERROR_NEST_TOO_DEEP:
            printf("Out of the nest limit.\n\n");
            break;

        case PARSE_ERROR_OOM:
            printf("Out of memory.\n\n");
            break;

        }

    }

}



/* Check if the JSON node has array/object children that contain nested arrays/objects.
 * Used by display to decide whether to use multi-line formatting. */
static bool has_complex_children(cjson_t* cjson){

    if(cjson == NULL){
        return false;
    }

    if(cjson->type != cjson_array && cjson->type != cjson_object){
        return false;
    }

    cjson_t* current_item = cjson->value.child;

    while(current_item != NULL){

        if(current_item->type == cjson_array || current_item->type == cjson_object){
            return true;
        }

        current_item = current_item->next;

    }

    return false;

}

/* Print 2 spaces per nesting level for pretty-print indentation */
static void display_leading_whitespace(size_t cur_nest_depth){

    for(size_t i = 0; i < cur_nest_depth; i++){
        printf("  ");
    }

}

/* Recursively pretty-print a JSON node to stdout with indentation */
static void cjson_display_with_nestinfo(cjson_t* cjson, size_t cur_nest_depth){

    if(cjson == NULL){
        return;
    }

    switch (cjson->type)
    {
    case cjson_null:

        printf("null");
        break;
    
    case cjson_true:
        
        printf("true");
        break;

    case cjson_false:
        
        printf("false");
        break;

    case cjson_number:

        double num = cjson->value.number;

        if(num == (long long)num){
            printf("%lld", (long long)num);
        }
        else{
            printf("%.6g", num);
        }

        break;
    
    case cjson_string:
        
        printf("\"%s\"", cjson->value.string);
        break;

    case cjson_array:
        
        printf("[");

        {

        bool flag = has_complex_children(cjson);

        cjson_t* current_item = cjson->value.child;

        while(current_item != NULL){

            if(flag){

                printf("\n");
                display_leading_whitespace(cur_nest_depth + 1);

            }

            cjson_display_with_nestinfo(current_item, cur_nest_depth + 1);

            if(current_item->next != NULL){

                printf(",");

                if(!flag){
                    printf(" ");
                }

            }

            current_item = current_item->next;

        }

        if(flag){

            printf("\n");
            display_leading_whitespace(cur_nest_depth);

        }
        
        }      

        printf("]");

        break;

    case cjson_object:
        
        printf("{");

        {

        bool flag = has_complex_children(cjson);

        cjson_t* current_item = cjson->value.child;

        while(current_item != NULL){

            if(flag){
                printf("\n");
                display_leading_whitespace(cur_nest_depth + 1);
            }

            printf("\"%s\" : ", current_item->key);
            cjson_display_with_nestinfo(current_item, cur_nest_depth + 1);

            if(current_item->next != NULL){

                printf(",");

                if(!flag){
                    printf(" ");
                }

            }

            current_item = current_item->next;

        }

        if(flag){
            printf("\n");
            display_leading_whitespace(cur_nest_depth);
        }

        }

        printf("}");

        break;

    }

}

/* Public API: pretty-print the parsed JSON tree to stdout */
void cjson_display(cjson_t* cjson){

    cjson_display_with_nestinfo(cjson, 0);
    
}


/* Calculate the UTF-8 byte length needed to escape a string for JSON output.
 * Accounts for escape sequences and \u00XX encoding of control characters. */
static size_t cjson_stringify_size_string(const unsigned char* str){

    size_t len = 0;

    while (*str){
        
        if (*str == '\"' || *str == '\\' || *str == '\n' || *str == '\t' ||
            *str == '\r' || *str == '\b' || *str == '\f'){
            len += 2;  // escaped as two characters
        } 
        else if (*str < 0x20){
            len += 6;  // \u00XX
        }
        else {
            len += 1;
        }
        str++;

    }

    return len;

}

/* Pass 1: calculate the exact byte size needed to serialize the JSON tree.
 * Returns the total size (including null terminator, added by caller). */
static size_t cjson_stringify_size(const cjson_t* cjson){

    if(cjson == NULL){
        return 0;
    }

    switch (cjson->type){

        case cjson_null:   return 4;

        case cjson_true:   return 4;

        case cjson_false:  return 5;

        case cjson_number: {
            // Rough estimate: max double string length ~24 chars
            return 24;
        }

        case cjson_string: {

            if (cjson->value.string == NULL) return 2; // ""

            size_t len = 2; // two quotes

            len += cjson_stringify_size_string(cjson->value.string);

            return len;

        }

        case cjson_array: {

            size_t len = 2; //[]
            cjson_t* cur = cjson->value.child;

            while (cur){

                len += cjson_stringify_size(cur);

                if(cur->next){
                    len += 2;
                } //comma + space

                cur = cur->next;

            }

            return len;

        }

        case cjson_object: {

            size_t len = 2; //{}
            cjson_t* cur = cjson->value.child;

            while (cur){

                len += cjson_stringify_size_string(cur->key);
                len += 3; //space + colon + space
                len += cjson_stringify_size(cur); //value length
        
                if (cur->next) len += 2; //comma + space
                cur = cur->next;

            }

            return len;

        }

    }

    return 0;

}

/* Write a string value into the output buffer, escaping special characters as needed */
static void cjson_stringify_write_string(unsigned char* str, unsigned char* buf, size_t* pos){

    while(*str){

        if     (*str == '\"') { buf[(*pos)++] = '\\'; buf[(*pos)++] = '\"'; }
        else if(*str == '\\') { buf[(*pos)++] = '\\'; buf[(*pos)++] = '\\'; }
        else if(*str == '\n') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'n';  }
        else if(*str == '\t') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 't';  }
        else if(*str == '\r') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'r';  }
        else if(*str == '\b') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'b';  }
        else if(*str == '\f') { buf[(*pos)++] = '\\'; buf[(*pos)++] = 'f';  }
        else if(*str < 0x20){
            // Control character encoded as \u00XX
            static const char hex[] = "0123456789ABCDEF";
            buf[(*pos)++] = '\\'; buf[(*pos)++] = 'u';
            buf[(*pos)++] = '0'; buf[(*pos)++] = '0';
            buf[(*pos)++] = hex[*str >> 4];
            buf[(*pos)++] = hex[*str & 0x0F];

        } 
        else{
            buf[(*pos)++] = *str;
        }

        str++;

    }

}

/* Pass 2: recursively write the JSON tree into the pre-allocated buffer at *pos */
static void cjson_stringify_write(cjson_t* cjson, unsigned char* buf, size_t* pos){

    if (cjson == NULL){
        return;
    }

    switch (cjson->type){

        case cjson_null:

            memcpy(buf + *pos, "null", 4);
            *pos += 4; 
            return;

        case cjson_true:

            memcpy(buf + *pos, "true", 4);
            *pos += 4; 
            return;

        case cjson_false:

            memcpy(buf + *pos, "false", 5);
            *pos += 5; 
            return;

        case cjson_number:

            *pos += snprintf((char*)buf + *pos, 24, "%.17g", cjson->value.number);
            return;
        

        case cjson_string:

            buf[(*pos)++] = '\"';

            if (cjson->value.string){

                cjson_stringify_write_string((unsigned char*)cjson->value.string, buf, pos);

            }

            buf[(*pos)++] = '\"';

            return;

        case cjson_array:{

            buf[(*pos)++] = '[';
            cjson_t* cur = cjson->value.child;

            while (cur){

                cjson_stringify_write(cur, buf, pos);

                if (cur->next){
                    memcpy(buf + *pos, ", ", 2);
                    *pos += 2;
                }

                cur = cur->next;

            }

            buf[(*pos)++] = ']';
            return;

        }

        case cjson_object: {

            buf[(*pos)++] = '{';
            cjson_t* cur = cjson->value.child;

            while(cur){

                // write key
                buf[(*pos)++] = '\"';
                cjson_stringify_write_string(cur->key, buf, pos);
                buf[(*pos)++] = '\"';

                memcpy(buf + *pos, " : ", 3);
                *pos += 3;

                // write value
                cjson_stringify_write(cur, buf, pos);

                if (cur->next){
                    memcpy(buf + *pos, ", ", 2);
                    *pos += 2;
                }

                cur = cur->next;

            }

            buf[(*pos)++] = '}';

            return;

        }

    }

}

/* Public API: serialize a parsed JSON tree back to a JSON text string.
 * Uses two-pass approach (size calculation + write) since arena has no realloc.
 * Returns arena-allocated string, or NULL on failure. */
unsigned char* cjson_stringify(cjson_t* cjson, arena* arena_pool){

    if(cjson == NULL){
        return NULL;
    }

    unsigned char* res_buf = arena_calloc(arena_pool, cjson_stringify_size(cjson) + 1, sizeof(unsigned char));

    if(res_buf == NULL){
        return NULL;
    }

    size_t res_buf_pos = 0;

    cjson_stringify_write(cjson, res_buf, &res_buf_pos);

    return res_buf;

}


/* Recursively compute the maximum depth of the JSON tree (for diagnostics) */
static size_t cjson_depth(cjson_t* cjson){

    if(cjson == NULL){
        return 0;
    }

    size_t max_depth = 0;

    cjson_t* current_item = NULL;

    if(cjson->type == cjson_array || cjson->type == cjson_object){
        current_item = cjson->value.child;
    }

    while(current_item != NULL){

        size_t cur_depth = cjson_depth(current_item);

        max_depth = max_depth > cur_depth ? max_depth : cur_depth;

        current_item = current_item->next;
        
    }

    return max_depth + 1;

}