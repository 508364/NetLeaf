#include "netleaf_autocomplete.h"
#include "netleaf_module.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// =========================================
// Cross-platform strdup implementation
// =========================================
#ifdef _WIN32
#define strdup _strdup
#endif

// =========================================
// Module State
// =========================================

static int g_autocomplete_available = 0;
static int g_autocomplete_enabled = 0;
static int g_feature_mask = NL_AUTOCOMPLETE_FEATURE_ALL;
static char g_default_encoding[32] = "UTF-8";

// =========================================
// Module Info
// =========================================

static nl_module_info_t g_autocomplete_module_info = {
    .type = NL_MODULE_AUTOCOMPLETE,
    .name = "autocomplete",
    .version = NL_AUTOCOMPLETE_VERSION,
    .capabilities = NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    .status = NL_MODULE_STATUS_UNINITIALIZED,
    .platform_windows = 1,
    .platform_linux = 1,
    .platform_macos = 1,
    .init = nl_autocomplete_init,
    .shutdown = NULL,
    .is_available = nl_autocomplete_is_available,
    .get_version = nl_autocomplete_version,
    .description = "Auto-completion for charset and Vue imports",
    .author = "NetLeaf Team",
    .next = NULL
};

nl_module_info_t* nl_autocomplete_get_module_info(void) {
    return &g_autocomplete_module_info;
}

// =========================================
// Flexible Enable/Disable Helper
// =========================================

static int parse_enable_value(const char* value) {
    if (!value) return 0;
    
    if (strcmp(value, "1") == 0 || strcmp(value, "0") == 0) {
        return atoi(value);
    }
    
    char lower[16];
    size_t i;
    for (i = 0; i < sizeof(lower) - 1 && value[i]; i++) {
        lower[i] = tolower((unsigned char)value[i]);
    }
    lower[i] = '\0';
    
    if (strcmp(lower, "true") == 0 || strcmp(lower, "on") == 0 || 
        strcmp(lower, "yes") == 0) {
        return 1;
    }
    if (strcmp(lower, "false") == 0 || strcmp(lower, "off") == 0 || 
        strcmp(lower, "no") == 0) {
        return 0;
    }
    
    return 0;
}

// =========================================
// Module Info API
// =========================================

int nl_autocomplete_is_available(void) {
    return g_autocomplete_available;
}

int nl_autocomplete_init(void) {
    if (!g_autocomplete_available) {
        g_autocomplete_available = 1;
        g_autocomplete_enabled = 1;
        g_feature_mask = NL_AUTOCOMPLETE_FEATURE_ALL;
    }
    return g_autocomplete_available;
}

const char* nl_autocomplete_version(void) {
    return NL_AUTOCOMPLETE_VERSION;
}

// =========================================
// Enable/Disable API
// =========================================

void nl_autocomplete_enable_ex(const char* enable_str) {
    g_autocomplete_enabled = parse_enable_value(enable_str);
}

void nl_autocomplete_enable(int enable) {
    g_autocomplete_enabled = enable ? 1 : 0;
}

int nl_autocomplete_is_enabled(void) {
    return g_autocomplete_enabled;
}

// =========================================
// Feature Toggle API
// =========================================

void nl_autocomplete_enable_feature_ex(nl_autocomplete_feature_t feature, const char* enable_str) {
    int enable = parse_enable_value(enable_str);
    if (enable) {
        g_feature_mask |= feature;
    } else {
        g_feature_mask &= ~feature;
    }
}

void nl_autocomplete_enable_feature(nl_autocomplete_feature_t feature) {
    g_feature_mask |= feature;
}

void nl_autocomplete_disable_feature(nl_autocomplete_feature_t feature) {
    g_feature_mask &= ~feature;
}

int nl_autocomplete_is_feature_enabled(nl_autocomplete_feature_t feature) {
    return (g_feature_mask & feature) != 0;
}

// =========================================
// Encoding API
// =========================================

void nl_autocomplete_set_encoding(const char* encoding) {
    if (encoding) {
        strncpy(g_default_encoding, encoding, sizeof(g_default_encoding) - 1);
        g_default_encoding[sizeof(g_default_encoding) - 1] = '\0';
    }
}

const char* nl_autocomplete_get_encoding(void) {
    return g_default_encoding;
}

// =========================================
// Helper Functions
// =========================================

static int nl_autocomplete_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

int nl_autocomplete_strncasecmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0 && *s1 && *s2) {
        int diff = nl_autocomplete_tolower((unsigned char)*s1) - nl_autocomplete_tolower((unsigned char)*s2);
        if (diff != 0) return diff;
        s1++;
        s2++;
    }
    return 0;
}

char* nl_autocomplete_strncasestr(const char* haystack, const char* needle, size_t len) {
    if (!haystack || !needle || needle[0] == '\0') return NULL;
    size_t needle_len = strlen(needle);
    if (needle_len > len) return NULL;
    const char* end = haystack + len - needle_len;
    for (const char* p = haystack; p <= end; p++) {
        if (nl_autocomplete_strncasecmp(p, needle, needle_len) == 0) {
            return (char*)p;
        }
    }
    return NULL;
}
