#ifndef NETLEAF_HTTP_H
#define NETLEAF_HTTP_H

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

/* ============================================================
 * 短名转接层（向后兼容）
 *
 * 原长名接口（nl_http_* / nl_http2_* / nl_http3_*）标记为 deprecated，
 * 直接调用时编译器产生告警但仍可编译运行；一一对应的短名（nlh_*）
 * 为推荐用法，无告警。短名实现见 src/http/netleaf_http_shortnames.c，
 * 随核心库 netleaf_core 导出，签名与原长名完全一致。
 * ============================================================ */
/* 库自身源码构建时（内部 .c 调用原长名实现体）关闭 deprecated 告警，
 * 仅对外部用户调用产生告警。内部 .c 在 include 本头前 #define NL_HTTP_INTERNAL_BUILD。 */
#ifndef NL_HTTP_INTERNAL_BUILD
#ifdef _WIN32
    /* MSVC / MinGW(GCC) 均支持 __declspec(deprecated) */
    #define NL_HTTP_DEPRECATED __declspec(deprecated)
#elif defined(__GNUC__) || defined(__clang__)
    #define NL_HTTP_DEPRECATED __attribute__((deprecated))
#else
    #define NL_HTTP_DEPRECATED
#endif
#else
    #define NL_HTTP_DEPRECATED
#endif

// HTTPS 编译门控：仅在启用 NL_HTTPS_ENABLE 时暴露 TLS 相关 API。
// 编译时由 BUILD_TLS / BUILD_TLS3 CMake 选项驱动（见 src/tls/CMakeLists.txt、
// src/tls3/CMakeLists.txt 链接逻辑）。
// 未定义该宏时，nl_http_server_enable_tls / nl_http_client_connect 的
// HTTPS 路径退化为桩函数，返回 NL_HTTP_TLS_UNAVAILABLE(-100)。
// 底层 TLS 后端由 NL_HTTPS_USE_TLS3 切换：
//   - 未定义：tls2（mbedTLS 2.28.x，TLS 1.0-1.3）
//   - 定义：  tls3（mbedTLS 3.x，TLS 1.2/1.3）
#ifdef NL_HTTPS_ENABLE
    #ifdef NL_HTTPS_USE_TLS3
        #include "netleaf_tls3.h"
    #else
        #include "netleaf_tls2.h"
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nl_http_request nl_http_request_t;
typedef struct nl_http_response nl_http_response_t;
typedef struct nl_http_server nl_http_server_t;
typedef struct nl_http2_server nl_http2_server_t;
typedef struct nl_http3_server nl_http3_server_t;
typedef struct nl_http2_stream nl_http2_stream_t;

// ============================================================
// HTTPS 支持（可选：编译时定义 NL_HTTPS_ENABLE 并链接 netleaf_tls）
// ============================================================

/* 结果码：-100 = TLS 不可用（未编译或握手失败前返回） */
#define NL_HTTP_TLS_UNAVAILABLE (-100)

/**
 * HTTPS 服务端 TLS 配置。
 * 仅需填充 cert_file/key_file；CA/对端校验等字段留 NULL 即可。
 */
typedef struct nl_http_tls_server_cfg {
    const char* cert_file;   /**< 服务端证书 (PEM) */
    const char* key_file;    /**< 服务端私钥 (PEM) */
    const char* key_pass;    /**< 私钥口令；NULL = 未加密 */
    int         client_auth; /**< 1 = 要求客户端证书 (mTLS)；0 = 单向 TLS */
} nl_http_tls_server_cfg_t;

/**
 * HTTPS 客户端 TLS 配置。
 * ca_file 为 NULL 时不校验服务端证书（适用于自签场景）；
 * 生产环境应始终提供 ca_file 并启用 hostname 校验。
 */
typedef struct nl_http_tls_client_cfg {
    const char* ca_file;    /**< CA 证书文件；NULL = 不校验 */
    const char* cert_file;  /**< 客户端证书（mTLS 时提供）；NULL = 无 */
    const char* key_file;   /**< 客户端私钥；NULL = 无 */
    int         verify;     /**< 1 = 校验服务端证书；0 = 跳过 */
} nl_http_tls_client_cfg_t;

