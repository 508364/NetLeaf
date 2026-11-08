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

#define NL_LINKAGG_VERSION "3.0.1"
#define NL_LINKAGG_VERSION_MAJOR 3
#define NL_LINKAGG_VERSION_MINOR 0
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
    NL_LINKAGG_ERROR_ID_FORMAT = -10,             // Invalid ID format (not xxx.xxx)
    NL_LINKAGG_ERROR_TIMEOUT = -11,               // 宿主拉取/响应超时（已重试）
    NL_LINKAGG_ERROR_ABORTED = -12                // 请求已被客户端放弃
} nl_linkagg_error_t;

// 反向数据池默认参数（定义在此以便内部实现直接使用）
#define NL_LINKAGG_PULL_DEFAULT_TIMEOUT_MS 2000
#define NL_LINKAGG_PULL_DEFAULT_RETRIES    1
#define NL_LINKAGG_MAX_REQUEST_BYTES       (8u * 1024u * 1024u)

// =========================================
// Module Information
// =========================================

// LinkAgg module capabilities
#define NL_LINKAGG_CAPABILITIES \
    (NL_CAP_SERVER | NL_CAP_THREAD_SAFE | \
     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX | NL_CAP_PLATFORM_MACOS)

// LinkAgg module info structure (defined in implementation)
NL_LINKAGG_API nl_module_info_t* nl_lagg_get_module_info(void);

// LinkAgg extension entry point (for dynamic loading)
NL_LINKAGG_API nl_extension_info_t* nl_lagg_get_extension_info(void);

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

// Opaque server type (full definition in netleaf_linkagg_internal.h)
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

// =========================================
// Pull-based data pool (v3.1)
// =========================================
// 链路聚合反向模型：NetLeaf 只把接入请求放入“数据池”，由服务端宿主程序
// 主动按端点(endpoint)拉取属于自己的请求，处理完再回送响应。
// 拉取超时后重试一次；仍超时则标记该端点异常（健康拉取后自动恢复）。

// 待处理请求句柄（由 nl_lagg_pull 返回，必须通过 respond/free 归还）
typedef struct nl_lagg_request nl_lagg_request_t;

// 主动拉取归属于 endpoint 的请求；endpoint 为 NULL 或 "*" 表示任意端点。
// 返回 NULL 表示超时/无数据（不视为错误）。host 需调用 request_respond/free。
NL_LINKAGG_API nl_lagg_request_t* nl_lagg_pull(nl_lagg_server_t* server,
                                               const char* endpoint,
                                               int timeout_ms);

// 读取请求元信息与原始请求字节（生命周期由句柄管理，勿自行释放）
NL_LINKAGG_API int nl_lagg_request_get_id(nl_lagg_request_t* req, uint64_t* out_id);
NL_LINKAGG_API const char* nl_lagg_request_get_endpoint(nl_lagg_request_t* req);
NL_LINKAGG_API const void* nl_lagg_request_get_data(nl_lagg_request_t* req, size_t* out_len);

// 回送响应（内部拷贝）；成功后客户端结束等待。返回 NL_LINKAGG_OK 或错误码。
NL_LINKAGG_API int nl_lagg_request_respond(nl_lagg_request_t* req,
                                           const void* data, size_t len);
// 放弃请求（宿主无法处理时调用），客户端会收到 503。
NL_LINKAGG_API void nl_lagg_request_free(nl_lagg_request_t* req);

// 配置客户端等待宿主响应的超时与重试次数（默认 2000ms / 1 次）
NL_LINKAGG_API void nl_lagg_set_pull_timeout(nl_lagg_server_t* server,
                                             int timeout_ms, int retries);
NL_LINKAGG_API int nl_lagg_get_pull_timeout(nl_lagg_server_t* server);

// 查询端点健康状态：1=正常，0=异常，-1=端点不存在
NL_LINKAGG_API int nl_lagg_get_backend_health(nl_lagg_server_t* server,
                                              const char* endpoint);

// 跨进程拉取端点（宿主通过 netleaf_ipc 连接该地址，使用 FETCH/RESP 协议）。
// 返回写入 buf 的端点字符串；buf 为 NULL 时仅返回内部缓存地址。
NL_LINKAGG_API const char* nl_lagg_get_pool_endpoint(nl_lagg_server_t* server,
                                                     char* buf, size_t buf_len);

// 启用/关闭跨进程数据池服务（默认启用；关闭后仅支持进程内 nl_lagg_pull）
NL_LINKAGG_API int nl_lagg_set_pool_service(nl_lagg_server_t* server, int enable);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_LINKAGG_H
