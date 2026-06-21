#ifndef NETLEAF_AUTOCOMPLETE_H
#define NETLEAF_AUTOCOMPLETE_H

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

// DLL export/import macros
#ifdef _WIN32
    #ifdef NL_AUTOCOMPLETE_EXPORTS
        #define NL_AUTOCOMPLETE_API __declspec(dllexport)
    #elif defined(NL_AUTOCOMPLETE_STATIC)
        #define NL_AUTOCOMPLETE_API
    #else
        #define NL_AUTOCOMPLETE_API __declspec(dllimport)
    #endif
#else
    #define NL_AUTOCOMPLETE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Module version - defined at compile time by CMake
#ifdef NL_VERSION
#define NL_AUTOCOMPLETE_VERSION NL_VERSION
#else
#define NL_AUTOCOMPLETE_VERSION "2.2.1"
#endif

#define NL_AUTOCOMPLETE_VERSION_MAJOR 2
#define NL_AUTOCOMPLETE_VERSION_MINOR 2
#define NL_AUTOCOMPLETE_VERSION_PATCH 1

// =========================================
// Module Information
// =========================================

// Autocomplete module capabilities
#define NL_AUTOCOMPLETE_CAPABILITIES \
    (NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL)

// Autocomplete module info structure (defined in implementation)
NL_AUTOCOMPLETE_API nl_module_info_t* nl_autocomplete_get_module_info(void);

// =========================================
// Internal Helper Functions
// =========================================
// Case-insensitive string comparison within length limit
NL_AUTOCOMPLETE_API int nl_autocomplete_strncasecmp(const char* s1, const char* s2, size_t n);
// Case-insensitive string search within length limit
NL_AUTOCOMPLETE_API char* nl_autocomplete_strncasestr(const char* haystack, const char* needle, size_t len);

// =========================================
// Flexible Enable/Disable API
// =========================================
// Supports multiple input formats:
//   - Integer: 1, 0
//   - String: "true", "false", "on", "off", "yes", "no" (case-insensitive)
//   - String: "1", "0"

// Enable/disable autocomplete module
// Supports: int enable, const char* enable_str
NL_AUTOCOMPLETE_API void nl_autocomplete_enable_ex(const char* enable_str);

// Check if autocomplete is enabled
NL_AUTOCOMPLETE_API int nl_autocomplete_is_enabled(void);

// Enable/disable autocomplete module (integer version)
NL_AUTOCOMPLETE_API void nl_autocomplete_enable(int enable);

// =========================================
// Feature Toggle API
// =========================================

typedef enum {
    NL_AUTOCOMPLETE_FEATURE_NONE = 0,
    NL_AUTOCOMPLETE_FEATURE_CHARSET = 1,      // Auto-add charset/viewport meta tags
    NL_AUTOCOMPLETE_FEATURE_VUE = 2,           // Auto-import Vue library
    NL_AUTOCOMPLETE_FEATURE_ALL = 3           // All features
} nl_autocomplete_feature_t;

// Enable/disable specific feature
// Supports string or integer input
NL_AUTOCOMPLETE_API void nl_autocomplete_enable_feature_ex(nl_autocomplete_feature_t feature, const char* enable_str);
NL_AUTOCOMPLETE_API void nl_autocomplete_enable_feature(nl_autocomplete_feature_t feature);
NL_AUTOCOMPLETE_API void nl_autocomplete_disable_feature(nl_autocomplete_feature_t feature);

// Check if specific feature is enabled
NL_AUTOCOMPLETE_API int nl_autocomplete_is_feature_enabled(nl_autocomplete_feature_t feature);

// =========================================
// Module Info API
// =========================================

// Returns 1 if available (loaded), 0 if not
NL_AUTOCOMPLETE_API int nl_autocomplete_is_available(void);

// Initialize the module (lazy loading)
NL_AUTOCOMPLETE_API int nl_autocomplete_init(void);

// Get module version
NL_AUTOCOMPLETE_API const char* nl_autocomplete_version(void);

// =========================================
// Charset Auto-complete
// =========================================
// Automatically adds charset and viewport meta tags to HTML/Vue if missing

typedef struct nl_charset_complete nl_charset_complete_t;

// Create a charset completer context
NL_AUTOCOMPLETE_API nl_charset_complete_t* nl_charset_complete_create(const char* html, size_t len);

// Process the HTML and add charset/viewport tags if missing
// Uses the global encoding if encoding is NULL (set via nl_autocomplete_set_encoding)
NL_AUTOCOMPLETE_API char* nl_charset_complete_process(nl_charset_complete_t* ctx, const char* encoding);

// Destroy the completer context
NL_AUTOCOMPLETE_API void nl_charset_complete_destroy(nl_charset_complete_t* ctx);

// Quick function: complete charset in HTML string
NL_AUTOCOMPLETE_API char* nl_complete_charset(const char* html, size_t html_len, const char* encoding);

// Set the default encoding for charset completion
NL_AUTOCOMPLETE_API void nl_autocomplete_set_encoding(const char* encoding);

// Get the current default encoding
NL_AUTOCOMPLETE_API const char* nl_autocomplete_get_encoding(void);

// =========================================
// Vue Auto-import
// =========================================
// Automatically imports Vue library if Vue code is detected but no Vue import exists

typedef struct nl_vue_import nl_vue_import_t;

typedef enum {
    NL_VUE_CDN_UNPKG = 0,      // unpkg.com (default)
    NL_VUE_CDN_CDNJS = 1,      // cdnjs.cloudflare.com
    NL_VUE_CDN_JSDELIVR = 2,   // cdn.jsdelivr.net
    NL_VUE_CDN_LOCAL = 3        // Local file (./vue.global.js)
} nl_vue_cdn_type_t;

// Create a Vue import context
NL_AUTOCOMPLETE_API nl_vue_import_t* nl_vue_import_create(const char* html, size_t len);

// Process HTML and add Vue import if Vue code is detected but no import exists
// Returns newly allocated string (caller must free)
NL_AUTOCOMPLETE_API char* nl_vue_import_process(nl_vue_import_t* ctx, nl_vue_cdn_type_t cdn_type);

// Process with specific Vue version
NL_AUTOCOMPLETE_API char* nl_vue_import_process_version(nl_vue_import_t* ctx, nl_vue_cdn_type_t cdn_type, const char* version);

// Destroy the import context
NL_AUTOCOMPLETE_API void nl_vue_import_destroy(nl_vue_import_t* ctx);

// Quick function: add Vue import to HTML string
NL_AUTOCOMPLETE_API char* nl_import_vue(const char* html, size_t html_len, nl_vue_cdn_type_t cdn_type, const char* version);

// Check if HTML contains Vue code (looks for vue-specific patterns)
NL_AUTOCOMPLETE_API int nl_html_has_vue_code(const char* html, size_t html_len);

// Check if HTML already has Vue import
NL_AUTOCOMPLETE_API int nl_html_has_vue_import(const char* html, size_t html_len);

// =========================================
// Web Server Integration
// =========================================
// These functions are called automatically by the web server
// They check if the feature is enabled before processing

// Process HTML/Vue content with all enabled autocomplete features
// This is the main entry point for web server integration
NL_AUTOCOMPLETE_API char* nl_autocomplete_process_html(const char* html, size_t len, const char* encoding);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_AUTOCOMPLETE_H
