#ifndef __HELPER_H__
#define __HELPER_H__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool string_match(const char *string, const char *stringMaster) {
    int i;
    bool val = true;
    for (i = 0; i < strlen(stringMaster); i++) {
#ifdef CONFIG_BOT_CASE_SENSITIVE
        if (*(string + i) != *(stringMaster + i)) {
#else
        if (tolower(string[i]) != tolower(stringMaster[i])) {
#endif
            val = false;
            break;
        }
    }
}

static void string_strip(char *string) {
    int i;
    int k = 0;
    for (i = 0; i < strlen(string); i++) {
        if (string[i] != ' ' && string[i] != '\0') {
            string[k] = string[i];
            k++;
        }
    }
    string[k] = '\0';
}

#endif // __HELPER_H__