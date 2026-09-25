#ifndef NETLEAF_H
#define NETLEAF_H 1

/**
 * @file netleaf.h
 * @brief NetLeaf 主头文件 v2.4.0
 * @date 2026-09-18
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "netleaf_module.h"

// Version macros
#define NETLEAF_VERSION "2.4.1"
#define NETLEAF_VERSION_MAJOR 2
#define NETLEAF_VERSION_MINOR 4
#define NETLEAF_VERSION_PATCH 1

// Network types
typedef enum {
    NL_NET_TCP,
    NL_NET_UDP
} nl_net_type_t;

// Protocol types
typedef enum {
    NL_PROTO_TCP,
    NL_PROTO_HTTP,
    NL_PROTO_WEBSOCKET,
    NL_PROTO_MQTT,
    NL_PROTO_UDP
} nl_proto_t;

// Protocol types (legacy alias for compatibility)
typedef nl_proto_t nl_protocol_t;

// HTTP method types
typedef enum {
    NL_METHOD_GET = 0,
    NL_METHOD_POST,
    NL_METHOD_PUT,
    NL_METHOD_DELETE,
    NL_METHOD_PATCH,
    NL_METHOD_HEAD,
    NL_METHOD_OPTIONS
} nl_http_method_t;

// HTTP handler type
typedef void (*nl_http_handler_t)(const char* path, nl_http_method_t method, const char* body, size_t body_size, char** response, size_t* response_len, void* userdata);

// Request handler type (legacy alias)
typedef nl_http_handler_t nl_request_handler;

// UDP message handler type
typedef void (*nl_udp_message_handler)(const char* data, size_t len, void* userdata);

// Socket descriptor
#ifdef _WIN32
    #include <winsock2.h>
    typedef SOCKET nl_sock_t;
#else
    typedef int nl_sock_t;
#endif

// Maximum path length
#define NL_MAX_PATH 260
#define NL_MAX_URL_LEN 2048
#define NL_MAX_HEADER_LEN 8192
#define NL_MAX_BODY_LEN (1024 * 1024 * 100) // 100MB

// Server configuration
typedef struct {
    const char* host;
    int port;
    nl_net_type_t type;
    int max_connections;
    int timeout_ms;
    bool ssl_enabled;
} nl_server_config_t;

// Client configuration
typedef struct {
    const char* host;
    int port;
    nl_net_type_t type;
    int timeout_ms;
    bool ssl_enabled;
} nl_client_config_t;

// Error codes
typedef enum {
    NL_OK = 0,
    NL_ERROR = -1,
    NL_ENOTSUPPORTED = -2,
    NL_EINVAL = -10,
    NL_ECONNREFUSED = -11,
    NL_ECONNECT = -12,
    NL_EAGAIN = -13,
    NL_ECLOSED = -14,
    NL_ETIMEOUT = -15,
    NL_ENOMEM = -16,
    NL_EADDRINUSE = -17,
    NL_ETOOLARGE = -18,
    NL_EPROTO = -19,
    NL_ENOENT = -20,
    NL_EPARSE = -21,
    NL_ESYNTAX = -22,
    NL_EFILE = -23
} nl_errno_t;

// JSON status type (alias for error codes)
typedef nl_errno_t nl_status_t;

// JSON value types
typedef enum {
    NL_JSON_NULL = 0,
    NL_JSON_BOOL,
    NL_JSON_INT,
    NL_JSON_DOUBLE,
    NL_JSON_STRING,
    NL_JSON_ARRAY,
    NL_JSON_OBJECT
} nl_json_type_t;

// Socket option type
typedef enum {
    NL_OPT_TCP_NODELAY = 0,
    NL_OPT_TCP_KEEPALIVE,
    NL_OPT_SO_SNDBUF,
    NL_OPT_SO_RCVBUF,
    NL_OPT_SO_REUSEADDR,
    NL_OPT_SO_REUSEPORT,
    NL_OPT_SO_BROADCAST
} nl_socket_option_t;

// Server handle
typedef struct nl_server nl_server_t;

// Client handle
typedef struct nl_client nl_client_t;

// WebSocket handle
typedef struct nl_ws_conn nl_ws_conn_t;

// Linkagg handle
typedef struct nl_linkagg_handle nl_linkagg_handle_t;

// Config handle (internal)
typedef struct nl_config nl_config_t;

// Buffer handle
typedef struct nl_buffer nl_buffer_t;

// Web server handle (for advanced web features)
typedef struct nl_web_server nl_web_server_t;

// Redirect type for web server
typedef enum {
    NL_REDIRECT_PERMANENT = 301,
    NL_REDIRECT_TEMPORARY = 302
} nl_redirect_type_t;

// Error page variables alias (for compatibility)
typedef struct {
    int status_code;
    const char* error_message;
    const char* requested_path;
    const char* suggestion;
    const char* server_version;
    const char* timestamp;
} nl_error_page_vars_t;

// Error page return codes
#define NL_ERROR_PAGE_OK 0
#define NL_ERROR_PAGE_NOT_FOUND -1

// TOML value types
typedef enum {
    NL_TOML_NULL = 0,
    NL_TOML_BOOL,
    NL_TOML_INT,
    NL_TOML_FLOAT,
    NL_TOML_STRING,
    NL_TOML_ARRAY,
    NL_TOML_TABLE
} nl_toml_type_t;

// File server handle
typedef struct nl_file_server nl_file_server_t;

// Router handle
typedef struct nl_router nl_router_t;

// =========================================
// Version Information
// =========================================

/**
 * @brief Get NetLeaf version string
 */
