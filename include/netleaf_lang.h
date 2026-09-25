#ifndef NETLEAF_LANG_H
#define NETLEAF_LANG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

// DLL export/import macros
#ifdef _WIN32
    #ifdef NL_LANG_EXPORTS
        #define NL_LANG_API __declspec(dllexport)
    #elif defined(NL_LANG_STATIC)
        #define NL_LANG_API
    #else
        #define NL_LANG_API __declspec(dllimport)
    #endif
#else
    #define NL_LANG_API
#endif

// =========================================
// Language Code Format (xx_xx, case-insensitive)
// =========================================
// Format: language_country (2-3 letters + underscore + 2-3 letters)
// All input is normalized to lowercase internally
// Examples: en_us, ZH_CN, Ja_JP, ko_kr, ES_ES, fr_fr, Pt_BR, ru_ru, AR_SA
// Valid: EN_US (normalized to "en_us"), Zh_Cn (normalized to "zh_cn")
// Invalid: en, en-US, chinese, en__us (double underscore)

// Maximum language code length
#define NL_LANG_CODE_MAX_LEN 16

// Validate language code format (must be xx_xx)
NL_LANG_API int nl_lang_validate_code(const char* code);

// =========================================
// Language Configuration
// =========================================

// Get current language code (e.g., "en_us", "zh_cn")
NL_LANG_API const char* nl_lang_get(void);

// Set language by code (must be xx_xx format)
// Returns 0 on success, -1 if format is invalid
NL_LANG_API int nl_lang_set(const char* code);

// Get default language code
NL_LANG_API const char* nl_lang_get_default(void);

// Set default language code
NL_LANG_API int nl_lang_set_default(const char* code);

// Get all registered language codes
// Returns array of strings, terminated by NULL
NL_LANG_API const char** nl_lang_get_all(void);

// Get number of registered languages
NL_LANG_API int nl_lang_get_count(void);

// =========================================
// Error Message Structure (Multi-language)
// =========================================

// Single error message entry with multi-language support
typedef struct nl_error_entry {
    int code;
    char** messages;      // Array of messages, indexed by language
    int lang_count;       // Number of languages
    struct nl_error_entry* next;
} nl_error_entry_t;

// Error messages for a library
typedef struct {
    int lib_id;
    char** languages;     // Array of language codes (e.g., "en_us", "zh_cn")
    int lang_count;       // Number of languages
    nl_error_entry_t* errors;  // Linked list of error entries
} nl_error_registry_t;

// =========================================
// Library Error Registration
// =========================================

// Register a library with multiple languages
NL_LANG_API int nl_lang_register_lib(int lib_id, const char** languages, int lang_count);

// Register a single language to a library (can be called multiple times)
NL_LANG_API int nl_lang_register_language(int lib_id, const char* lang_code);

// Unregister a library
NL_LANG_API void nl_lang_unregister_lib(int lib_id);

// Unregister a language from a library
NL_LANG_API int nl_lang_unregister_language(int lib_id, const char* lang_code);

// Check if a library is registered
NL_LANG_API int nl_lang_is_registered(int lib_id);

// Check if a language is registered for a library
NL_LANG_API int nl_lang_has_language(int lib_id, const char* lang_code);

// Add error message for a library (all languages at once)
NL_LANG_API int nl_lang_add_error(int lib_id, int code, const char** messages);

// Add error message for a single language (creates error entry if not exists)
NL_LANG_API int nl_lang_set_error(int lib_id, int code, const char* lang_code, const char* message);

// Add error message for a single language (alias, requires existing error entry)
NL_LANG_API int nl_lang_add_error_single(int lib_id, int code, const char* lang_code, const char* message);

// Remove error message (all languages)
NL_LANG_API int nl_lang_remove_error(int lib_id, int code);

// Remove error message for a single language
NL_LANG_API int nl_lang_remove_error_single(int lib_id, int code, const char* lang_code);

// =========================================
// Custom Error Code Registration
// =========================================

