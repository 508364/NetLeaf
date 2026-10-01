#ifndef NETLEAF_LANG_H
#define NETLEAF_LANG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

// =========================================
// Shared Type / Macro Definitions
// （NL_LANG_ENABLE 未定义时也必须存在，供扩展编译期安全使用）
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

// ---- Table-driven error registration (maintenance-friendly, v2.4.1) ----
typedef struct {
    int code;
    const char* en;   // en_us message
    const char* zh;   // zh_cn message
} nl_lang_error_def_t;

#define NL_ERROR_BEGIN(name) static const nl_lang_error_def_t name[] = {
#define NL_ERROR(code, en, zh) { (code), (en), (zh) },
#define NL_ERROR_END };

// Helper macros for Library Registration
#define NL_LANG_CODES(...) { __VA_ARGS__ }
#define NL_ERROR_MSGS(...) { __VA_ARGS__ }

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
#define NL_LIB_MQTT      0x0008

// =========================================
// 编译门控：NL_LANG_ENABLE
// 说明：NetLeaf 各扩展对 netleaf_lang 是"官方软依赖"。
//       默认构建由 CMake 选项 BUILD_LANG（默认 ON）开启并定义 NL_LANG_ENABLE；
//       关闭时扩展不链接 lang，以下全部声明/宏在编译期退化为 no-op，
//       无需改动扩展源码。
// =========================================
#ifdef NL_LANG_ENABLE

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

// Maximum language code length
#define NL_LANG_CODE_MAX_LEN 16

// Validate language code format (must be xx_xx)
NL_LANG_API int nl_lang_validate_code(const char* code);

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

// Register custom error code (for personalized experience)
// Returns 0 on success, -1 if code already exists
NL_LANG_API int nl_lang_register_custom_code(int lib_id, int code);

// Check if error code exists
NL_LANG_API int nl_lang_has_error(int lib_id, int code);

// Get all registered error codes for a library
// Returns array of codes, terminated by 0, caller must free
NL_LANG_API int* nl_lang_get_error_codes(int lib_id, int* count);

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

// Load error messages from JSON file
NL_LANG_API int nl_lang_load_json(const char* filepath);
NL_LANG_API int nl_lang_load_json_for(int lib_id, const char* filepath);
NL_LANG_API int nl_lang_load_json_multi(int lib_id, const char** files, int file_count);
NL_LANG_API int nl_lang_save_json(int lib_id, const char* filepath);

// Multi-Library Shared File Support
typedef struct {
    const char* filepath;       // JSON file path
    int* lib_ids;               // Array of library IDs that share this file
    int lib_count;              // Number of libraries
    int allow_duplicate_codes;  // Allow duplicate error codes across libraries (0 = no, 1 = yes)
} nl_lang_shared_file_t;

NL_LANG_API int nl_lang_register_shared_file(const nl_lang_shared_file_t* config);
NL_LANG_API int nl_lang_load_shared_file(const char* filepath);
NL_LANG_API int nl_lang_is_shared_lib(int lib_id);
NL_LANG_API int* nl_lang_get_shared_libraries(int lib_id, int* count);

// Duplicate Error Code Detection
NL_LANG_API int nl_lang_check_code_conflict(int lib_id, int code);
NL_LANG_API void nl_lang_set_strict_duplicates(int enabled);
NL_LANG_API int nl_lang_get_strict_duplicates(void);

// URL/URI Loading
NL_LANG_API int nl_lang_load_url(const char* url);
NL_LANG_API int nl_lang_load_url_for(int lib_id, const char* url);
NL_LANG_API int nl_lang_load_url_multi(int lib_id, const char** urls, int url_count);

typedef int (*nl_uri_handler_t)(const char* uri, char** buffer, size_t* size);
NL_LANG_API void nl_lang_set_uri_handler(const char* scheme, nl_uri_handler_t handler);
NL_LANG_API void nl_lang_unset_uri_handler(const char* scheme);

// Utility Functions
NL_LANG_API int nl_error_is_success(int error_code);
NL_LANG_API const char* nl_lang_get_error_category(int error_code);
NL_LANG_API const char* nl_lang_get_lib_name(int lib_id);
NL_LANG_API void nl_lang_register_lib_name(int lib_id, const char* name);

