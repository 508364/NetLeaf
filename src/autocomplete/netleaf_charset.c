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
// Charset Complete Implementation
// =========================================
// This file contains the charset auto-complete functionality
// which automatically adds charset and viewport meta tags to HTML

struct nl_charset_complete {
    char* html;
    size_t html_len;
    int has_charset;
    int has_viewport;
};

static int check_html_has_charset(const char* html, size_t len) {
    if (!html || len == 0) return 0;
    const char* p = html;
    size_t remaining = len;
    
    while (remaining > 0) {
        const char* tag_start = memchr(p, '<', remaining);
        if (!tag_start) break;
        
        if (remaining >= 5 && nl_autocomplete_strncasecmp(tag_start + 1, "meta", 4) == 0) {
            const char* meta_end = tag_start + 5;
            size_t meta_len = remaining - (size_t)(meta_end - tag_start);
            size_t check_len = meta_len > 100 ? 100 : meta_len;
            
            if (nl_autocomplete_strncasestr(meta_end, "charset", check_len)) return 1;
            if (nl_autocomplete_strncasestr(meta_end, "http-equiv", check_len) && 
                nl_autocomplete_strncasestr(meta_end, "Content-Type", check_len)) return 1;
        }
        
        size_t consumed = (size_t)(tag_start - p + 1);
        p = tag_start + 1;
        remaining -= consumed;
    }
    return 0;
}

static int check_html_has_viewport(const char* html, size_t len) {
    if (!html || len == 0) return 0;
    const char* p = html;
    size_t remaining = len;
    
    while (remaining > 0) {
        const char* tag_start = memchr(p, '<', remaining);
        if (!tag_start) break;
        
        if (remaining >= 5 && nl_autocomplete_strncasecmp(tag_start + 1, "meta", 4) == 0) {
            const char* meta_end = tag_start + 5;
            size_t meta_len = remaining - (size_t)(meta_end - tag_start);
            size_t check_len = meta_len > 100 ? 100 : meta_len;
            
            if (nl_autocomplete_strncasestr(meta_end, "viewport", check_len)) return 1;
        }
        
        size_t consumed = (size_t)(tag_start - p + 1);
        p = tag_start + 1;
        remaining -= consumed;
    }
    return 0;
}

nl_charset_complete_t* nl_charset_complete_create(const char* html, size_t len) {
    if (!html || len == 0) return NULL;
    
    nl_charset_complete_t* ctx = calloc(1, sizeof(nl_charset_complete_t));
    if (!ctx) return NULL;
    
    ctx->html = malloc(len + 1);
    if (!ctx->html) {
        free(ctx);
        return NULL;
    }
    
    memcpy(ctx->html, html, len);
    ctx->html[len] = '\0';
    ctx->html_len = len;
    ctx->has_charset = check_html_has_charset(html, len);
    ctx->has_viewport = check_html_has_viewport(html, len);
    
    return ctx;
}

char* nl_charset_complete_process(nl_charset_complete_t* ctx, const char* encoding) {
    if (!ctx || !ctx->html) return NULL;
    
    // Use provided encoding or fall back to global default
    if (!encoding || encoding[0] == '\0') {
        encoding = nl_autocomplete_get_encoding();
    }
    
    // If charset already exists and viewport exists, return copy
    if (ctx->has_charset && ctx->has_viewport) {
        return strdup(ctx->html);
    }
    
    // Find injection point - prefer after <head>, then <html>
    char* inject = NULL;
    char* head = nl_autocomplete_strncasestr(ctx->html, "<head", ctx->html_len);
    char* html_tag = nl_autocomplete_strncasestr(ctx->html, "<html", ctx->html_len);
    
    if (head) {
        inject = head + 5;
    } else if (html_tag) {
        inject = html_tag + 6;
    } else {
        inject = ctx->html;
    }
    
    // Build meta tags
    char meta[512];
    int meta_len = 0;
    
    if (!ctx->has_charset) {
        meta_len += snprintf(meta + meta_len, sizeof(meta) - meta_len,
            "<meta charset=\"%s\">\n", encoding);
    }
    if (!ctx->has_viewport) {
        meta_len += snprintf(meta + meta_len, sizeof(meta) - meta_len,
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n");
    }
    
    if (meta_len == 0) {
        return strdup(ctx->html);
    }
    
    size_t prefix = (size_t)(inject - ctx->html);
    size_t new_len = ctx->html_len + meta_len;
    char* result = malloc(new_len + 1);
    if (!result) return NULL;
    
    memcpy(result, ctx->html, prefix);
    memcpy(result + prefix, meta, meta_len);
    memcpy(result + prefix + meta_len, inject, ctx->html_len - prefix);
    result[new_len] = '\0';
    
    return result;
}

void nl_charset_complete_destroy(nl_charset_complete_t* ctx) {
    if (ctx) {
        free(ctx->html);
        free(ctx);
    }
}

char* nl_complete_charset(const char* html, size_t html_len, const char* encoding) {
    if (!html || html_len == 0) return NULL;
    
    nl_charset_complete_t* ctx = nl_charset_complete_create(html, html_len);
    if (!ctx) return NULL;
    
    char* result = nl_charset_complete_process(ctx, encoding);
    nl_charset_complete_destroy(ctx);
    return result;
}