/**
 * 启动 HTTPS 服务端（在已有 nl_http_server_t 上附加 TLS 层）。
 * 调用后 server_thread 中每次 accept 完成后先执行 TLS 握手。
 * 需在 nl_http_server_start() 之前调用。
 * 仅在构建时定义 NL_HTTPS_ENABLE（链接 netleaf_tls）时可用，
 * 否则直接返回 NL_HTTP_TLS_UNAVAILABLE。
 * @return 0 = 成功；负数 = 错误（含 TLS 不可用）
 * @deprecated 推荐使用短名 nlh_server_enable_tls（签名一致，无告警）。
 */
NL_API NL_HTTP_DEPRECATED int nl_http_server_enable_tls(nl_http_server_t* server,
                                     const nl_http_tls_server_cfg_t* cfg);

/**
 * 创建 HTTPS 客户端连接并完成 TLS 握手。
 * 仅在 NL_HTTPS_ENABLE 构建时可用，否则返回 NULL。
 * @return nl_http_client_t* 句柄；NULL = 失败（含 TLS 不可用）
 * @deprecated 推荐使用短名 nlh_client_connect（签名一致，无告警）。
 */
typedef struct nl_http_client nl_http_client_t;

NL_API NL_HTTP_DEPRECATED nl_http_client_t* nl_http_client_connect(const char* host, int port,
                                                 const nl_http_tls_client_cfg_t* cfg);
NL_API NL_HTTP_DEPRECATED void nl_http_client_close(nl_http_client_t* client);
/**
 * 发送 HTTP/1.1 请求并读取完整响应，响应 body 写入 out_buf。
 * 仅在 NL_HTTPS_ENABLE 构建时可用，否则返回 NL_HTTP_TLS_UNAVAILABLE。
 * @return 200..599 = HTTP 状态码；负数 = 错误（NL_HTTP_TLS_UNAVAILABLE 等）
 * @deprecated 推荐使用短名 nlh_client_request（签名一致，无告警）。
 */
NL_API NL_HTTP_DEPRECATED int nl_http_client_request(nl_http_client_t* client,
                                   const char* method,
                                   const char* path,
                                   const char* body, size_t body_len,
                                   char* out_buf, size_t out_buf_size,
                                   size_t* out_body_len);

typedef enum {
    NL_HTTP_GET,
    NL_HTTP_POST,
    NL_HTTP_PUT,
    NL_HTTP_DELETE,
    NL_HTTP_HEAD,
    NL_HTTP_OPTIONS,
    NL_HTTP_PATCH,
    NL_HTTP_UNKNOWN
} nl_http_method_t;

typedef enum {
    NL_HTTP_VERSION_1_0,
    NL_HTTP_VERSION_1_1,
    NL_HTTP_VERSION_2,
    NL_HTTP_VERSION_3
} nl_http_version_t;

typedef enum {
    NL_H2_FRAME_DATA = 0x0,
    NL_H2_FRAME_HEADERS = 0x1,
    NL_H2_FRAME_PRIORITY = 0x2,
    NL_H2_FRAME_RST_STREAM = 0x3,
    NL_H2_FRAME_SETTINGS = 0x4,
    NL_H2_FRAME_PUSH_PROMISE = 0x5,
    NL_H2_FRAME_PING = 0x6,
    NL_H2_FRAME_GOAWAY = 0x7,
    NL_H2_FRAME_WINDOW_UPDATE = 0x8,
    NL_H2_FRAME_CONTINUATION = 0x9
} nl_h2_frame_type_t;

// HTTP/3 (QUIC) related definitions
typedef enum {
    NL_H3_FRAME_DATA = 0x0,
    NL_H3_FRAME_HEADERS = 0x1,
    NL_H3_FRAME_CANCEL_PUSH = 0x3,
    NL_H3_FRAME_SETTINGS = 0x4,
    NL_H3_FRAME_PUSH_PROMISE = 0x5,
    NL_H3_FRAME_GOAWAY = 0x7,
    NL_H3_FRAME_MAX_PUSH_ID = 0xD
} nl_h3_frame_type_t;

// QUIC packet types
typedef enum {
    NL_QUIC_PACKET_INITIAL = 0x0,
    NL_QUIC_PACKET_0RTT = 0x1,
    NL_QUIC_PACKET_HANDSHAKE = 0x2,
    NL_QUIC_PACKET_RETRY = 0x3,
    NL_QUIC_PACKET_VERSION_NEGOTIATION = 0x4,
    NL_QUIC_PACKET_SHORT = 0x5
} nl_quic_packet_type_t;

