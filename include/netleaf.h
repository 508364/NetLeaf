#ifndef NETLEAF_H
#define NETLEAF_H

#include <stddef.h>
#include <stdint.h>

// DLL export/import macros
#ifdef _WIN32
    #ifdef NL_EXPORTS
        #define NL_API __declspec(dllexport)
    #else
        #define NL_API __declspec(dllimport)
    #endif
#else
    #define NL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NETLEAF_VERSION "2.2.0"
#define NETLEAF_VERSION_MAJOR 2
#define NETLEAF_VERSION_MINOR 2
#define NETLEAF_VERSION_PATCH 0

typedef enum {
    NL_OK = 0,
    NL_ERROR = -1,
    NL_EINVAL = -2,
    NL_ENOMEM = -3,
    NL_ETIMEOUT = -4,
    NL_ECONNECT = -5,
    NL_EAGAIN = -6,
    NL_ECLOSED = -7,
    NL_ENOTSUPPORTED = -8,
    NL_EPARSE = -9,
    NL_ESYNTAX = -10,
    NL_EFILE = -11,
    NL_EKEY_NOT_FOUND = -11,
    NL_EKEY_TYPE = -12,
    NL_EBUFFER = -13,
    NL_EOF = -14
} nl_status_t;

typedef enum {
    NL_PROTO_TCP,
    NL_PROTO_UDP,
    NL_PROTO_HTTP,
    NL_PROTO_WEBSOCKET
} nl_protocol_t;

typedef enum {
    NL_LOG_DEBUG,
    NL_LOG_INFO,
    NL_LOG_WARN,
    NL_LOG_ERROR
} nl_log_level_t;

// Debug mode API
NL_API void nl_debug_enable(int enable);
NL_API int nl_debug_is_enabled(void);

// Logging API
NL_API void nl_log(nl_log_level_t level, const char* format, ...);
NL_API void nl_log_debug(const char* format, ...);
NL_API void nl_log_info(const char* format, ...);
NL_API void nl_log_warn(const char* format, ...);
NL_API void nl_log_error(const char* format, ...);

typedef enum {
    NL_OPT_TCP_NODELAY = 1,
    NL_OPT_TCP_KEEPALIVE = 2,
    NL_OPT_TCP_KEEPIDLE = 3,
    NL_OPT_TCP_KEEPINTVL = 4,
    NL_OPT_TCP_KEEPCNT = 5,
    NL_OPT_SO_SNDBUF = 6,
    NL_OPT_SO_RCVBUF = 7,
    NL_OPT_SO_REUSEADDR = 8,
    NL_OPT_SO_REUSEPORT = 9,
    NL_OPT_SO_LINGER = 10,
    NL_OPT_SO_BROADCAST = 11
} nl_socket_option_t;

typedef struct nl_server nl_server_t;
typedef struct nl_client nl_client_t;
typedef struct nl_config nl_config_t;
typedef struct nl_buffer nl_buffer_t;
typedef struct nl_event_loop nl_event_loop_t;

typedef void (*nl_request_handler)(nl_buffer_t* req, nl_buffer_t* resp, void* user_data);
typedef void (*nl_log_callback)(nl_log_level_t level, const char* msg, void* user_data);
typedef void (*nl_udp_message_handler)(const char* data, size_t len, 
                                        const char* from_addr, int from_port, void* user_data);

NL_API nl_server_t* nl_server_create(nl_protocol_t protocol, int port);
NL_API void nl_server_destroy(nl_server_t* server);
NL_API int nl_server_start(nl_server_t* server);
NL_API void nl_server_stop(nl_server_t* server);
NL_API void nl_server_set_handler(nl_server_t* server, nl_request_handler handler, void* user_data);
NL_API void nl_server_set_udp_handler(nl_server_t* server, nl_udp_message_handler handler, void* user_data);
NL_API int nl_server_set_option(nl_server_t* server, nl_socket_option_t option, int value);
NL_API int nl_server_get_option(nl_server_t* server, nl_socket_option_t option, int* value);