NL_API const char* nl_version_string(void);

/**
 * @brief Get NetLeaf version number
 */
NL_API void nl_version(int* major, int* minor, int* patch);

/**
 * @brief Get NetLeaf major/minor/patch version parts
 */
NL_API int nl_version_major(void);
NL_API int nl_version_minor(void);
NL_API int nl_version_patch(void);

// =========================================
// Server API
// =========================================

/**
 * @brief Create a new server
 * @param config Server configuration
 * @return Server handle or NULL on failure
 */
NL_API nl_server_t* nl_server_create(nl_protocol_t protocol, int port);

/**
 * @brief Start the server
 * @param server Server handle
 * @return NL_OK on success
 */
NL_API int nl_server_start(nl_server_t* server);

/**
 * @brief Stop the server
 * @param server Server handle
 */
NL_API void nl_server_stop(nl_server_t* server);

/**
 * @brief Destroy the server
 * @param server Server handle
 */
NL_API void nl_server_destroy(nl_server_t* server);

/**
 * @brief Set HTTP handler for server
 */
NL_API void nl_server_set_handler(nl_server_t* server, nl_request_handler handler, void* user_data);

/**
 * @brief Set UDP message handler for server
 */
NL_API void nl_server_set_udp_handler(nl_server_t* server, nl_udp_message_handler handler, void* user_data);

/**
 * @brief Set socket option for server
 */
NL_API int nl_server_set_option(nl_server_t* server, nl_socket_option_t option, int value);

/**
 * @brief Get socket option for server
 */
NL_API int nl_server_get_option(nl_server_t* server, nl_socket_option_t option, int* value);

/**
 * @brief Server connection callback
 */
typedef void (*nl_server_connect_cb)(nl_server_t* server, nl_client_t* client, void* userdata);

/**
 * @brief Server disconnect callback
 */
typedef void (*nl_server_disconnect_cb)(nl_server_t* server, nl_client_t* client, void* userdata);

/**
 * @brief Set server callbacks
 */
NL_API void nl_server_set_connect_cb(nl_server_t* server, nl_server_connect_cb cb, void* userdata);
NL_API void nl_server_set_disconnect_cb(nl_server_t* server, nl_server_disconnect_cb cb, void* userdata);

// =========================================
// Client API
// =========================================

/**
 * @brief Create a new client
 * @param config Client configuration
 * @return Client handle or NULL on failure
 */