// Register custom error code (for personalized experience)
// Returns 0 on success, -1 if code already exists
NL_LANG_API int nl_lang_register_custom_code(int lib_id, int code);

// Check if error code exists
NL_LANG_API int nl_lang_has_error(int lib_id, int code);

// Get all registered error codes for a library
// Returns array of codes, terminated by 0, caller must free
NL_LANG_API int* nl_lang_get_error_codes(int lib_id, int* count);

// =========================================
// Error Message Lookup
// =========================================

// Get error message for current language
NL_LANG_API const char* nl_lang_get_error(int lib_id, int error_code);

// Get error message for specific language
NL_LANG_API const char* nl_lang_get_error_for(int lib_id, int error_code, const char* lang_code);

// Format error message with context
NL_LANG_API const char* nl_lang_format_error(char* buffer, size_t buffer_size,
    int lib_id, int error_code, const char* context);

// Format error message for specific language
NL_LANG_API const char* nl_lang_format_error_for(char* buffer, size_t buffer_size,
    int lib_id, int error_code, const char* lang_code, const char* context);

// =========================================
// File Loading (JSON)
// =========================================

// Load error messages from JSON file
// JSON format:
// {
//   "lib_id": 2,
//   "languages": ["en_us", "zh_cn", "ja_jp"],
//   "errors": {
//     "0": { "en_us": "Success", "zh_cn": "成功", "ja_jp": "成功" },
//     "-1": { "en_us": "Unknown error", "zh_cn": "未知错误", "ja_jp": "不明なエラー" }
//   }
// }
NL_LANG_API int nl_lang_load_json(const char* filepath);

// Load error messages from JSON file for specific library
NL_LANG_API int nl_lang_load_json_for(int lib_id, const char* filepath);

// Load multiple language files for a library
// files: array of file paths
// file_count: number of files
NL_LANG_API int nl_lang_load_json_multi(int lib_id, const char** files, int file_count);

// Save error messages to JSON file
NL_LANG_API int nl_lang_save_json(int lib_id, const char* filepath);

// =========================================
// Multi-Library Shared File Support
// =========================================

// Shared file configuration
typedef struct {
    const char* filepath;       // JSON file path
    int* lib_ids;               // Array of library IDs that share this file
    int lib_count;              // Number of libraries
    int allow_duplicate_codes;  // Allow duplicate error codes across libraries (0 = no, 1 = yes)
} nl_lang_shared_file_t;

// Register shared file for multiple libraries
// Must be called before loading the file
// Returns 0 on success, -1 if duplicate codes detected (when allow_duplicate_codes = 0)
NL_LANG_API int nl_lang_register_shared_file(const nl_lang_shared_file_t* config);

// Load shared file for all registered libraries
NL_LANG_API int nl_lang_load_shared_file(const char* filepath);

// Check if a library is in a shared file group
NL_LANG_API int nl_lang_is_shared_lib(int lib_id);

// Get libraries sharing a file with this library
// Returns array of lib IDs, terminated by -1, caller must free
NL_LANG_API int* nl_lang_get_shared_libraries(int lib_id, int* count);

// =========================================
// Duplicate Error Code Detection
// =========================================

// Check if error code would conflict with existing codes
// Returns: 0 = no conflict, lib_id of conflicting library if conflict exists
NL_LANG_API int nl_lang_check_code_conflict(int lib_id, int code);

// Enable/disable strict duplicate checking (global setting)
NL_LANG_API void nl_lang_set_strict_duplicates(int enabled);

// Get current strict duplicate checking setting
NL_LANG_API int nl_lang_get_strict_duplicates(void);

// =========================================
// URL/URI Loading
// =========================================

// Load error messages from URL (HTTP/HTTPS)
// Supports: http://, https://, file://, custom://
// Custom URI handler can be registered via nl_lang_set_uri_handler
NL_LANG_API int nl_lang_load_url(const char* url);

// Load error messages from URL for specific library
NL_LANG_API int nl_lang_load_url_for(int lib_id, const char* url);

