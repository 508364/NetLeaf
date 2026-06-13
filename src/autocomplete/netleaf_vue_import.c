#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "netleaf_autocomplete.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define strdup _strdup
#endif

// =========================================
// Vue Import Implementation
// =========================================
// This file contains the Vue auto-import functionality
// which automatically adds Vue library import when Vue code is detected

// Vue import patterns to detect
static const char* g_vue_import_patterns[] = {
    "vue.js", "vue.min.js", "vue.global.js", "vue.runtime.js",
    "cdn.jsdelivr.net/npm/vue", "unpkg.com/vue@", 
    "cdnjs.cloudflare.com/ajax/libs/vue/", "vuejs.org/"
};

// Vue code patterns to detect
static const char* g_vue_code_patterns[] = {
    "new Vue(", "Vue.createApp", "createApp(",
    "ref(", "reactive(", "computed(",
    "watch(", "watchEffect(",
    "onMounted(", "onUpdated(", "onUnmounted(",
    "v-if", "v-for", "v-show", "v-bind", "v-model",
    "{{", "}}",
    ":class", ":style", "@click", "@submit",
    "vue-app", "id=\"app\""
};

struct nl_vue_import {
    char* html;
    size_t html_len;
    int has_vue_code;
    int has_vue_import;
};

static const char* get_vue_cdn_url(nl_vue_cdn_type_t cdn_type, const char* version) {
    static char url[256];
    if (!version) version = "3.4.0";
    
    switch (cdn_type) {
        case NL_VUE_CDN_UNPKG:
            snprintf(url, sizeof(url), "https://unpkg.com/vue@%s/dist/vue.global.js", version);
            break;
        case NL_VUE_CDN_CDNJS:
            snprintf(url, sizeof(url), "https://cdnjs.cloudflare.com/ajax/libs/vue/%s/vue.min.js", version);
            break;
        case NL_VUE_CDN_JSDELIVR:
            snprintf(url, sizeof(url), "https://cdn.jsdelivr.net/npm/vue@%s/dist/vue.global.js", version);
            break;
        case NL_VUE_CDN_LOCAL:
            snprintf(url, sizeof(url), "./vue.global.js");
            break;
        default:
            snprintf(url, sizeof(url), "https://unpkg.com/vue@%s/dist/vue.global.js", version);
            break;
    }
    return url;
}

int nl_html_has_vue_import(const char* html, size_t html_len) {
    if (!html || html_len == 0) return 0;
    
    for (size_t i = 0; i < sizeof(g_vue_import_patterns)/sizeof(g_vue_import_patterns[0]); i++) {
        if (nl_autocomplete_strncasestr(html, g_vue_import_patterns[i], html_len)) {
            return 1;
        }
    }
    return 0;
}

int nl_html_has_vue_code(const char* html, size_t html_len) {
    if (!html || html_len == 0) return 0;
    
    int match_count = 0;
    for (size_t i = 0; i < sizeof(g_vue_code_patterns)/sizeof(g_vue_code_patterns[0]); i++) {
        if (nl_autocomplete_strncasestr(html, g_vue_code_patterns[i], html_len)) {
            match_count++;
            if (match_count >= 2) return 1;
        }
    }
    return 0;
}

nl_vue_import_t* nl_vue_import_create(const char* html, size_t len) {
    if (!html || len == 0) return NULL;
    
    nl_vue_import_t* ctx = calloc(1, sizeof(nl_vue_import_t));
    if (!ctx) return NULL;
    
    ctx->html = malloc(len + 1);
    if (!ctx->html) {
        free(ctx);
        return NULL;
    }
    
    memcpy(ctx->html, html, len);
    ctx->html[len] = '\0';
    ctx->html_len = len;
    ctx->has_vue_code = nl_html_has_vue_code(html, len);
    ctx->has_vue_import = nl_html_has_vue_import(html, len);
    
    return ctx;
}

char* nl_vue_import_process(nl_vue_import_t* ctx, nl_vue_cdn_type_t cdn_type) {
    return nl_vue_import_process_version(ctx, cdn_type, "3.4.0");
}

char* nl_vue_import_process_version(nl_vue_import_t* ctx, nl_vue_cdn_type_t cdn_type, const char* version) {
    if (!ctx || !ctx->html) return NULL;
    
    // Only add Vue import if there's Vue code but no import
    if (!ctx->has_vue_code || ctx->has_vue_import) {
        return strdup(ctx->html);
    }
    
    const char* url = get_vue_cdn_url(cdn_type, version);
    char script[512];
    int script_len = snprintf(script, sizeof(script), "<script src=\"%s\"></script>\n", url);
    
    // Find insertion point: before </head>, </body>, or at end
    char* head_end = nl_autocomplete_strncasestr(ctx->html, "</head", ctx->html_len);
    char* body_end = nl_autocomplete_strncasestr(ctx->html, "</body", ctx->html_len);
    char* inject = head_end ? head_end : (body_end ? body_end : ctx->html + ctx->html_len);
    
    size_t prefix = (size_t)(inject - ctx->html);
    size_t new_len = ctx->html_len + script_len;
    char* result = malloc(new_len + 1);
    if (!result) return NULL;
    
    memcpy(result, ctx->html, prefix);
    memcpy(result + prefix, script, script_len);
    memcpy(result + prefix + script_len, inject, ctx->html_len - prefix);
    result[new_len] = '\0';
    
    return result;
}

void nl_vue_import_destroy(nl_vue_import_t* ctx) {
    if (ctx) {
        free(ctx->html);
        free(ctx);
    }
}

char* nl_import_vue(const char* html, size_t html_len, nl_vue_cdn_type_t cdn_type, const char* version) {
    if (!html || html_len == 0) return NULL;
    
    nl_vue_import_t* ctx = nl_vue_import_create(html, html_len);
    if (!ctx) return NULL;
    
    char* result = nl_vue_import_process_version(ctx, cdn_type, version);
    nl_vue_import_destroy(ctx);
    return result;
}

// =========================================
// Web Server Integration
// =========================================

char* nl_autocomplete_process_html(const char* html, size_t len, const char* encoding) {
    if (!html || len == 0) return NULL;
    if (!nl_autocomplete_is_enabled()) return NULL;
    
    char* result = NULL;
    
    // Step 1: Charset completion
    if (nl_autocomplete_is_feature_enabled(NL_AUTOCOMPLETE_FEATURE_CHARSET)) {
        result = nl_complete_charset(html, len, encoding);
        if (result) {
            size_t result_len = strlen(result);
            
            // Step 2: Vue import
            if (nl_autocomplete_is_feature_enabled(NL_AUTOCOMPLETE_FEATURE_VUE)) {
                char* vue_result = nl_import_vue(result, result_len, NL_VUE_CDN_UNPKG, "3.4.0");
                free(result);
                result = vue_result;
            }
        }
    } else if (nl_autocomplete_is_feature_enabled(NL_AUTOCOMPLETE_FEATURE_VUE)) {
        result = nl_import_vue(html, len, NL_VUE_CDN_UNPKG, "3.4.0");
    } else {
        return NULL;
    }
    
    return result;
}
