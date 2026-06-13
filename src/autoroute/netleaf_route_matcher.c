#include "netleaf_autoroute.h"
#include <stdlib.h>
#include <string.h>

// Route matcher implementation helpers
// Note: struct nl_route_matcher and struct nl_route_node
// are defined in netleaf_autoroute.c

// Split path into segments
// Reserved for future standalone library API
#ifdef __GNUC__
__attribute__((unused))
#endif
static void split_path(const char* path, char segments[][128], int* count) {
    *count = 0;
    if (!path) return;
    
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    
    char* token = strtok(tmp, "/");
    while (token && *count < 32) {
        strncpy(segments[*count], token, 127);
        segments[*count][127] = '\0';
        (*count)++;
        token = strtok(NULL, "/");
    }
}

// Internal add route implementation
// Note: This is duplicated in netleaf_autoroute.c - prefer that version
// This file is kept for potential future extraction
