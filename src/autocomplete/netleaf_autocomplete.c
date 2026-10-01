#include "netleaf_autocomplete.h"
#include "netleaf_module.h"
#include "netleaf_autocomplete_lang.h"
#include "nl_util.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/*
 * Cross-platform strdup / 大小写无关比较 / parse_enable 已收敛至公共工具层
 * nl_util：本文件不再本地重复定义，改为直接调用 nl_util 中的对应实现。
 */

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
    .author = "508364",
    .next = NULL
};

nl_module_info_t* nl_autocomplete_get_module_info(void) {
    return &g_autocomplete_module_info;
}

// =========================================
// Flexible Enable/Disable Helper
// =========================================

static int parse_enable_value(const char* value) {
    return nl_parse_enable_value(value);
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
        NL_AUTOCOMPLETE_REGISTER_LANG();
    }
    return g_autocomplete_available;
}

const char* nl_autocomplete_version(void) {
    return NL_AUTOCOMPLETE_VERSION;
}

// =========================================
// Extension Definition (for dynamic loading)
// =========================================

NL_EXTENSION_DEFINE(autocomplete, "AutoComplete", NL_AUTOCOMPLETE_VERSION, "508364",
    "Auto-completion for charset and Vue imports",
    "Windows,Linux,MacOS",
    NL_CAP_THREAD_SAFE,
    nl_autocomplete_init, NULL, nl_autocomplete_is_available, nl_autocomplete_version);

nl_extension_info_t* nl_autocomplete_get_extension_info(void) {
    return &nl_extension_info_autocomplete;
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
// Helper Functions（收敛至公共工具层 nl_util）
// =========================================

int nl_autocomplete_strncasecmp(const char* s1, const char* s2, size_t n) {
    return nl_strncasecmp(s1, s2, n);
}

char* nl_autocomplete_strncasestr(const char* haystack, const char* needle, size_t len) {
    return nl_strncasestr(haystack, needle, len);
}