// QUIC error codes
typedef enum {
    NL_QUIC_NO_ERROR = 0x0,
    NL_QUIC_INTERNAL_ERROR = 0x1,
    NL_QUIC_CONNECTION_REFUSED = 0x2,
    NL_QUIC_FLOW_CONTROL_ERROR = 0x3,
    NL_QUIC_STREAM_LIMIT_ERROR = 0x4,
    NL_QUIC_STREAM_STATE_ERROR = 0x5,
    NL_QUIC_FINAL_SIZE_ERROR = 0x6,
    NL_QUIC_FRAME_ENCODING_ERROR = 0x7,
    NL_QUIC_TRANSPORT_PARAMETER_ERROR = 0x8,
    NL_QUIC_CONNECTION_ID_LIMIT_ERROR = 0x9,
    NL_QUIC_PROTOCOL_VIOLATION = 0xA,
    NL_QUIC_INVALID_TOKEN = 0xB,
    NL_QUIC_APPLICATION_ERROR = 0xC,
    NL_QUIC_CRYPTO_BUFFER_EXCEEDED = 0xD,
    NL_QUIC_KEY_UPDATE_ERROR = 0xE,
    NL_QUIC_AEAD_LIMIT_REACHED = 0xF
} nl_quic_error_code_t;

// QUIC transport parameter IDs
typedef enum {
    NL_QUIC_PARAM_ORIGINAL_DESTINATION_CONNECTION_ID = 0x0,
    NL_QUIC_PARAM_MAX_IDLE_TIMEOUT = 0x1,
    NL_QUIC_PARAM_STATELESS_RESET_TOKEN = 0x2,
    NL_QUIC_PARAM_MAX_UDP_PAYLOAD_SIZE = 0x3,
    NL_QUIC_PARAM_INITIAL_MAX_DATA = 0x4,
    NL_QUIC_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL = 0x5,
    NL_QUIC_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE = 0x6,
    NL_QUIC_PARAM_INITIAL_MAX_STREAM_DATA_UNI = 0x7,
    NL_QUIC_PARAM_INITIAL_MAX_STREAMS_BIDI = 0x8,
    NL_QUIC_PARAM_INITIAL_MAX_STREAMS_UNI = 0x9,
    NL_QUIC_PARAM_ACK_DELAY_EXPONENT = 0xA,
    NL_QUIC_PARAM_MAX_ACK_DELAY = 0xB,
    NL_QUIC_PARAM_DISABLE_ACTIVE_MIGRATION = 0xC,
    NL_QUIC_PARAM_PREFERRED_ADDRESS = 0xD,
    NL_QUIC_PARAM_ACTIVE_CONNECTION_ID_LIMIT = 0xE,
    NL_QUIC_PARAM_INITIAL_SOURCE_CONNECTION_ID = 0xF,
    NL_QUIC_PARAM_RETRY_SOURCE_CONNECTION_ID = 0x10
} nl_quic_transport_param_t;

// QUIC frame types
typedef enum {
    NL_QUIC_FRAME_PADDING = 0x0,
    NL_QUIC_FRAME_PING = 0x1,
    NL_QUIC_FRAME_ACK = 0x2,
    NL_QUIC_FRAME_ACK_ECN = 0x3,
    NL_QUIC_FRAME_RESET_STREAM = 0x4,
    NL_QUIC_FRAME_STOP_SENDING = 0x5,
    NL_QUIC_FRAME_CRYPTO = 0x6,
    NL_QUIC_FRAME_NEW_TOKEN = 0x7,
    NL_QUIC_FRAME_STREAM = 0x8,
    NL_QUIC_FRAME_STREAM_FIN = 0x9,
    NL_QUIC_FRAME_MAX_DATA = 0x10,
    NL_QUIC_FRAME_MAX_STREAM_DATA = 0x11,
    NL_QUIC_FRAME_MAX_STREAMS_BIDI = 0x12,
    NL_QUIC_FRAME_MAX_STREAMS_UNI = 0x13,
    NL_QUIC_FRAME_DATA_BLOCKED = 0x14,
    NL_QUIC_FRAME_STREAM_DATA_BLOCKED = 0x15,
    NL_QUIC_FRAME_STREAMS_BLOCKED_BIDI = 0x16,
    NL_QUIC_FRAME_STREAMS_BLOCKED_UNI = 0x17,
    NL_QUIC_FRAME_NEW_CONNECTION_ID = 0x18,
    NL_QUIC_FRAME_RETIRE_CONNECTION_ID = 0x19,
    NL_QUIC_FRAME_PATH_CHALLENGE = 0x1A,
    NL_QUIC_FRAME_PATH_RESPONSE = 0x1B,
    NL_QUIC_FRAME_CONNECTION_CLOSE = 0x1C,
    NL_QUIC_FRAME_HANDSHAKE_DONE = 0x1E
} nl_quic_frame_type_t;

