#ifndef NETLEAF_LANG_H
#define NETLEAF_LANG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

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
int nl_lang_validate_code(const char* code);

// =========================================
// Language Configuration
// =========================================

// Get current language code (e.g., "en_us", "zh_cn")
const char* nl_lang_get(void);

// Set language by code (must be xx_xx format)
// Returns 0 on success, -1 if format is invalid
int nl_lang_set(const char* code);

// Get default language code
const char* nl_lang_get_default(void);

// Set default language code
int nl_lang_set_default(const char* code);

// Get all registered language codes
// Returns array of strings, terminated by NULL
const char** nl_lang_get_all(void);

// Get number of registered languages
int nl_lang_get_count(void);

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
int nl_lang_register_lib(int lib_id, const char** languages, int lang_count);

// Register a single language to a library (can be called multiple times)
int nl_lang_register_language(int lib_id, const char* lang_code);

// Unregister a library
void nl_lang_unregister_lib(int lib_id);

// Unregister a language from a library
int nl_lang_unregister_language(int lib_id, const char* lang_code);

// Check if a library is registered
int nl_lang_is_registered(int lib_id);

// Check if a language is registered for a library
int nl_lang_has_language(int lib_id, const char* lang_code);

// Add error message for a library (all languages at once)
int nl_lang_add_error(int lib_id, int code, const char** messages);

// Add error message for a single language (creates error entry if not exists)
int nl_lang_set_error(int lib_id, int code, const char* lang_code, const char* message);

// Add error message for a single language (alias, requires existing error entry)
int nl_lang_add_error_single(int lib_id, int code, const char* lang_code, const char* message);

// Remove error message (all languages)
int nl_lang_remove_error(int lib_id, int code);

// Remove error message for a single language
int nl_lang_remove_error_single(int lib_id, int code, const char* lang_code);

// =========================================
// Custom Error Code Registration
// =========================================

// Register custom error code (for personalized experience)
// Returns 0 on success, -1 if code already exists
int nl_lang_register_custom_code(int lib_id, int code);

// Check if error code exists
int nl_lang_has_error(int lib_id, int code);

// Get all registered error codes for a library
// Returns array of codes, terminated by 0, caller must free
int* nl_lang_get_error_codes(int lib_id, int* count);

// =========================================
// Error Message Lookup
// =========================================

// Get error message for current language
const char* nl_lang_get_error(int lib_id, int error_code);

// Get error message for specific language
const char* nl_lang_get_error_for(int lib_id, int error_code, const char* lang_code);

// Format error message with context
const char* nl_lang_format_error(char* buffer, size_t buffer_size,
    int lib_id, int error_code, const char* context);

