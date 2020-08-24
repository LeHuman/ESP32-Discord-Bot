#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char JSON_Key[] = "\"%s\":";
static const char JSON_String[] = "\"%s\"";
static const char *JSON_Array_Open = "[";
static const char *JSON_Array_Close = "]";
static const char *JSON_List_Open = "{";
static const char *JSON_List_Close = "}";
static const char *JSON_Comma = ",";

// IMPROVE: should these string functions accept buffers instead?

// Append string by redefining string pointer
static inline void str_append(char **string, const char *append) {
    char *new_str = calloc(1, strlen(*string) + strlen(append) + 1);
    strcat(new_str, *string);
    strcat(new_str, append);
    free(*string);
    *string = new_str;
}

// Single parameter string format by redefining string pointer
static inline void pfstr(char **string, const char *pattern, const char *value) {
    int len = strlen(pattern) + strlen(value) + 1;
    char *new_str = calloc(1, len);
    snprintf(new_str, len, pattern, value);
    free(*string);
    *string = new_str;
}

// Append a single parameter formatted string by redefining string pointer
static inline void pfstr_append(char **string, const char *pattern, const char *value) {
    char *pf_str = malloc(0);
    pfstr(&pf_str, pattern, value);
    str_append(string, pf_str);
    free(pf_str);
}

typedef struct json_object {
    char *string;
    // bool expecting_key; // IMPROVE: error checking json building
    // bool expecting_value;
    // bool finished;
    unsigned int addComma; // massive jsons would have issues but idc
} json_object_t;

extern json_object_t json_init() {
    json_object_t obj = {
        .string = strdup(JSON_List_Open), // Start Json
        // .expecting_key = true,
        // .expecting_value = false,
        // .finished = false,
        .addComma = 0,
    };
    return obj;
}

extern char *json_finish(json_object_t *json) {
    str_append(&json->string, JSON_List_Close);
    // json->finished = false;
    return json->string;
}

extern void json_open_array(json_object_t *json) {
    json->addComma <<= 1;
    str_append(&json->string, JSON_Array_Open);
}

extern void json_close_array(json_object_t *json) {
    json->addComma >>= 1;
    str_append(&json->string, JSON_Array_Close);
}

extern void json_open_list(json_object_t *json) {
    json->addComma <<= 1;
    str_append(&json->string, JSON_List_Open);
}

extern void json_close_list(json_object_t *json) {
    json->addComma >>= 1;
    str_append(&json->string, JSON_List_Close);
}

extern void json_value(json_object_t *json, const char *key) {
    str_append(&json->string, key);
}

extern void json_string(json_object_t *json, const char *key) {
    int len = strlen(JSON_String) + strlen(key) + 1;
    char *str = calloc(1, len);
    snprintf(str, len, JSON_String, key);
    json_value(json, str);
    free(str);
}

extern void json_key(json_object_t *json, const char *key) {
    if (json->addComma & 1) {
        str_append(&json->string, JSON_Comma);
    } else {
        json->addComma |= 1;
    }
    pfstr_append(&json->string, JSON_Key, key);
}