// Load multiple URLs for a library
NL_LANG_API int nl_lang_load_url_multi(int lib_id, const char** urls, int url_count);

// URI handler callback
typedef int (*nl_uri_handler_t)(const char* uri, char** buffer, size_t* size);

// Register custom URI handler
// Example: "route://" -> handler that fetches from internal routing system
NL_LANG_API void nl_lang_set_uri_handler(const char* scheme, nl_uri_handler_t handler);

// Unregister URI handler
NL_LANG_API void nl_lang_unset_uri_handler(const char* scheme);

// =========================================
// Utility Functions
// =========================================

// Check if error code indicates success
NL_LANG_API int nl_error_is_success(int error_code);

// Get error category from code
NL_LANG_API const char* nl_lang_get_error_category(int error_code);

// Get library name from ID
NL_LANG_API const char* nl_lang_get_lib_name(int lib_id);

// Register library name
NL_LANG_API void nl_lang_register_lib_name(int lib_id, const char* name);

// =========================================
// Async Loading Support
// =========================================

// Async load callback
typedef void (*nl_lang_async_callback_t)(int lib_id, int result, void* user_data);

// Async load JSON file
// Returns immediately, callback is called when loading completes
NL_LANG_API int nl_lang_load_json_async(int lib_id, const char* filepath,
    nl_lang_async_callback_t callback, void* user_data);

// Async load multiple JSON files
NL_LANG_API int nl_lang_load_json_multi_async(int lib_id, const char** files, int file_count,
    nl_lang_async_callback_t callback, void* user_data);

// Async load URL
NL_LANG_API int nl_lang_load_url_async(int lib_id, const char* url,
    nl_lang_async_callback_t callback, void* user_data);

// Async load multiple URLs
NL_LANG_API int nl_lang_load_url_multi_async(int lib_id, const char** urls, int url_count,
    nl_lang_async_callback_t callback, void* user_data);

// Async load shared file
NL_LANG_API int nl_lang_load_shared_file_async(const char* filepath,
    nl_lang_async_callback_t callback, void* user_data);

// Check if async loading is in progress for a library
NL_LANG_API int nl_lang_is_async_loading(int lib_id);

// Wait for async loading to complete (blocking)
NL_LANG_API int nl_lang_wait_async(int lib_id, int timeout_ms);

// Cancel async loading
NL_LANG_API int nl_lang_cancel_async(int lib_id);

// Get async loading progress (0-100)
NL_LANG_API int nl_lang_get_async_progress(int lib_id);

// =========================================
// Helper Macros for Library Registration
// =========================================

// Define language codes array
#define NL_LANG_CODES(...) { __VA_ARGS__ }

// Define error messages for all languages
#define NL_ERROR_MSGS(...) { __VA_ARGS__ }

// ---- Table-driven error registration (maintenance-friendly, v2.4.1) ----
// Declare an error table with English (en_us) and Chinese (zh_cn) messages,
// then register it in one call. Example:
//   NL_ERROR_BEGIN(my_errors)
//       NL_ERROR(0,  "Success", "成功")
//       NL_ERROR(-1, "Failed",  "失败")
//   NL_ERROR_END
//   nl_lang_register_errors(NL_LIB_MY, my_errors);
typedef struct {
    int code;
    const char* en;   // en_us message
    const char* zh;   // zh_cn message
} nl_lang_error_def_t;

#define NL_ERROR_BEGIN(name) static const nl_lang_error_def_t name[] = {
#define NL_ERROR(code, en, zh) { (code), (en), (zh) },
#define NL_ERROR_END };

// Register a whole (en_us + zh_cn) error table for a library.
NL_LANG_API int nl_lang_register_errors_ex(int lib_id, const nl_lang_error_def_t* table, int count);

// Convenience wrapper that derives the entry count from the array size.
#define nl_lang_register_errors(lib_id, table) \
    nl_lang_register_errors_ex((lib_id), (table), (int)(sizeof(table) / sizeof((table)[0])))