// Format error message for specific language
const char* nl_lang_format_error_for(char* buffer, size_t buffer_size,
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
int nl_lang_load_json(const char* filepath);

// Load error messages from JSON file for specific library
int nl_lang_load_json_for(int lib_id, const char* filepath);

// Load multiple language files for a library
// files: array of file paths
// file_count: number of files
int nl_lang_load_json_multi(int lib_id, const char** files, int file_count);

// Save error messages to JSON file
int nl_lang_save_json(int lib_id, const char* filepath);

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
int nl_lang_register_shared_file(const nl_lang_shared_file_t* config);

// Load shared file for all registered libraries
int nl_lang_load_shared_file(const char* filepath);

// Check if a library is in a shared file group
int nl_lang_is_shared_lib(int lib_id);

// Get libraries sharing a file with this library
// Returns array of lib IDs, terminated by -1, caller must free
int* nl_lang_get_shared_libraries(int lib_id, int* count);

// =========================================
// Duplicate Error Code Detection
// =========================================

// Check if error code would conflict with existing codes
// Returns: 0 = no conflict, lib_id of conflicting library if conflict exists
int nl_lang_check_code_conflict(int lib_id, int code);

// Enable/disable strict duplicate checking (global setting)
void nl_lang_set_strict_duplicates(int enabled);

// Get current strict duplicate checking setting
int nl_lang_get_strict_duplicates(void);

// =========================================
// URL/URI Loading
// =========================================

// Load error messages from URL (HTTP/HTTPS)
// Supports: http://, https://, file://, custom://
// Custom URI handler can be registered via nl_lang_set_uri_handler
int nl_lang_load_url(const char* url);

// Load error messages from URL for specific library
int nl_lang_load_url_for(int lib_id, const char* url);

// Load multiple URLs for a library
int nl_lang_load_url_multi(int lib_id, const char** urls, int url_count);

// URI handler callback
typedef int (*nl_uri_handler_t)(const char* uri, char** buffer, size_t* size);

// Register custom URI handler
// Example: "route://" -> handler that fetches from internal routing system
void nl_lang_set_uri_handler(const char* scheme, nl_uri_handler_t handler);

// Unregister URI handler
void nl_lang_unset_uri_handler(const char* scheme);

// =========================================
// Utility Functions
// =========================================

// Check if error code indicates success
int nl_error_is_success(int error_code);

// Get error category from code
const char* nl_lang_get_error_category(int error_code);

// Get library name from ID
const char* nl_lang_get_lib_name(int lib_id);

// Register library name
void nl_lang_register_lib_name(int lib_id, const char* name);

// =========================================
// Async Loading Support
// =========================================

// Async load callback
typedef void (*nl_lang_async_callback_t)(int lib_id, int result, void* user_data);

// Async load JSON file
// Returns immediately, callback is called when loading completes
int nl_lang_load_json_async(int lib_id, const char* filepath,
    nl_lang_async_callback_t callback, void* user_data);

// Async load multiple JSON files
int nl_lang_load_json_multi_async(int lib_id, const char** files, int file_count,
    nl_lang_async_callback_t callback, void* user_data);

// Async load URL
int nl_lang_load_url_async(int lib_id, const char* url,
    nl_lang_async_callback_t callback, void* user_data);

// Async load multiple URLs
int nl_lang_load_url_multi_async(int lib_id, const char** urls, int url_count,
    nl_lang_async_callback_t callback, void* user_data);

// Async load shared file
int nl_lang_load_shared_file_async(const char* filepath,
    nl_lang_async_callback_t callback, void* user_data);

// Check if async loading is in progress for a library
int nl_lang_is_async_loading(int lib_id);

// Wait for async loading to complete (blocking)
int nl_lang_wait_async(int lib_id, int timeout_ms);

// Cancel async loading
int nl_lang_cancel_async(int lib_id);

// Get async loading progress (0-100)
int nl_lang_get_async_progress(int lib_id);

// =========================================
// Helper Macros for Library Registration
// =========================================

// Define language codes array
#define NL_LANG_CODES(...) { __VA_ARGS__ }

// Define error messages for all languages
#define NL_ERROR_MSGS(...) { __VA_ARGS__ }

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

// =========================================
// Module Info for Lazy Loading
// =========================================

#define NL_LANG_VERSION "2.2.1"
#define NL_LANG_VERSION_MAJOR 2
#define NL_LANG_VERSION_MINOR 2
#define NL_LANG_VERSION_PATCH 1

// Lang module capabilities
typedef enum {
    NL_LANG_CAP_MULTI_LANG = 1 << 0,      // Multi-language support
    NL_LANG_CAP_ASYNC = 1 << 1,           // Async loading
    NL_LANG_CAP_SHARED = 1 << 2,          // Shared file support
    NL_LANG_CAP_CUSTOM = 1 << 3           // Custom error codes
} nl_lang_cap_t;

// Module functions (for lazy loading)
const char* nl_lang_version(void);
int nl_lang_is_available(void);
int nl_lang_init(void);
void nl_lang_shutdown(void);

// =========================================
// Module Capability Helpers
// =========================================

#define NL_LANG_CAP_HAS(cap, flag) (((cap) & (flag)) != 0)

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_LANG_H