NL_API nl_client_t* nl_client_create(nl_protocol_t protocol);

/**
 * @brief Connect to server
 * @param client Client handle
 * @param host Host to connect to
 * @param port Port to connect to
 * @return NL_OK on success
 */
NL_API int nl_client_connect(nl_client_t* client, const char* host, int port);

/**
 * @brief Disconnect from server
 * @param client Client handle
 */
NL_API void nl_client_disconnect(nl_client_t* client);

/**
 * @brief Destroy the client
 * @param client Client handle
 */
NL_API void nl_client_destroy(nl_client_t* client);

/**
 * @brief Send data to server
 * @param client Client handle
 * @param data Data to send
 * @param len Data length
 * @return Number of bytes sent or error code
 */
NL_API int nl_client_send(nl_client_t* client, const void* data, size_t len);

/**
 * @brief Receive data from server
 * @param client Client handle
 * @param buf Output buffer
 * @param len Buffer length
 * @return Number of bytes received or error code
 */
NL_API int nl_client_recv(nl_client_t* client, void* buf, size_t len);

/**
 * @brief Send UDP data to specific host
 */
NL_API int nl_client_send_to(nl_client_t* client, const char* host, int port, const void* data, size_t len);

/**
 * @brief Receive UDP data from any host
 */
NL_API int nl_client_recv_from(nl_client_t* client, void* buf, size_t len, char* from_addr, size_t addr_len, int* from_port);

/**
 * @brief Set socket option for client
 */
NL_API int nl_client_set_option(nl_client_t* client, nl_socket_option_t option, int value);

/**
 * @brief Get socket option for client
 */
NL_API int nl_client_get_option(nl_client_t* client, nl_socket_option_t option, int* value);

/**
 * @brief Get client socket fd
 */
NL_API int nl_client_get_fd(nl_client_t* client);

// =========================================
// Config API (Internal)
// =========================================

/**
 * @brief Create config handle
 */
NL_API nl_config_t* nl_config_create(void);

/**
 * @brief Destroy config handle
 */
NL_API void nl_config_destroy(nl_config_t* config);

/**
 * @brief Load config from file
 */
NL_API int nl_config_load(nl_config_t* config, const char* path);

/**
 * @brief Save config to file
 */
NL_API int nl_config_save(nl_config_t* config, const char* path);

// =========================================
// Buffer API
// =========================================

/**
 * @brief Create a new buffer
 */
NL_API nl_buffer_t* nl_buffer_create(size_t capacity);

/**
 * @brief Destroy a buffer
 */
NL_API void nl_buffer_destroy(nl_buffer_t* buffer);

/**
 * @brief Clear a buffer
 */
NL_API void nl_buffer_clear(nl_buffer_t* buffer);

/**
 * @brief Write data to buffer
 */
NL_API size_t nl_buffer_write(nl_buffer_t* buffer, const void* data, size_t len);

/**
 * @brief Read data from buffer
 */
NL_API size_t nl_buffer_read(nl_buffer_t* buffer, void* data, size_t len);

/**
 * @brief Get buffer size
 */
NL_API size_t nl_buffer_size(nl_buffer_t* buffer);

// =========================================
// WebSocket API (use include/optimize/netleaf_websocket.h instead)
// =========================================

/**
 * @brief WebSocket connection handler type
 */
typedef void (*nl_ws_handler)(nl_ws_conn_t* ws, const char* msg, size_t len, void* userdata);

// =========================================
// HTTP API (use include/optimize/netleaf_http.h for full API)
// =========================================

// Note: HTTP types and functions are defined in include/optimize/netleaf_http.h

// =========================================
// MQTT API (Optional - include netleaf_mqtt.h to use)
// =========================================
// To use MQTT functionality, include: #include "netleaf_mqtt.h"
// MQTT module is optional and can be enabled with BUILD_MQTT=ON

// =========================================
// Async API
// =========================================

/**
 * @brief Execute callback in async context
 */
NL_API void nl_async_exec(void (*cb)(void*), void* userdata);