// Async Loading Support
typedef void (*nl_lang_async_callback_t)(int lib_id, int result, void* user_data);
NL_LANG_API int nl_lang_load_json_async(int lib_id, const char* filepath,
    nl_lang_async_callback_t callback, void* user_data);
NL_LANG_API int nl_lang_load_json_multi_async(int lib_id, const char** files, int file_count,
    nl_lang_async_callback_t callback, void* user_data);
NL_LANG_API int nl_lang_load_url_async(int lib_id, const char* url,
    nl_lang_async_callback_t callback, void* user_data);
NL_LANG_API int nl_lang_load_url_multi_async(int lib_id, const char** urls, int url_count,
    nl_lang_async_callback_t callback, void* user_data);
NL_LANG_API int nl_lang_load_shared_file_async(const char* filepath,
    nl_lang_async_callback_t callback, void* user_data);
NL_LANG_API int nl_lang_is_async_loading(int lib_id);
NL_LANG_API int nl_lang_wait_async(int lib_id, int timeout_ms);
NL_LANG_API int nl_lang_cancel_async(int lib_id);
NL_LANG_API int nl_lang_get_async_progress(int lib_id);

// Register a whole (en_us + zh_cn) error table for a library.
NL_LANG_API int nl_lang_register_errors_ex(int lib_id, const nl_lang_error_def_t* table, int count);

// Convenience wrapper that derives the entry count from the array size.
#define nl_lang_register_errors(lib_id, table) \
    nl_lang_register_errors_ex((lib_id), (table), (int)(sizeof(table) / sizeof((table)[0])))

// Variable Substitution System (v2.4.0)
typedef enum {
    NL_VAR_TYPE_NONE = 0,
    NL_VAR_TYPE_STRING,
    NL_VAR_TYPE_INTEGER,
    NL_VAR_TYPE_FLOAT,
    NL_VAR_TYPE_BOOL,
    NL_VAR_TYPE_SCRIPT      // Result from script execution
} nl_var_type_t;

typedef int (*nl_var_provider_t)(const char* name, char* out, size_t out_size, void* userdata);

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

typedef struct {
    int success;          // 1 if script executed successfully
    char result[256];     // Result string (max 256 chars)
    int result_len;       // Actual result length
    int error_code;       // Error code if failed
} nl_script_result_t;

typedef nl_script_result_t* (*nl_script_exec_func_t)(const char* script, const char* lang_code, int error_code, void* userdata);

NL_LANG_API int nl_lang_register_script_engine(const char* engine_id, nl_script_exec_func_t exec_func, void* userdata);
NL_LANG_API void nl_lang_unregister_script_engine(const char* engine_id);
NL_LANG_API int nl_lang_execute_script(const char* engine_id, const char* script, const char* lang_code, int error_code, char* result, size_t result_size);
NL_LANG_API const char** nl_lang_get_script_engines(int* count);

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
NL_LANG_API int nl_lang_var_set_provider(const char* name, nl_var_provider_t provider, void* userdata);
NL_LANG_API int nl_lang_var_is_dynamic(const char* name);

// ---- External variables (v2.4.1) ----
NL_LANG_API int nl_lang_var_bind_env(const char* name, const char* env_name);
NL_LANG_API int nl_lang_var_load_env(const char* prefix);
NL_LANG_API int nl_lang_var_load_file(const char* filepath, const char* prefix);

NL_LANG_API const char* nl_lang_var_replace(const char* input, char* output, size_t output_size);
NL_LANG_API int nl_lang_var_condition_eval(const char* expr);
NL_LANG_API const char* nl_lang_var_replace_html(const char* input, char* output, size_t output_size);
NL_LANG_API const char* nl_lang_get_error_with_vars(int lib_id, int error_code, char* buffer, size_t buffer_size);

#define NL_LANG_VERSION "2.4.2"
#define NL_LANG_VERSION_MAJOR 2
#define NL_LANG_VERSION_MINOR 4
#define NL_LANG_VERSION_PATCH 1