NL_API nl_client_t* nl_client_create(nl_protocol_t protocol);
NL_API void nl_client_destroy(nl_client_t* client);
NL_API int nl_client_connect(nl_client_t* client, const char* host, int port);
NL_API void nl_client_disconnect(nl_client_t* client);
NL_API int nl_client_send(nl_client_t* client, const void* data, size_t len);
NL_API int nl_client_recv(nl_client_t* client, void* buf, size_t len);
NL_API int nl_client_send_to(nl_client_t* client, const char* host, int port, const void* data, size_t len);
NL_API int nl_client_recv_from(nl_client_t* client, void* buf, size_t len, char* from_addr, size_t addr_len, int* from_port);
NL_API int nl_client_set_option(nl_client_t* client, nl_socket_option_t option, int value);
NL_API int nl_client_get_option(nl_client_t* client, nl_socket_option_t option, int* value);
NL_API int nl_client_get_fd(nl_client_t* client);
NL_API int nl_server_get_fd(nl_server_t* server);

NL_API nl_config_t* nl_config_create(void);
NL_API void nl_config_destroy(nl_config_t* config);
NL_API int nl_config_load(nl_config_t* config, const char* path);
NL_API int nl_config_save(nl_config_t* config, const char* path);
NL_API const char* nl_config_get(nl_config_t* config, const char* key);
NL_API void nl_config_set(nl_config_t* config, const char* key, const char* value);

NL_API nl_buffer_t* nl_buffer_create(size_t capacity);
NL_API void nl_buffer_destroy(nl_buffer_t* buffer);
NL_API void nl_buffer_clear(nl_buffer_t* buffer);
NL_API size_t nl_buffer_write(nl_buffer_t* buffer, const void* data, size_t len);
NL_API size_t nl_buffer_read(nl_buffer_t* buffer, void* data, size_t len);
NL_API size_t nl_buffer_size(nl_buffer_t* buffer);

NL_API void nl_log_set_level(nl_log_level_t level);
NL_API void nl_log_set_callback(nl_log_callback callback, void* user_data);

NL_API const char* nl_version_string(void);
NL_API int nl_version_major(void);
NL_API int nl_version_minor(void);
NL_API int nl_version_patch(void);

// =========================================
// Advanced Server API - File Server
// =========================================

typedef struct nl_file_server nl_file_server_t;

typedef enum {
    NL_METHOD_GET,
    NL_METHOD_POST,
    NL_METHOD_PUT,
    NL_METHOD_DELETE,
    NL_METHOD_PATCH,
    NL_METHOD_HEAD,
    NL_METHOD_OPTIONS
} nl_http_method_t;

typedef void (*nl_http_handler_t)(
    const char* path,
    nl_http_method_t method,
    const char* body,
    size_t body_size,
    char** response,
    size_t* response_size,
    void* user_data
);

// Create a file server for serving static files
NL_API nl_file_server_t* nl_file_server_create(const char* directory, int port);
NL_API void nl_file_server_destroy(nl_file_server_t* server);
NL_API int nl_file_server_start(nl_file_server_t* server);
NL_API void nl_file_server_stop(nl_file_server_t* server);
NL_API void nl_file_server_set_index(nl_file_server_t* server, const char* index_file);
NL_API void nl_file_server_set_easter_egg(nl_file_server_t* server, int enable);

// Simple one-liner to start serving files
NL_API int nl_serve_files(const char* directory, int port);

// =========================================
// Advanced Server API - Route Configuration
// =========================================

typedef struct nl_route nl_route_t;
typedef struct nl_router nl_router_t;

NL_API nl_router_t* nl_router_create(void);
NL_API void nl_router_destroy(nl_router_t* router);
NL_API void nl_router_add_route(nl_router_t* router, const char* path, nl_http_method_t method, nl_http_handler_t handler, void* user_data);
NL_API void nl_router_set_static_dir(nl_router_t* router, const char* directory);