/**
 * @brief Schedule timer callback
 */
NL_API void* nl_timer_create(int interval_ms, void (*cb)(void*), void* userdata);
NL_API void nl_timer_destroy(void* timer);

// =========================================
// Encoding/Decoding API
// =========================================

/**
 * @brief URL encode
 */
NL_API char* nl_url_encode(const char* input);

/**
 * @brief URL decode
 */
NL_API char* nl_url_decode(const char* input);

/**
 * @brief JSON escape
 */
NL_API char* nl_json_escape(const char* input);

/**
 * @brief Parse JSON string, returns json handle (use nl_json_destroy to free)
 */
NL_API void* nl_json_parse(const char* json_str, nl_status_t* error_code, int* error_line, int* error_col);

/**
 * @brief Parse JSON from file, returns json handle (use nl_json_destroy to free)
 */
NL_API void* nl_json_parse_file(const char* file_path, nl_status_t* error_code);

/**
 * @brief Destroy JSON handle
 */
NL_API void nl_json_destroy(void* json);

/**
 * @brief Get JSON value type (nl_json_type_t)
 */
NL_API int nl_json_get_type(void* json);

/**
 * @brief Get JSON boolean value
 */
NL_API int nl_json_get_bool(void* json);

/**
 * @brief Get JSON integer value
 */
NL_API int64_t nl_json_get_int(void* json);

/**
 * @brief Get JSON double value
 */
NL_API double nl_json_get_double(void* json);

/**
 * @brief Get JSON string value
 */
NL_API const char* nl_json_get_string(void* json);

/**
 * @brief Get JSON array element count
 */
NL_API size_t nl_json_array_size(void* json);

/**
 * @brief Get JSON array element by index
 */
NL_API void* nl_json_array_get(void* json, size_t index);

/**
 * @brief Get JSON object member by key
 */
NL_API void* nl_json_object_get(void* json, const char* key);

/**
 * @brief Check whether a JSON object contains a key
 */
NL_API int nl_json_has_key(void* json, const char* key);

/**
 * @brief Serialize a JSON handle to a string (caller frees)
 */
NL_API char* nl_json_stringify(void* json, int pretty);

/**
 * @brief Serialize a JSON handle to a file
 */
NL_API int nl_json_save_file(void* json, const char* file_path, int pretty);

/**
 * @brief Get a human-readable message for a JSON status code
 */
NL_API const char* nl_json_error_message(nl_status_t error_code);

// =========================================
// TOML API
// =========================================

/**
 * @brief Parse TOML string, returns toml handle (use nl_toml_destroy to free)
 */
NL_API void* nl_toml_parse(const char* toml_str, nl_status_t* error_code, int* error_line, int* error_col);

/**
 * @brief Parse TOML from file, returns toml handle (use nl_toml_destroy to free)
 */
NL_API void* nl_toml_parse_file(const char* file_path, nl_status_t* error_code);

/**
 * @brief Destroy TOML handle
 */
NL_API void nl_toml_destroy(void* toml);

/**
 * @brief Get TOML value type (nl_toml_type_t)
 */
NL_API int nl_toml_get_type(void* toml);

/**
 * @brief Get TOML boolean value
 */
NL_API int nl_toml_get_bool(void* toml);

/**
 * @brief Get TOML integer value
 */
NL_API int64_t nl_toml_get_int(void* toml);

/**
 * @brief Get TOML float value
 */
NL_API double nl_toml_get_float(void* toml);

/**
 * @brief Get TOML string value
 */
NL_API const char* nl_toml_get_string(void* toml);

/**
 * @brief Get TOML array element count
 */
NL_API size_t nl_toml_array_size(void* toml);

/**
 * @brief Get TOML array element by index
 */
NL_API void* nl_toml_array_get(void* toml, size_t index);

/**
 * @brief Get TOML table member by key
 */
NL_API void* nl_toml_table_get(void* toml, const char* key);

/**
 * @brief Check whether a TOML table contains a key
 */