typedef enum {
    NL_H2_SETTINGS_HEADER_TABLE_SIZE = 0x1,
    NL_H2_SETTINGS_ENABLE_PUSH = 0x2,
    NL_H2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    NL_H2_SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    NL_H2_SETTINGS_MAX_FRAME_SIZE = 0x5,
    NL_H2_SETTINGS_MAX_HEADER_LIST_SIZE = 0x6
} nl_h2_settings_id_t;

typedef enum {
    NL_H2_NO_ERROR = 0x0,
    NL_H2_PROTOCOL_ERROR = 0x1,
    NL_H2_INTERNAL_ERROR = 0x2,
    NL_H2_FLOW_CONTROL_ERROR = 0x3,
    NL_H2_SETTINGS_TIMEOUT = 0x4,
    NL_H2_STREAM_CLOSED = 0x5,
    NL_H2_FRAME_SIZE_ERROR = 0x6,
    NL_H2_REFUSED_STREAM = 0x7,
    NL_H2_CANCEL = 0x8,
    NL_H2_COMPRESSION_ERROR = 0x9,
    NL_H2_CONNECT_ERROR = 0xa,
    NL_H2_ENHANCE_YOUR_CALM = 0xb,
    NL_H2_INADEQUATE_SECURITY = 0xc,
    NL_H2_HTTP_1_1_REQUIRED = 0xd
} nl_h2_error_code_t;

typedef void (*nl_http_handler)(const nl_http_request_t* req, nl_http_response_t* resp, void* user_data);

/* 以下原长名接口均标记 NL_HTTP_DEPRECATED：调用仍可用但产生告警，
 * 推荐使用下方一一对应的 nlh_* 短名（签名一致、无告警）。 */
NL_API NL_HTTP_DEPRECATED nl_http_server_t* nl_http_server_create(int port);
NL_API NL_HTTP_DEPRECATED void nl_http_server_destroy(nl_http_server_t* server);
NL_API NL_HTTP_DEPRECATED int nl_http_server_start(nl_http_server_t* server);
NL_API NL_HTTP_DEPRECATED void nl_http_server_stop(nl_http_server_t* server);
NL_API NL_HTTP_DEPRECATED void nl_http_server_set_handler(nl_http_server_t* server, nl_http_handler handler, void* user_data);
NL_API NL_HTTP_DEPRECATED void nl_http_server_enable_http2(nl_http_server_t* server, int enable);
NL_API NL_HTTP_DEPRECATED void nl_http_server_enable_http3(nl_http_server_t* server, int enable);

NL_API NL_HTTP_DEPRECATED nl_http2_server_t* nl_http2_server_create(int port);
NL_API NL_HTTP_DEPRECATED void nl_http2_server_destroy(nl_http2_server_t* server);
NL_API NL_HTTP_DEPRECATED int nl_http2_server_start(nl_http2_server_t* server);
NL_API NL_HTTP_DEPRECATED void nl_http2_server_stop(nl_http2_server_t* server);
NL_API NL_HTTP_DEPRECATED void nl_http2_server_set_handler(nl_http2_server_t* server, nl_http_handler handler, void* user_data);

NL_API NL_HTTP_DEPRECATED nl_http3_server_t* nl_http3_server_create(int port);
NL_API NL_HTTP_DEPRECATED void nl_http3_server_destroy(nl_http3_server_t* server);
NL_API NL_HTTP_DEPRECATED int nl_http3_server_start(nl_http3_server_t* server);
NL_API NL_HTTP_DEPRECATED void nl_http3_server_stop(nl_http3_server_t* server);
NL_API NL_HTTP_DEPRECATED void nl_http3_server_set_handler(nl_http3_server_t* server, nl_http_handler handler, void* user_data);

