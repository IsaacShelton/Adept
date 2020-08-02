
#include <stdarg.h>
#include "UTIL/util.h"

void expand(void **inout_memory, length_t unit_size, length_t length, length_t *inout_capacity, length_t amount, length_t default_capacity){
    // Expands an array in memory to be able to fit more units

    if(*inout_capacity == 0 && amount != 0){
        *inout_memory = malloc(unit_size * default_capacity);
        *inout_capacity = default_capacity;
    }

    while(length + amount > *inout_capacity){
        *inout_capacity *= 2;
        
        #ifdef UTIL_USE_REALLOC
        *inout_memory = realloc(*inout_memory, unit_size * *inout_capacity);
        #else
        void *new_memory = malloc(unit_size * *inout_capacity);
        memcpy(new_memory, *inout_memory, unit_size * length);
        free(*inout_memory);
        *inout_memory = new_memory;
        #endif
    }
}

void coexpand(void **inout_memory1, length_t unit_size1, void **inout_memory2, length_t unit_size2, length_t length, length_t *inout_capacity, length_t amount, length_t default_capacity){
    // Expands an array in memory to be able to fit more units

    if(*inout_capacity == 0 && amount != 0){
        *inout_memory1 = malloc(unit_size1 * default_capacity);
        *inout_memory2 = malloc(unit_size2 * default_capacity);
        *inout_capacity = default_capacity;
    }

    while(length + amount > *inout_capacity){
        *inout_capacity *= 2;

        #ifdef UTIL_USE_REALLOC
        *inout_memory1 = realloc(*inout_memory1, unit_size1 * *inout_capacity);
        *inout_memory2 = realloc(*inout_memory2, unit_size2 * *inout_capacity);
        #else
        void *new_memory = malloc(unit_size1 * *inout_capacity);
        memcpy(new_memory, *inout_memory1, unit_size1 * length);
        free(*inout_memory1);
        *inout_memory1 = new_memory;

        new_memory = malloc(unit_size2 * *inout_capacity);
        memcpy(new_memory, *inout_memory2, unit_size2 * length);
        free(*inout_memory2);
        *inout_memory2 = new_memory;
        #endif
    }
}

#ifdef UTIL_USE_REALLOC
void grow_impl(void **inout_memory, length_t unit_size, length_t new_length){
#else
void grow_impl(void **inout_memory, length_t unit_size, length_t old_length, length_t new_length){
#endif
    #ifdef UTIL_USE_REALLOC
    *inout_memory = realloc(*inout_memory, unit_size * new_length);
    #else
    void *memory = malloc(unit_size * new_length);
    memcpy(memory, *inout_memory, unit_size * old_length);
    free(*inout_memory);
    *inout_memory = memory;
    #endif
}

strong_cstr_t strclone(const char *src){
    length_t size = strlen(src) + 1;
    char *clone = malloc(size);
    memcpy(clone, src, size);
    return clone;
}

void freestrs(strong_cstr_t *array, length_t length){
    for(length_t i = 0; i != length; i++) free(array[i]);
    free(array);
}

char *mallocandsprintf(const char *format, ...){
    // TODO: Add support for things other than %s
    char *destination = NULL;
    va_list args;
    va_list transverse;
    va_start(args, format);
    va_copy(transverse, args);
    const char *p = format;
    size_t size = 1;
    while(*p != '\0'){
        if(*(p++) == '%'){
            switch(*(p++)){
            case '%':
                size++;
                break;
            case 's':
                size += strlen(va_arg(transverse, char*));
                break;
            default:
                printf("WARNING: mallocandsprintf() received non %%s in format\n");
                size += 50; // Assume whatever it is, it will fit in 50 bytes
            }
        } else size++;
    }
    va_end(transverse);
    destination = malloc(size);
    vsprintf(destination, format, args);
    va_end(args);
    return destination;
}

strong_cstr_t long_to_string(long int value, weak_cstr_t suffix){
    length_t suffix_length = suffix ? strlen(suffix) : 0;
    strong_cstr_t string = malloc(23 + suffix_length);
    sprintf(string, "%ld", value);

    length_t numeric_length = strlen(string);

    // Don't copy null-terminating byte directly from 'suffix' c-string, because it may be NULL
    memcpy(&string[numeric_length], suffix, suffix_length);
    string[numeric_length + suffix_length] = 0x00;
    return string;
}

strong_cstr_t double_to_string(double value, char suffix){
    strong_cstr_t string = malloc(23);
    sprintf(string, "%06.6ff", value);
    length_t length = strlen(string);
    
    // Trim extra zeros for good measure
    if(length > 1) for(length_t i = length - 2; i > 0; i--){
        if(string[i] != '0' || string[i - 1] == '.') break;
        
        string[i] = suffix;
        string[i + 1] = 0x00;
    }

    return string;
}

strong_cstr_t string_to_escaped_string(char *array, length_t length){
    length_t put_index = 1;
    length_t special_characters = 0;

    // Count number of special characters (\n, \t, \b, etc.)
    for(length_t i = 0; i != length; i++){
        if(array[i] <= 0x1F || array[i] == '\\') special_characters++;
    }

    strong_cstr_t string = malloc(length + special_characters + 3);
    string[0] = '\"';
    
    for(length_t i = 0; i != length; i++){
        if(array[i] <= 0x1F || array[i] == '\\'){
            // Escape special character
            string[put_index++] = '\\';
        } else {
            // Put regular character
        }
        switch(array[i]){
        case '\0': string[put_index++] =  '0'; break;
        case '\t': string[put_index++] =  't'; break;
        case '\n': string[put_index++] =  'n'; break;
        case '\r': string[put_index++] =  'r'; break;
        case '\b': string[put_index++] =  'b'; break;
        case '\e': string[put_index++] =  'e'; break;
        case '\\': string[put_index++] = '\\'; break;
        case '"':  string[put_index++] =  '"'; break;
        default:
            // Unrecognized special character, don't escape
            string[put_index - 1] = array[i];
        }
    }

    string[put_index++] = '"';
    string[put_index++] = '\0';
    return string;
}

length_t string_count_character(weak_cstr_t string, char character){
    return string_modern_count_character(string, strlen(string), character);
}

length_t string_modern_count_character(weak_cstr_t string, length_t length, char character){
    length_t count = 0;
    for(length_t i = 0; i != length; i++) if(string[i] == character) count++;
    return count;
}
