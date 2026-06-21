#ifndef NETLEAF_AUTOROUTE_H
#define NETLEAF_AUTOROUTE_H

#include <stddef.h>
#include "netleaf_module.h"

// DLL export/import macros
#ifdef _WIN32
    #ifdef NL_AUTOROUTE_EXPORTS
        #define NL_AUTOROUTE_API __declspec(dllexport)
    #elif defined(NL_AUTOROUTE_STATIC)
        #define NL_AUTOROUTE_API
    #else
        #define NL_AUTOROUTE_API __declspec(dllimport)
    #endif
#else
    #define NL_AUTOROUTE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Module version - defined at compile time by CMake
#ifdef NL_VERSION
#define NL_AUTOROUTE_VERSION NL_VERSION
#else
#define NL_AUTOROUTE_VERSION "2.2.1"
#endif

#define NL_AUTOROUTE_VERSION_MAJOR 2
#define NL_AUTOROUTE_VERSION_MINOR 2
#define NL_AUTOROUTE_VERSION_PATCH 1

// =========================================
// Module Information
// =========================================

// Autoroute module capabilities
#define NL_AUTOROUTE_CAPABILITIES \
    (NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL)

// Autoroute module info structure (defined in implementation)
NL_AUTOROUTE_API nl_module_info_t* nl_autoroute_get_module_info(void);

// =========================================
// Flexible Enable/Disable API
// =========================================
// Supports multiple input formats:
//   - Integer: 1, 0
//   - String: "true", "false", "on", "off", "yes", "no" (case-insensitive)
//   - String: "1", "0"

// Enable/disable autoroute module
NL_AUTOROUTE_API void nl_autoroute_enable_ex(const char* enable_str);

// Check if autoroute is enabled
NL_AUTOROUTE_API int nl_autoroute_is_enabled(void);

// Enable/disable autoroute module (integer version)
NL_AUTOROUTE_API void nl_autoroute_enable(int enable);

// =========================================
// Module Info API
// =========================================

// Returns 1 if available, 0 if not loaded
NL_AUTOROUTE_API int nl_autoroute_is_available(void);

// Initialize the module (lazy loading)
NL_AUTOROUTE_API int nl_autoroute_init(void);

// Get module version
NL_AUTOROUTE_API const char* nl_autoroute_version(void);

// =========================================
// Route Matcher
// =========================================

typedef struct nl_route_matcher nl_route_matcher_t;

// Suggestion structure
typedef struct {
    char path[256];
    double score;  // 0.0 - 1.0, higher is more similar
} nl_route_suggestion_t;

// Create a route matcher
NL_AUTOROUTE_API nl_route_matcher_t* nl_route_matcher_create(void);

// Add a route to the matcher
NL_AUTOROUTE_API void nl_route_matcher_add_route(nl_route_matcher_t* matcher, const char* path);

// Add multiple routes at once
NL_AUTOROUTE_API void nl_route_matcher_add_routes(nl_route_matcher_t* matcher, const char** paths, int count);

// Destroy the matcher
NL_AUTOROUTE_API void nl_route_matcher_destroy(nl_route_matcher_t* matcher);

// =========================================
// Find Similar Routes
// =========================================

// Find similar routes to the given path
// min_similarity: minimum similarity score (0.0 - 1.0), typically 0.4
// Returns JSON array string like: ["route1", "route2", ...]
// Caller must free the returned string
NL_AUTOROUTE_API char* nl_route_matcher_find_similar(nl_route_matcher_t* matcher, 
                                    const char* path, 
                                    double min_similarity);

// Get suggestions as structured array
// Returns number of suggestions found (up to max_count)
NL_AUTOROUTE_API int nl_route_matcher_get_suggestions(nl_route_matcher_t* matcher,
                                      const char* path,
                                      nl_route_suggestion_t* suggestions,
                                      int max_count);

// =========================================
// Route Similarity Calculation
// =========================================

// Calculate similarity between two paths
// Returns value between 0.0 (completely different) and 1.0 (identical)
NL_AUTOROUTE_API double nl_route_similarity(const char* path1, const char* path2);

// Levenshtein distance between two strings
NL_AUTOROUTE_API int nl_levenshtein_distance(const char* s1, const char* s2);

// =========================================
// Quick Helper Functions
// =========================================

// One-shot: find best matching route from a list
// Returns the best match or NULL if none above min_similarity
NL_AUTOROUTE_API const char* nl_find_best_route(const char* target, const char** routes, int count, double min_similarity);

// Check if a route pattern matches a path
// Supports wildcards: * matches any characters, ** matches path segments
NL_AUTOROUTE_API int nl_route_matches(const char* pattern, const char* path);

// =========================================
// Global Route Matcher (for web server integration)
// =========================================

NL_AUTOROUTE_API nl_route_matcher_t* nl_autoroute_get_global_matcher(void);
NL_AUTOROUTE_API void nl_autoroute_set_global_matcher(nl_route_matcher_t* matcher);

// Auto-discover routes from web server and register them
NL_AUTOROUTE_API void nl_autoroute_discover_routes(void);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_AUTOROUTE_H