// Start a server with router
NL_API int nl_router_serve(nl_router_t* router, int port);

// Quick start function
NL_API int nl_serve(int port, nl_http_handler_t default_handler, void* user_data);

// =========================================
// Inline HTML/Vue Modern Web Server API
// =========================================

typedef struct nl_web_server nl_web_server_t;

// Create a web server with inline HTML/Vue support
// Now automatically starts the server
NL_API nl_web_server_t* nl_web_create(int port);
NL_API void nl_web_destroy(nl_web_server_t* server);
NL_API int nl_web_start(nl_web_server_t* server);
NL_API void nl_web_stop(nl_web_server_t* server);

// Stop server by port (new simplified API)
NL_API void nl_web_stop_by_port(int port);

// Set auto-cleanup on program exit (default: disabled)
NL_API void nl_web_set_auto_cleanup(int enable);

// Set encoding for web responses
NL_API void nl_web_set_encoding(nl_web_server_t* server, const char* encoding);

// Supported encoding constants
#define NL_ENCODING_UTF8 "UTF-8"
#define NL_ENCODING_GBK "GBK"
#define NL_ENCODING_GB2312 "GB2312"
#define NL_ENCODING_GB18030 "GB18030"
#define NL_ENCODING_ISO8859_1 "ISO-8859-1"
#define NL_ENCODING_US_ASCII "US-ASCII"
#define NL_ENCODING_UTF16 "UTF-16"
#define NL_ENCODING_BIG5 "Big5"

// Validate encoding string
NL_API int nl_web_validate_encoding(const char* encoding);

// =========================================
// Error Page API (v2.2.0)
// =========================================
// Support custom error pages with variable substitution
// Error pages can be linked with autoroute for route suggestions

typedef enum {
    NL_ERROR_PAGE_OK = 0,
    NL_ERROR_PAGE_NOT_FOUND = 1,
    NL_ERROR_PAGE_INVALID_TEMPLATE = 2,
    NL_ERROR_PAGE_VAR_NOT_FOUND = 3,
    NL_ERROR_PAGE_RENDER_FAILED = 4
} nl_error_page_status_t;

// Error page variables structure (mandatory fields for substitution)
typedef struct {
    int status_code;              // HTTP status code (404, 500, etc.)
    const char* error_message;    // Error description
    const char* requested_path;   // Requested URL path
    const char* suggestion;       // Suggested route (from autoroute)
    const char* timestamp;        // Server timestamp
    const char* server_version;   // Server version string
} nl_error_page_vars_t;

// Set custom error page template for a status code
// Template file must contain mandatory placeholders:
//   {{ERROR_CODE}}, {{ERROR_MESSAGE}}, {{REQUESTED_PATH}}, {{SUGGESTION}}
NL_API int nl_web_server_set_error_page(nl_web_server_t* server,
                                        int status_code,
                                        const char* template_path);

// Enable/disable autoroute suggestions on error pages
NL_API int nl_web_server_enable_error_suggestions(nl_web_server_t* server,
                                                   int enable);

// Check if error suggestions are enabled
NL_API int nl_web_server_is_error_suggestions_enabled(nl_web_server_t* server);

// Render error page with given variables
// Returns allocated string, must be freed by caller
NL_API char* nl_render_error_page(const char* template_content,
                                   nl_error_page_vars_t* vars);

// Quick error page response generation
// Builds a default error page with the given status and message
NL_API char* nl_make_error_response(int status_code,
                                     const char* error_message,
                                     const char* requested_path,
                                     const char* suggestion);

// =========================================
// Dynamic Encoding Adaptation API (v2.1.6)
// =========================================
// Auto-detect and convert encoding based on client request
// "见人说人话，见鬼说鬼话" - automatic encoding negotiation and conversion