typedef enum {
    NL_LANG_CAP_MULTI_LANG   = 1 << 0,      // Multi-language support
    NL_LANG_CAP_ASYNC        = 1 << 1,      // Async loading
    NL_LANG_CAP_SHARED       = 1 << 2,      // Shared file support
    NL_LANG_CAP_CUSTOM       = 1 << 3,      // Custom error codes
    NL_LANG_CAP_VARIABLES    = 1 << 4,      // Variable substitution (v2.4.0)
    NL_LANG_CAP_SCRIPTING    = 1 << 5,      // Script execution support (v2.4.0)
    NL_LANG_CAP_CONDITIONALS = 1 << 6       // Conditional expressions (v2.4.0)
} nl_lang_cap_t;

NL_LANG_API const char* nl_lang_version(void);
NL_LANG_API int nl_lang_is_available(void);
NL_LANG_API int nl_lang_init(void);
NL_LANG_API void nl_lang_shutdown(void);
NL_LANG_API nl_module_info_t* nl_lang_get_module_info(void);
NL_LANG_API nl_extension_info_t* nl_lang_get_extension_info(void);

#define NL_LANG_CAP_HAS(cap, flag) (((cap) & (flag)) != 0)

#else /* !NL_LANG_ENABLE：无 lang 库时的软依赖空实现 */

/* 无 lang 库：退化为空操作。各扩展调用点仍编译通过，但注册无效果。
   保持返回类型与签名一致，避免调用方类型告警。 */
#define NL_LANG_API
#define NL_LANG_CODE_MAX_LEN 16

#define nl_lang_validate_code(code) (-1)
#define nl_lang_get() ("en_us")
#define nl_lang_set(code) (-1)
#define nl_lang_get_default() ("en_us")
#define nl_lang_set_default(code) (-1)
#define nl_lang_get_all() (0)
#define nl_lang_get_count() (0)

#define nl_lang_register_lib(l, langs, cnt) (-1)
#define nl_lang_register_language(l, lc) (-1)
#define nl_lang_unregister_lib(l) (0)
#define nl_lang_unregister_language(l, lc) (-1)
#define nl_lang_is_registered(l) (0)
#define nl_lang_has_language(l, lc) (0)
#define nl_lang_add_error(l, c, m) (-1)
#define nl_lang_set_error(l, c, lc, m) (-1)
#define nl_lang_add_error_single(l, c, lc, m) (-1)
#define nl_lang_remove_error(l, c) (-1)
#define nl_lang_remove_error_single(l, c, lc) (-1)
#define nl_lang_register_custom_code(l, c) (-1)
#define nl_lang_has_error(l, c) (0)
#define nl_lang_get_error_codes(l, p) (0)
#define nl_lang_get_error(l, c) ("")
#define nl_lang_get_error_for(l, c, lc) ("")
#define nl_lang_format_error(b, bs, l, c, ctx) ("")
#define nl_lang_format_error_for(b, bs, l, c, lc, ctx) ("")
#define nl_lang_load_json(fp) (-1)
#define nl_lang_load_json_for(l, fp) (-1)
#define nl_lang_load_json_multi(l, fs, n) (-1)
#define nl_lang_save_json(l, fp) (-1)

typedef struct {
    const char* filepath;
    int* lib_ids;
    int lib_count;
    int allow_duplicate_codes;
} nl_lang_shared_file_t;

#define nl_lang_register_shared_file(cfg) (-1)
#define nl_lang_load_shared_file(fp) (-1)
#define nl_lang_is_shared_lib(l) (0)
#define nl_lang_get_shared_libraries(l, p) (0)
#define nl_lang_check_code_conflict(l, c) (0)
#define nl_lang_set_strict_duplicates(en) (0)
#define nl_lang_get_strict_duplicates() (0)
#define nl_lang_load_url(url) (-1)
#define nl_lang_load_url_for(l, url) (-1)
#define nl_lang_load_url_multi(l, us, n) (-1)
typedef int (*nl_uri_handler_t)(const char* uri, char** buffer, size_t* size);
#define nl_lang_set_uri_handler(s, h) (0)
#define nl_lang_unset_uri_handler(s) (0)
#define nl_error_is_success(c) (0)
#define nl_lang_get_error_category(c) ("")
#define nl_lang_get_lib_name(l) ("")
#define nl_lang_register_lib_name(l, n) (0)

