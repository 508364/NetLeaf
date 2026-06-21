#ifndef NETLEAF_LINKAGG_H
#define NETLEAF_LINKAGG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

// DLL export/import macros
#ifdef _WIN32
    #ifdef NL_LINKAGG_EXPORTS
        #define NL_LINKAGG_API __declspec(dllexport)
    #elif defined(NL_LINKAGG_STATIC)
        #define NL_LINKAGG_API
    #else
        #define NL_LINKAGG_API __declspec(dllimport)
    #endif
#else
    #define NL_LINKAGG_API
#endif

#define NL_LINKAGG_VERSION "2.2.1"
#define NL_LINKAGG_VERSION_MAJOR 2
#define NL_LINKAGG_VERSION_MINOR 2
#define NL_LINKAGG_VERSION_PATCH 1

// =========================================
// Limits
// =========================================

// Maximum number of backends (0-511)
#define NL_LINKAGG_MAX_BACKENDS 512

// ID format: xxx.xxx (must contain exactly one dot)
#define NL_LINKAGG_ID_MIN_LENGTH 3  // x.x minimum
#define NL_LINKAGG_ID_MAX_LENGTH 256

// =========================================
// Error Codes
// =========================================

typedef enum {
    NL_LINKAGG_OK = 0,
    NL_LINKAGG_ERROR = -1,
    NL_LINKAGG_ERROR_PLATFORM_NOT_SUPPORTED = -2,
    NL_LINKAGG_ERROR_MEMORY = -3,
    NL_LINKAGG_ERROR_INVALID_PARAM = -4,
    NL_LINKAGG_ERROR_PORT_BIND_FAILED = -5,
    NL_LINKAGG_ERROR_BACKEND_FAILED = -6,
    NL_LINKAGG_ERROR_NOT_STARTED = -7,
    NL_LINKAGG_ERROR_TOO_MANY_BACKENDS = -8,     // Exceeded 512 backends
    NL_LINKAGG_ERROR_ID_CONFLICT = -9,            // ID already in use
    NL_LINKAGG_ERROR_ID_FORMAT = -10              // Invalid ID format (not xxx.xxx)
} nl_linkagg_error_t;

// =========================================
// Module Information
// =========================================

// LinkAgg module capabilities
#define NL_LINKAGG_CAPABILITIES \
    (NL_CAP_SERVER | NL_CAP_THREAD_SAFE | \
     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX)

// LinkAgg module info structure (defined in implementation)
NL_LINKAGG_API nl_module_info_t* nl_lagg_get_module_info(void);

// Check if LinkAgg is available on current platform
NL_LINKAGG_API int nl_lagg_is_available(void);

// Get LinkAgg version string
NL_LINKAGG_API const char* nl_lagg_version(void);

// =========================================
// Load Balancing Policy
// =========================================

typedef enum {
    NL_POLICY_ROUND_ROBIN,
    NL_POLICY_RANDOM,
    NL_POLICY_LEAST_CONNECTIONS,
    NL_POLICY_WEIGHTED_ROUND_ROBIN
} nl_lagg_policy_t;

// Backend types
typedef enum {
    NL_BACKEND_HTTP,
    NL_BACKEND_IPC
} nl_backend_type_t;

// Opaque server type
typedef struct nl_lagg_server nl_lagg_server_t;

// Callbacks for connection lifecycle
typedef void (*nl_lagg_on_connect_cb)(int client_fd, void* user_data);
typedef void (*nl_lagg_on_disconnect_cb)(int client_fd, void* user_data);

// =========================================
// Link Aggregation Server API
// =========================================

NL_LINKAGG_API nl_lagg_server_t* nl_lagg_create(int port);
NL_LINKAGG_API void nl_lagg_destroy(nl_lagg_server_t* server);
NL_LINKAGG_API int nl_lagg_start(nl_lagg_server_t* server);
NL_LINKAGG_API void nl_lagg_stop(nl_lagg_server_t* server);

NL_LINKAGG_API int nl_lagg_add_http_backend(nl_lagg_server_t* server, const char* host, int port, int weight);
NL_LINKAGG_API int nl_lagg_add_ipc_backend(nl_lagg_server_t* server, const char* endpoint, int weight);
NL_LINKAGG_API int nl_lagg_remove_backend(nl_lagg_server_t* server, const char* endpoint);

// Port ID management (format: xxx.xxx)
NL_LINKAGG_API int nl_lagg_set_port_id(nl_lagg_server_t* server, const char* id);
NL_LINKAGG_API const char* nl_lagg_get_port_id(nl_lagg_server_t* server);
NL_LINKAGG_API int nl_lagg_validate_id_format(const char* id);

NL_LINKAGG_API void nl_lagg_set_policy(nl_lagg_server_t* server, nl_lagg_policy_t policy);
NL_LINKAGG_API void nl_lagg_set_on_connect(nl_lagg_server_t* server, nl_lagg_on_connect_cb cb, void* user_data);
NL_LINKAGG_API void nl_lagg_set_on_disconnect(nl_lagg_server_t* server, nl_lagg_on_disconnect_cb cb, void* user_data);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_LINKAGG_H