// Enable/disable automatic encoding negotiation
// When enabled, the library will:
// 1. Parse Accept-Charset header from client
// 2. Automatically convert responses to the client's preferred encoding
// 3. Handle bidirectional conversion between internal UTF-8 and external encodings
NL_API void nl_web_enable_auto_encoding(nl_web_server_t* server, int enable);

// Check if auto-encoding is enabled
NL_API int nl_web_is_auto_encoding_enabled(nl_web_server_t* server);

// Set fallback encoding when client doesn't specify
NL_API void nl_web_set_fallback_encoding(nl_web_server_t* server, const char* encoding);

// Get the negotiated encoding for a request
// Returns the encoding that will be used for the response
NL_API const char* nl_web_get_negotiated_encoding(nl_web_server_t* server);

// Global encoding conversion functions
// Convert string from source_encoding to target_encoding
// Returns allocated buffer, must be freed by caller
// Smart optimization: Only converts non-ASCII characters to reduce performance overhead
NL_API char* nl_encoding_convert(const char* input, size_t input_len, 
                                 const char* source_encoding, const char* target_encoding);

// Detect encoding of a string
// Returns the most likely encoding or NULL if undetectable
NL_API const char* nl_encoding_detect(const char* input, size_t input_len);

// Get system default encoding
NL_API const char* nl_encoding_get_system_default(void);

// =========================================
// Multi-Scenario Encoding Conversion API
// =========================================
// All functions support lazy loading and automatic cleanup
// Only processes non-ASCII characters to minimize performance impact

// Console output with automatic encoding conversion
// Automatically converts to console's native encoding
NL_API void nl_encoding_console_output(const char* text, const char* encoding);

// HTML response encoding conversion
// Input is assumed to be UTF-8, converts to target_encoding
NL_API char* nl_encoding_html_convert(const char* html, size_t html_len, const char* target_encoding);

// JSON response encoding conversion
// Input is assumed to be UTF-8, converts to target_encoding
NL_API char* nl_encoding_json_convert(const char* json, size_t json_len, const char* target_encoding);

// TOML response encoding conversion
// Input is assumed to be UTF-8, converts to target_encoding
NL_API char* nl_encoding_toml_convert(const char* toml, size_t toml_len, const char* target_encoding);

// Vue code encoding conversion
// Input is assumed to be UTF-8, converts to target_encoding
NL_API char* nl_encoding_vue_convert(const char* vue_code, size_t vue_len, const char* target_encoding);

// Inline page code encoding conversion
// Input is assumed to be UTF-8, converts to target_encoding
NL_API char* nl_encoding_inline_convert(const char* code, size_t code_len, const char* target_encoding);

// Log output encoding conversion
// Input is assumed to be UTF-8, converts to target_encoding
NL_API char* nl_encoding_log_convert(const char* log, size_t log_len, const char* target_encoding);

// =========================================
// Chinese Simplified/Traditional Conversion API
// =========================================

// Check if text contains simplified Chinese characters
NL_API int nl_encoding_is_simplified_chinese(const char* input, size_t input_len);

// Check if text contains traditional Chinese characters
NL_API int nl_encoding_is_traditional_chinese(const char* input, size_t input_len);

// Convert between simplified and traditional Chinese
// to_traditional: 1 = simplified -> traditional, 0 = traditional -> simplified
// Input must be UTF-8, output is UTF-8
NL_API char* nl_encoding_chinese_convert(const char* input, size_t input_len, int to_traditional);

// =========================================
// Encoding Module Management API (Lazy Loading)
// =========================================

// Check if encoding module is loaded
NL_API int nl_encoding_is_module_loaded(void);

// Unload encoding module (automatic cleanup, reduces memory usage)
NL_API void nl_encoding_unload_module(void);