// Example usage:
// static const char* languages[] = NL_LANG_CODES("en_us", "zh_cn", "ja_jp");
// static const char* msg_0[] = NL_ERROR_MSGS("Success", "成功", "成功");
// static const char* msg_1[] = NL_ERROR_MSGS("Unknown error", "未知错误", "不明なエラー");
// nl_lang_register_lib(NL_LIB_LINKAGG, languages, 3);
// nl_lang_add_error(NL_LIB_LINKAGG, 0, msg_0);
// nl_lang_add_error(NL_LIB_LINKAGG, -1, msg_1);

// =========================================
// Library Identifiers
// =========================================

#define NL_LIB_CORE      0x0000
#define NL_LIB_IPC       0x0001
#define NL_LIB_LINKAGG   0x0002
#define NL_LIB_AUTOROUTE 0x0003
#define NL_LIB_AUTOCOMPLETE 0x0004
#define NL_LIB_ERRORPAGE 0x0005
#define NL_LIB_LANG      0x0006
#define NL_LIB_TLS       0x0007
#define NL_LIB_MQTT      0x0008
#define NL_LIB_MQTT_SERVER 0x0009

// =========================================
// Variable Substitution System (v2.4.0)
// =========================================

// Variable value types
typedef enum {
    NL_VAR_TYPE_NONE = 0,
    NL_VAR_TYPE_STRING,
    NL_VAR_TYPE_INTEGER,
    NL_VAR_TYPE_FLOAT,
    NL_VAR_TYPE_BOOL,
    NL_VAR_TYPE_SCRIPT      // Result from script execution
} nl_var_type_t;

// Dynamic variable value provider: compute a variable's value on demand.
// Return non-zero and write a NUL-terminated string into `out` if a value is
// available; return 0 to fall back to the statically stored value.
typedef int (*nl_var_provider_t)(const char* name, char* out, size_t out_size, void* userdata);

// Variable storage structure
typedef struct nl_lang_variable {
    char* name;           // Variable name (e.g., "user_name")
    char* value_str;      // String value
    long long value_int;  // Integer value
    double value_float;   // Float value
    int value_bool;       // Boolean value
    nl_var_type_t type;   // Value type
    nl_var_provider_t provider;   // v2.4.1: dynamic value provider (NULL if static)
    void* provider_data;          // v2.4.1: userdata passed to provider
    char* env_name;               // v2.4.1: environment variable read on access
    struct nl_lang_variable* next;
} nl_lang_variable_t;

// Script engine result
typedef struct {
    int success;          // 1 if script executed successfully
    char result[256];     // Result string (max 256 chars)
    int result_len;       // Actual result length
    int error_code;       // Error code if failed
} nl_script_result_t;

// Script engine callback type
typedef nl_script_result_t* (*nl_script_exec_func_t)(const char* script, const char* lang_code, int error_code, void* userdata);

// Register a script engine handler
// engine_id: unique identifier (e.g., "lua", "python", "javascript")
NL_LANG_API int nl_lang_register_script_engine(const char* engine_id, nl_script_exec_func_t exec_func, void* userdata);

// Unregister a script engine handler
NL_LANG_API void nl_lang_unregister_script_engine(const char* engine_id);

// Execute script and get result
// Returns 0 on success, -1 on failure
NL_LANG_API int nl_lang_execute_script(const char* engine_id, const char* script, const char* lang_code, int error_code, char* result, size_t result_size);

// Get available script engines
NL_LANG_API const char** nl_lang_get_script_engines(int* count);

// Variable management APIs
NL_LANG_API int nl_lang_var_set(const char* name, const char* value);
NL_LANG_API int nl_lang_var_set_int(const char* name, long long value);
NL_LANG_API int nl_lang_var_set_float(const char* name, double value);
NL_LANG_API int nl_lang_var_set_bool(const char* name, int value);
NL_LANG_API const char* nl_lang_var_get(const char* name);
NL_LANG_API long long nl_lang_var_get_int(const char* name, long long default_value);
NL_LANG_API double nl_lang_var_get_float(const char* name, double default_value);
NL_LANG_API int nl_lang_var_get_bool(const char* name, int default_value);
NL_LANG_API int nl_lang_var_exists(const char* name);
NL_LANG_API int nl_lang_var_remove(const char* name);
NL_LANG_API void nl_lang_var_clear_all(void);