NL_API int nl_toml_has_key(void* toml, const char* key);

/**
 * @brief Serialize a TOML handle to a string (caller frees)
 */
NL_API char* nl_toml_stringify(void* toml);

/**
 * @brief Serialize a TOML handle to a file
 */
NL_API int nl_toml_save_file(void* toml, const char* file_path);

/**
 * @brief Get a human-readable message for a TOML status code
 */
NL_API const char* nl_toml_error_message(nl_status_t error_code);

/**
 * @brief Base64 encode
 */
NL_API char* nl_base64_encode(const char* input, size_t len);

/**
 * @brief Base64 decode
 */
NL_API char* nl_base64_decode(const char* input, size_t* out_len);

// =========================================
// Utility API
// =========================================

/**
 * @brief Get system info
 */
typedef struct {
    int cpu_count;
    size_t total_memory;
    size_t free_memory;
    char hostname[256];
} nl_sysinfo_t;

NL_API nl_sysinfo_t nl_get_sysinfo(void);

/**
 * @brief Print server status
 */
NL_API void nl_print_status(nl_server_t* server);

/**
 * @brief Print module information
 */
NL_API void nl_print_modules(void);

// =========================================
// Logging API
// =========================================

/**
 * @brief Log level enumeration
 */
typedef enum {
    NL_LOG_DEBUG = 0,
    NL_LOG_INFO,
    NL_LOG_WARN,
    NL_LOG_ERROR
} nl_log_level_t;

/**
 * @brief Log callback type
 */
typedef void (*nl_log_callback)(nl_log_level_t level, const char* message, void* userdata);

/**
 * @brief Set log level
 */
NL_API void nl_log_set_level(nl_log_level_t level);

/**
 * @brief Set log callback
 */
NL_API void nl_log_set_callback(nl_log_callback callback, void* userdata);

/**
 * @brief Log a message
 */
NL_API void nl_log(nl_log_level_t level, const char* format, ...);

/**
 * @brief Log debug message
 */
NL_API void nl_log_debug(const char* format, ...);

/**
 * @brief Log info message
 */
NL_API void nl_log_info(const char* format, ...);

/**
 * @brief Log warning message
 */
NL_API void nl_log_warn(const char* format, ...);

/**
 * @brief Log error message
 */
NL_API void nl_log_error(const char* format, ...);

// =========================================
// Debug API
// =========================================

/**
 * @brief Enable/disable debug mode
 */
NL_API void nl_debug_enable(int enable);

/**
 * @brief Check if debug mode is enabled
 */
NL_API int nl_debug_is_enabled(void);

// =========================================
// Memory/RAM Info API
// =========================================

/**
 * @brief RAM unit enumeration
 */
typedef enum {
    NL_RAM_UNIT_BINARY = 1024,      // Binary units (1024 bytes)
    NL_RAM_UNIT_DECIMAL = 1000      // Decimal units (1000 bytes)
} nl_ram_unit_t;

/**
 * @brief Get RAM information
 */
typedef struct {
    size_t total;
    size_t free;
    size_t used;
    double usage_percent;
} nl_ram_info_t;

NL_API nl_ram_info_t nl_get_ram_info(nl_ram_unit_t unit);

// =========================================
// Web Server API (Internal)
// =========================================

/**
 * @brief Create WebSocket connection handle
 */
NL_API nl_ws_conn_t* nl_ws_conn_create(int sock);

/**
 * @brief Create system info
 */
NL_API nl_sysinfo_t nl_sys_info_create(void);

/**
 * @brief Create a web server
 */
NL_API nl_web_server_t* nl_web_create(int port);

/**
 * @brief Destroy a web server
 */
NL_API void nl_web_destroy(nl_web_server_t* server);

/**
 * @brief Set web server encoding
 */
NL_API void nl_web_set_encoding(nl_web_server_t* server, const char* encoding);

/**
 * @brief Get route count
 */
NL_API int nl_web_get_route_count(nl_web_server_t* server);

/**
 * @brief Start a web server
 */