// Error and warning API
typedef enum {
    NL_WARN_NONE = 0,
    NL_WARN_PORT_IN_USE = 1,
    NL_WARN_INVALID_ENCODING = 2,
    NL_WARN_MEMORY_LIMIT = 3,
    NL_WARN_CONNECTION_LIMIT = 4,
    NL_WARN_INVALID_CONFIG = 5,
    NL_WARN_BUFFER_OVERFLOW = 6
} nl_warning_t;

NL_API const char* nl_warning_message(nl_warning_t warning);
NL_API void nl_web_enable_warnings(int enable);
NL_API int nl_web_get_last_warning(nl_warning_t* warning);

// Add inline HTML/Vue response
NL_API void nl_web_add_html(nl_web_server_t* server, const char* path, const char* html);
NL_API void nl_web_add_vue(nl_web_server_t* server, const char* path, const char* vue_code);
NL_API void nl_web_add_json(nl_web_server_t* server, const char* path, const char* json);

// Add inline HTML/Vue with variable substitution
// Variables are in format: {{<var>variable_name</var>}}
NL_API void nl_web_add_html_with_vars(nl_web_server_t* server, const char* path, const char* html, const char** vars, const char** values, int count);
NL_API void nl_web_add_vue_with_vars(nl_web_server_t* server, const char* path, const char* vue_code, const char** vars, const char** values, int count);

// Modern responsive helper: auto-generate modern UI
NL_API void nl_web_add_counter(nl_web_server_t* server, const char* path, const char* title);
NL_API void nl_web_add_dashboard(nl_web_server_t* server, const char* path, const char* title);
NL_API void nl_web_add_form(nl_web_server_t* server, const char* path, const char* title, const char** fields, int field_count);
NL_API void nl_web_add_todo(nl_web_server_t* server, const char* path, const char* title);
NL_API void nl_web_add_chat(nl_web_server_t* server, const char* path, const char* title);
NL_API void nl_web_add_gallery(nl_web_server_t* server, const char* path, const char* title, const char** image_urls, int count);

// Simple one-liner: serve inline HTML
NL_API int nl_serve_html(int port, const char* html);
NL_API int nl_serve_vue(int port, const char* vue_code);
NL_API int nl_serve_dashboard(int port, const char* title);
NL_API int nl_serve_todo(int port, const char* title);
NL_API int nl_serve_chat(int port, const char* title);

// =========================================
// JSON Parser API
// =========================================

typedef enum {
    NL_JSON_NULL = 0,
    NL_JSON_BOOL = 1,
    NL_JSON_INT = 2,
    NL_JSON_DOUBLE = 3,
    NL_JSON_STRING = 4,
    NL_JSON_ARRAY = 5,
    NL_JSON_OBJECT = 6
} nl_json_type_t;

NL_API void* nl_json_parse(const char* json_str, nl_status_t* error_code, int* error_line, int* error_col);
NL_API void* nl_json_parse_file(const char* file_path, nl_status_t* error_code);
NL_API void nl_json_destroy(void* json);
NL_API int nl_json_get_type(void* json);
NL_API int nl_json_get_bool(void* json);
NL_API int64_t nl_json_get_int(void* json);
NL_API double nl_json_get_double(void* json);
NL_API const char* nl_json_get_string(void* json);
NL_API size_t nl_json_array_size(void* json);
NL_API void* nl_json_array_get(void* json, size_t index);
NL_API void* nl_json_object_get(void* json, const char* key);
NL_API int nl_json_has_key(void* json, const char* key);
NL_API char* nl_json_stringify(void* json, int pretty);
NL_API int nl_json_save_file(void* json, const char* file_path, int pretty);
NL_API const char* nl_json_error_message(nl_status_t error_code);

// =========================================
// TOML Parser API
// =========================================

typedef enum {
    NL_TOML_STRING = 1,
    NL_TOML_INT = 2,
    NL_TOML_FLOAT = 3,
    NL_TOML_BOOL = 4,
    NL_TOML_ARRAY = 5,
    NL_TOML_TABLE = 6
} nl_toml_type_t;