NL_API NL_HTTP_DEPRECATED nl_http_method_t nl_http_request_get_method(const nl_http_request_t* req);
NL_API NL_HTTP_DEPRECATED nl_http_version_t nl_http_request_get_version(const nl_http_request_t* req);
NL_API NL_HTTP_DEPRECATED const char* nl_http_request_get_path(const nl_http_request_t* req);
NL_API NL_HTTP_DEPRECATED const char* nl_http_request_get_header(const nl_http_request_t* req, const char* name);
NL_API NL_HTTP_DEPRECATED const char* nl_http_request_get_body(const nl_http_request_t* req);
NL_API NL_HTTP_DEPRECATED size_t nl_http_request_get_body_size(const nl_http_request_t* req);

NL_API NL_HTTP_DEPRECATED void nl_http_response_set_status(nl_http_response_t* resp, int status);
NL_API NL_HTTP_DEPRECATED void nl_http_response_set_header(nl_http_response_t* resp, const char* name, const char* value);
NL_API NL_HTTP_DEPRECATED void nl_http_response_set_body(nl_http_response_t* resp, const char* body, size_t len);

/* ============================================================
 * 短名转接层（nlh_*）：与原长名一一对应、签名完全一致、无告警
 *
 * 实现在 src/http/netleaf_http_shortnames.c，随核心库 netleaf_core 导出。
 * 推荐新代码一律使用下列短名；原长名保留 deprecated 仅作过渡。
 * ============================================================ */
NL_API nl_http_server_t* nlh_server_create(int port);
NL_API void nlh_server_destroy(nl_http_server_t* server);
NL_API int nlh_server_start(nl_http_server_t* server);
NL_API void nlh_server_stop(nl_http_server_t* server);
NL_API void nlh_server_set_handler(nl_http_server_t* server, nl_http_handler handler, void* user_data);
NL_API void nlh_server_enable_http2(nl_http_server_t* server, int enable);
NL_API void nlh_server_enable_http3(nl_http_server_t* server, int enable);

NL_API nl_http2_server_t* nlh_h2_create(int port);
NL_API void nlh_h2_destroy(nl_http2_server_t* server);
NL_API int nlh_h2_start(nl_http2_server_t* server);
NL_API void nlh_h2_stop(nl_http2_server_t* server);
NL_API void nlh_h2_set_handler(nl_http2_server_t* server, nl_http_handler handler, void* user_data);

NL_API nl_http3_server_t* nlh_h3_create(int port);
NL_API void nlh_h3_destroy(nl_http3_server_t* server);
NL_API int nlh_h3_start(nl_http3_server_t* server);
NL_API void nlh_h3_stop(nl_http3_server_t* server);
NL_API void nlh_h3_set_handler(nl_http3_server_t* server, nl_http_handler handler, void* user_data);

NL_API nl_http_method_t nlh_req_method(const nl_http_request_t* req);
NL_API nl_http_version_t nlh_req_version(const nl_http_request_t* req);
NL_API const char* nlh_req_path(const nl_http_request_t* req);
NL_API const char* nlh_req_header(const nl_http_request_t* req, const char* name);
NL_API const char* nlh_req_body(const nl_http_request_t* req);
NL_API size_t nlh_req_body_size(const nl_http_request_t* req);

NL_API void nlh_resp_status(nl_http_response_t* resp, int status);
NL_API void nlh_resp_header(nl_http_response_t* resp, const char* name, const char* value);
NL_API void nlh_resp_body(nl_http_response_t* resp, const char* body, size_t len);

#ifdef NL_HTTPS_ENABLE
NL_API int nlh_server_enable_tls(nl_http_server_t* server,
                                 const nl_http_tls_server_cfg_t* cfg);
NL_API nl_http_client_t* nlh_client_connect(const char* host, int port,
                                            const nl_http_tls_client_cfg_t* cfg);
NL_API void nlh_client_close(nl_http_client_t* client);
NL_API int nlh_client_request(nl_http_client_t* client,
                               const char* method,
                               const char* path,
                               const char* body, size_t body_len,
                               char* out_buf, size_t out_buf_size,
                               size_t* out_body_len);
#endif /* NL_HTTPS_ENABLE */

#ifdef __cplusplus
}
#endif

#endif