// ---- Dynamic variables (v2.4.1) ----
// Register a provider that computes the variable's value on every access.
// Creates the variable if it does not exist yet.
NL_LANG_API int nl_lang_var_set_provider(const char* name, nl_var_provider_t provider, void* userdata);
// Returns 1 if the variable is dynamic (has a provider) or bound to the environment.
NL_LANG_API int nl_lang_var_is_dynamic(const char* name);

// ---- External variables (v2.4.1) ----
// Bind a variable to a process environment variable, read on each access.
NL_LANG_API int nl_lang_var_bind_env(const char* name, const char* env_name);
// Import environment variables (only those starting with `prefix` when non-NULL).
NL_LANG_API int nl_lang_var_load_env(const char* prefix);
// Import "KEY=VALUE" pairs from a text file (.env style); `prefix` filters keys.
NL_LANG_API int nl_lang_var_load_file(const char* filepath, const char* prefix);

// Variable substitution in strings
// Replaces {{VAR_NAME}} with actual values
NL_LANG_API const char* nl_lang_var_replace(const char* input, char* output, size_t output_size);

// Condition evaluation (supports simple expressions)
// expr: e.g., "age > 18", "status == 'active'", "score >= 60"
NL_LANG_API int nl_lang_var_condition_eval(const char* expr);

// Variable substitution with HTML <var> tag format (v2.4.0)
// Replaces {{<var>nl.lang.VAR_NAME</var>}} with actual values from lang variable storage
// Also supports generic {{<var>NAME</var>}} format (no nl.lang. prefix)
NL_LANG_API const char* nl_lang_var_replace_html(const char* input, char* output, size_t output_size);

// Get error message with variable substitution (v2.4.0)
// Replaces {{<var>nl.lang.VAR_NAME</var>}} in the returned message
NL_LANG_API const char* nl_lang_get_error_with_vars(int lib_id, int error_code, char* buffer, size_t buffer_size);

// =========================================
// Module Info for Lazy Loading
// =========================================

#define NL_LANG_VERSION "2.4.1"
#define NL_LANG_VERSION_MAJOR 2
#define NL_LANG_VERSION_MINOR 4
#define NL_LANG_VERSION_PATCH 1

// Lang module capabilities
typedef enum {
    NL_LANG_CAP_MULTI_LANG   = 1 << 0,      // Multi-language support
    NL_LANG_CAP_ASYNC        = 1 << 1,      // Async loading
    NL_LANG_CAP_SHARED       = 1 << 2,      // Shared file support
    NL_LANG_CAP_CUSTOM       = 1 << 3,      // Custom error codes
    NL_LANG_CAP_VARIABLES    = 1 << 4,      // Variable substitution (v2.4.0)
    NL_LANG_CAP_SCRIPTING    = 1 << 5,      // Script execution support (v2.4.0)
    NL_LANG_CAP_CONDITIONALS = 1 << 6       // Conditional expressions (v2.4.0)
} nl_lang_cap_t;

// Module functions (for lazy loading)
NL_LANG_API const char* nl_lang_version(void);
NL_LANG_API int nl_lang_is_available(void);
NL_LANG_API int nl_lang_init(void);
NL_LANG_API void nl_lang_shutdown(void);

// Lang module info (for NL extension system)
NL_LANG_API nl_module_info_t* nl_lang_get_module_info(void);

// Lang extension entry point (for dynamic loading)
NL_LANG_API nl_extension_info_t* nl_lang_get_extension_info(void);

// =========================================
// Module Capability Helpers
// =========================================

#define NL_LANG_CAP_HAS(cap, flag) (((cap) & (flag)) != 0)

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_LANG_H
