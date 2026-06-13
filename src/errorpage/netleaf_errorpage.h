#ifndef NETLEAF_ERRORPAGE_H
#define NETLEAF_ERRORPAGE_H

#include <stddef.h>

// DLL export/import macros
#ifdef _WIN32
    #ifdef NL_ERRORPAGE_EXPORTS
        #define NL_ERRORPAGE_API __declspec(dllexport)
    #elif defined(NL_ERRORPAGE_STATIC)
        #define NL_ERRORPAGE_API
    #else
        #define NL_ERRORPAGE_API __declspec(dllimport)
    #endif
#else
    #define NL_ERRORPAGE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Module version - defined at compile time by CMake
#ifdef NL_VERSION
#define NL_ERRORPAGE_VERSION NL_VERSION
#else
#define NL_ERRORPAGE_VERSION "2.2.0"
#endif

// =========================================
// Error Page Module - IMPORTANT
// =========================================
// This module is INDEPENDENT and standalone.
// If this module is not loaded/available, error page features will NOT work.
// Other modules CANNOT use this module's functionality.
// =========================================

// =========================================
// Flexible Enable/Disable API
// =========================================
// Supports:
//   - Integer: 1, 0
//   - String: "true", "false", "on", "off", "yes", "no" (case-insensitive)

NL_ERRORPAGE_API void nl_errorpage_enable_ex(const char* enable_str);
NL_ERRORPAGE_API int nl_errorpage_is_enabled(void);
NL_ERRORPAGE_API void nl_errorpage_enable(int enable);

// =========================================
// Module Info API
// =========================================

NL_ERRORPAGE_API int nl_errorpage_is_available(void);
NL_ERRORPAGE_API int nl_errorpage_init(void);
NL_ERRORPAGE_API const char* nl_errorpage_version(void);

// =========================================
// Required Variables for Custom Error Pages
// =========================================
// IMPORTANT: Custom error page templates MUST use the unified variable format:
//   {{<var>ERROR_CODE</var>}}     - HTTP status code (e.g., "404")
//   {{<var>ERROR_MESSAGE</var>}}  - Human-readable message (e.g., "Not Found")
//   {{<var>REQUESTED_PATH</var>}} - The path that caused the error
//   {{<var>SUGGESTION</var>}}     - Route suggestion (if available)
//   {{<var>SERVER_VERSION</var>}} - Server version string
//   {{<var>TIMESTAMP</var>}}      - Error timestamp

// =========================================
// Error Page Variables Structure
// =========================================

typedef struct {
    int status_code;
    const char* error_message;
    const char* requested_path;
    const char* suggestion;
    const char* server_version;
    const char* timestamp;
} nl_errorpage_vars_t;

// =========================================
// Template Loading and Validation
// =========================================

NL_ERRORPAGE_API int nl_errorpage_load_template(int status_code, const char* template_path);
NL_ERRORPAGE_API int nl_errorpage_set_template(int status_code, const char* template_content);
NL_ERRORPAGE_API const char* nl_errorpage_get_template(int status_code);
NL_ERRORPAGE_API int nl_errorpage_validate_template(const char* template_content);

// =========================================
// Error Page Rendering
// =========================================

NL_ERRORPAGE_API char* nl_errorpage_render(int status_code, nl_errorpage_vars_t* vars);
NL_ERRORPAGE_API char* nl_errorpage_render_default(int status_code, nl_errorpage_vars_t* vars);

// =========================================
// HTTP Response Generation
// =========================================

NL_ERRORPAGE_API char* nl_errorpage_make_response(int status_code, nl_errorpage_vars_t* vars);
NL_ERRORPAGE_API char* nl_errorpage_make_response_custom(int status_code, nl_errorpage_vars_t* vars, 
                                         const char* template_content);

// =========================================
// Quick Helper Functions
// =========================================

NL_ERRORPAGE_API char* nl_errorpage_quick_response(int status_code, const char* message, const char* path);
NL_ERRORPAGE_API char* nl_errorpage_404_with_suggestion(const char* path, const char* suggestion);

// =========================================
// Status Code Helpers
// =========================================

NL_ERRORPAGE_API const char* nl_errorpage_status_message(int status_code);
NL_ERRORPAGE_API int nl_errorpage_is_cacheable(int status_code);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_ERRORPAGE_H