typedef void (*nl_lang_async_callback_t)(int lib_id, int result, void* user_data);
#define nl_lang_load_json_async(l, fp, cb, ud) (-1)
#define nl_lang_load_json_multi_async(l, fs, n, cb, ud) (-1)
#define nl_lang_load_url_async(l, url, cb, ud) (-1)
#define nl_lang_load_url_multi_async(l, us, n, cb, ud) (-1)
#define nl_lang_load_shared_file_async(fp, cb, ud) (-1)
#define nl_lang_is_async_loading(l) (0)
#define nl_lang_wait_async(l, t) (-1)
#define nl_lang_cancel_async(l) (-1)
#define nl_lang_get_async_progress(l) (0)

#define nl_lang_register_errors_ex(l, table, cnt) (-1)
#define nl_lang_register_errors(lib_id, table) (-1)

typedef enum {
    NL_VAR_TYPE_NONE = 0,
    NL_VAR_TYPE_STRING,
    NL_VAR_TYPE_INTEGER,
    NL_VAR_TYPE_FLOAT,
    NL_VAR_TYPE_BOOL,
    NL_VAR_TYPE_SCRIPT
} nl_var_type_t;
typedef int (*nl_var_provider_t)(const char* name, char* out, size_t out_size, void* userdata);
typedef struct nl_lang_variable {
    char* name; char* value_str; long long value_int; double value_float;
    int value_bool; nl_var_type_t type; nl_var_provider_t provider;
    void* provider_data; char* env_name; struct nl_lang_variable* next;
} nl_lang_variable_t;
typedef struct {
    int success; char result[256]; int result_len; int error_code;
} nl_script_result_t;
typedef nl_script_result_t* (*nl_script_exec_func_t)(const char* script, const char* lang_code, int error_code, void* userdata);

#define nl_lang_register_script_engine(id, f, ud) (-1)
#define nl_lang_unregister_script_engine(id) (0)
#define nl_lang_execute_script(id, s, lc, c, r, rs) (-1)
#define nl_lang_get_script_engines(p) (0)
#define nl_lang_var_set(n, v) (-1)
#define nl_lang_var_set_int(n, v) (-1)
#define nl_lang_var_set_float(n, v) (-1)
#define nl_lang_var_set_bool(n, v) (-1)
#define nl_lang_var_get(n) ("")
#define nl_lang_var_get_int(n, d) (d)
#define nl_lang_var_get_float(n, d) (d)
#define nl_lang_var_get_bool(n, d) (d)
#define nl_lang_var_exists(n) (0)
#define nl_lang_var_remove(n) (-1)
#define nl_lang_var_clear_all() (0)
#define nl_lang_var_set_provider(n, p, ud) (-1)
#define nl_lang_var_is_dynamic(n) (0)
#define nl_lang_var_bind_env(n, e) (-1)
#define nl_lang_var_load_env(p) (-1)
#define nl_lang_var_load_file(fp, p) (-1)
#define nl_lang_var_replace(in, out, os) ("")
#define nl_lang_var_condition_eval(expr) (0)
#define nl_lang_var_replace_html(in, out, os) ("")
#define nl_lang_get_error_with_vars(l, c, b, bs) ("")

#define NL_LANG_VERSION "2.4.2"
#define NL_LANG_VERSION_MAJOR 2
#define NL_LANG_VERSION_MINOR 4
#define NL_LANG_VERSION_PATCH 2

typedef enum {
    NL_LANG_CAP_MULTI_LANG   = 1 << 0,
    NL_LANG_CAP_ASYNC        = 1 << 1,
    NL_LANG_CAP_SHARED       = 1 << 2,
    NL_LANG_CAP_CUSTOM       = 1 << 3,
    NL_LANG_CAP_VARIABLES    = 1 << 4,
    NL_LANG_CAP_SCRIPTING    = 1 << 5,
    NL_LANG_CAP_CONDITIONALS = 1 << 6
} nl_lang_cap_t;

#define nl_lang_version() ("2.4.2")
#define nl_lang_is_available() (0)
#define nl_lang_init() (0)
#define nl_lang_shutdown() (0)
#define nl_lang_get_module_info() (0)
#define nl_lang_get_extension_info() (0)
#define NL_LANG_CAP_HAS(cap, flag) (0)

#endif /* NL_LANG_ENABLE */

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_LANG_H