NL_API int nl_web_start(nl_web_server_t* server);

/**
 * @brief Stop a web server
 */
NL_API void nl_web_stop(nl_web_server_t* server);

/**
 * @brief Add a route to web server
 */
NL_API int nl_web_add_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type);

/**
 * @brief Remove a route from web server
 */
NL_API int nl_web_remove_route(nl_web_server_t* server, const char* path);

/**
 * @brief Add HTML content to web server
 */
NL_API void nl_web_add_html(nl_web_server_t* server, const char* path, const char* html);

/**
 * @brief Add HTTP 302 redirect
 */
NL_API void nl_web_add_redirect_302(nl_web_server_t* server, const char* path, const char* target_url);

/**
 * @brief Add Vue content to web server
 */
NL_API void nl_web_add_vue(nl_web_server_t* server, const char* path, const char* vue_code);

/**
 * @brief Add JSON content to web server
 */
NL_API void nl_web_add_json(nl_web_server_t* server, const char* path, const char* json);

/**
 * @brief Enable or disable automatic encoding handling
 */
NL_API void nl_web_enable_auto_encoding(nl_web_server_t* server, int enable);

/**
 * @brief Set the fallback encoding for the web server
 */
NL_API void nl_web_set_fallback_encoding(nl_web_server_t* server, const char* encoding);

/**
 * @brief List registered route paths into the caller-provided array
 */
NL_API int nl_web_list_routes(nl_web_server_t* server, char** paths, int max_paths);

/**
 * @brief Update the content and content type of an existing route
 */
NL_API int nl_web_update_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type);

/**
 * @brief Set the default redirect type (301/302)
 */
NL_API void nl_web_set_redirect_type(nl_web_server_t* server, nl_redirect_type_t type);

/**
 * @brief Get the default redirect type
 */
NL_API nl_redirect_type_t nl_web_get_redirect_type(nl_web_server_t* server);

/**
 * @brief Create error page
 */
NL_API char* nl_render_error_page(const char* template_content, nl_error_page_vars_t* vars);

/**
 * @brief Make error response
 */
NL_API char* nl_make_error_response(int status_code, const char* error_message, const char* requested_path, const char* suggestion);

// =========================================
// File Server API
// =========================================

NL_API nl_file_server_t* nl_file_server_create(const char* directory, int port);
NL_API void nl_file_server_destroy(nl_file_server_t* server);
NL_API void nl_file_server_stop(nl_file_server_t* server);
NL_API int nl_file_server_start(nl_file_server_t* server);
NL_API void nl_file_server_set_index(nl_file_server_t* server, const char* index_file);
NL_API void nl_file_server_set_easter_egg(nl_file_server_t* server, int enable);

// =========================================
// Router API
// =========================================

NL_API nl_router_t* nl_router_create(void);
NL_API void nl_router_destroy(nl_router_t* router);
NL_API void nl_router_add_route(nl_router_t* router, const char* path, nl_http_method_t method, nl_http_handler_t handler, void* user_data);
NL_API int nl_router_remove_route(nl_router_t* router, const char* path, nl_http_method_t method);
NL_API int nl_router_handle_request(nl_router_t* router, const char* path, nl_http_method_t method, const char* body, size_t body_size, char** response, size_t* response_size);
NL_API void nl_router_set_static_dir(nl_router_t* router, const char* dir);

// =========================================
// Module API
// =========================================

/**
 * @brief Initialize all modules
 */
NL_API int nl_modules_init(void);

/**
 * @brief Shutdown all modules
 */
NL_API int nl_modules_shutdown(void);

/**
 * @brief Get module count
 */
NL_API int nl_get_module_count(void);

/**
 * @brief Get module by type
 */
NL_API nl_module_info_t* nl_get_module(nl_module_type_t type);

// =========================================
// Encoding API
// =========================================

/**
 * @brief Convert encoding
 */
NL_API char* nl_encoding_convert(const char* input, size_t input_len, const char* from_encoding, const char* to_encoding);