NL_API void* nl_toml_parse(const char* toml_str, nl_status_t* error_code, int* error_line, int* error_col);
NL_API void* nl_toml_parse_file(const char* file_path, nl_status_t* error_code);
NL_API void nl_toml_destroy(void* toml);
NL_API int nl_toml_get_type(void* toml);
NL_API const char* nl_toml_get_string(void* toml);
NL_API int64_t nl_toml_get_int(void* toml);
NL_API double nl_toml_get_float(void* toml);
NL_API int nl_toml_get_bool(void* toml);
NL_API size_t nl_toml_array_size(void* toml);
NL_API void* nl_toml_array_get(void* toml, size_t index);
NL_API void* nl_toml_table_get(void* toml, const char* key);
NL_API int nl_toml_has_key(void* toml, const char* key);
NL_API char* nl_toml_stringify(void* toml);
NL_API int nl_toml_save_file(void* toml, const char* file_path);
NL_API const char* nl_toml_error_message(nl_status_t error_code);

// =========================================
// System Information API (Lazy Loading)
// =========================================

// RAM unit configuration
typedef enum {
    NL_RAM_UNIT_DECIMAL = 1000,  // Linux default: 1 KB = 1000 bytes
    NL_RAM_UNIT_BINARY = 1024    // Windows default: 1 KB = 1024 bytes
} nl_ram_unit_t;

// Set RAM unit (affects nl_sys_info_get_total_ram)
NL_API void nl_sys_info_set_ram_unit(nl_ram_unit_t unit);
NL_API nl_ram_unit_t nl_sys_info_get_ram_unit(void);

// Get system information (lazily loaded)
NL_API const char* nl_sys_info_get_os_name(void);
NL_API const char* nl_sys_info_get_architecture(void);
NL_API const char* nl_sys_info_get_cpu_model(void);
NL_API int64_t nl_sys_info_get_total_ram(void);  // Returns bytes based on configured unit
NL_API const char* nl_sys_info_get_runtime_version(void);

// Release cached system info (for lazy reload)
NL_API void nl_sys_info_clear_cache(void);

// Check if system info has been loaded
NL_API int nl_sys_info_is_loaded(void);

// =========================================
// Lazy Loading Configuration API
// =========================================

typedef enum {
    NL_LAZY_MODULE_HTTP = 1,
    NL_LAZY_MODULE_WEBSOCKET = 2,
    NL_LAZY_MODULE_TCP = 4,
    NL_LAZY_MODULE_UDP = 8,
    NL_LAZY_MODULE_TOML = 16,
    NL_LAZY_MODULE_JSON = 32,
    NL_LAZY_MODULE_SYSINFO = 64,
    NL_LAZY_MODULE_ALL = 0xFF
} nl_lazy_module_t;

typedef enum {
    NL_LAZY_STATUS_UNLOADED = 0,
    NL_LAZY_STATUS_LOADING = 1,
    NL_LAZY_STATUS_LOADED = 2,
    NL_LAZY_STATUS_STOPPING = 3,
    NL_LAZY_STATUS_STOPPED = 4
} nl_lazy_status_t;

NL_API void nl_lazy_enable(int enable);
NL_API void nl_lazy_enable_module(nl_lazy_module_t module);
NL_API void nl_lazy_disable_module(nl_lazy_module_t module);
NL_API int nl_lazy_is_enabled(nl_lazy_module_t module);
NL_API void nl_lazy_clear_all_cache(void);
NL_API void nl_lazy_preload_module(nl_lazy_module_t module);

NL_API void nl_lazy_stop_module(nl_lazy_module_t module);
NL_API nl_lazy_status_t nl_lazy_get_module_status(nl_lazy_module_t module);
NL_API int nl_lazy_is_module_loaded(nl_lazy_module_t module);
NL_API int nl_lazy_set_thread_count(int count);
NL_API int nl_lazy_get_thread_count(void);

#ifdef __cplusplus
}
#endif

#endif