/**
 * @brief Get system default encoding
 */
NL_API const char* nl_encoding_get_system_default(void);

/**
 * @brief Detect encoding of input data
 */
NL_API const char* nl_encoding_detect(const char* input, size_t input_len);

/**
 * @brief Output text to console using the given encoding
 */
NL_API void nl_encoding_console_output(const char* text, const char* encoding);

/**
 * @brief Convert HTML buffer to the target encoding (caller frees)
 */
NL_API char* nl_encoding_html_convert(const char* html, size_t html_len, const char* target_encoding);

/**
 * @brief Check whether the input contains simplified Chinese characters
 */
NL_API int nl_encoding_is_simplified_chinese(const char* input, size_t input_len);

/**
 * @brief Check whether the input contains traditional Chinese characters
 */
NL_API int nl_encoding_is_traditional_chinese(const char* input, size_t input_len);

// =========================================
// System Info API
// =========================================

/**
 * @brief Clear system info cache
 */
NL_API void nl_sys_info_clear_cache(void);

/**
 * @brief Get OS name
 */
NL_API const char* nl_sys_info_get_os_name(void);

/**
 * @brief Get system architecture
 */
NL_API const char* nl_sys_info_get_architecture(void);

/**
 * @brief Get CPU model
 */
NL_API const char* nl_sys_info_get_cpu_model(void);

/**
 * @brief Get total RAM in bytes
 */
NL_API int64_t nl_sys_info_get_total_ram(void);

/**
 * @brief Get runtime version
 */
NL_API const char* nl_sys_info_get_runtime_version(void);

/**
 * @brief Check whether the system info module is loaded
 */
NL_API int nl_sys_info_is_loaded(void);

// =========================================
// Logging API
// =========================================

/**
 * @brief Check if a module is available
 * @param module_name Module name (e.g., "autoroute", "autocomplete", "errorpage", "lang", "vue", "mqtt")
 * @return 1 if available, 0 if not
 */
NL_API int nl_module_available(const char* module_name);

/**
 * @brief Get all available modules
 * @param modules Output array of module names
 * @param max_count Maximum number of modules to retrieve
 * @return Number of available modules
 */
NL_API int nl_get_modules(const char** modules, int max_count);

// =========================================
// Lazy Loading API
// =========================================

/**
 * @brief Enable or disable the lazy loading subsystem
 */
NL_API void nl_lazy_enable(int enable);

/**
 * @brief Enable lazy loading for a specific module
 */
NL_API void nl_lazy_enable_module(nl_lazy_module_t module);

/**
 * @brief Disable lazy loading for a specific module
 */
NL_API void nl_lazy_disable_module(nl_lazy_module_t module);

/**
 * @brief Check whether lazy loading is enabled for a module
 */
NL_API int nl_lazy_is_enabled(nl_lazy_module_t module);

/**
 * @brief Clear all cached lazy-loaded data
 */
NL_API void nl_lazy_clear_all_cache(void);

/**
 * @brief Preload a module
 */
NL_API void nl_lazy_preload_module(nl_lazy_module_t module);

/**
 * @brief Get the load status of a module
 */
NL_API nl_lazy_status_t nl_lazy_get_module_status(nl_lazy_module_t module);

/**
 * @brief Check whether a module is loaded
 */
NL_API int nl_lazy_is_module_loaded(nl_lazy_module_t module);

/**
 * @brief Set the lazy loader thread count
 */
NL_API int nl_lazy_set_thread_count(int count);

/**
 * @brief Get the lazy loader thread count
 */
NL_API int nl_lazy_get_thread_count(void);

// =========================================
// Compatibility Macros (v2.2.x series)
// =========================================

// Legacy websocket aliases (use netleaf_websocket.h for full API)
// Note: nl_ws_server_create and nl_ws_client_create are in include/optimize/netleaf_websocket.h

// Legacy http aliases (use netleaf_http.h for full API)
// Note: nl_http_server_create is in include/optimize/netleaf_http.h

#endif // NETLEAF_H
