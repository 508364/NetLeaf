/**
 * @file netleaf_tls.h
 * @brief TLS/SSL extension for NetLeaf (built-in mbedTLS implementation)
 * @version 1.1.0
 * @date 2026-09-25
 * @copyright Copyright (c) 2026
 *
 * This module provides TLS/SSL encryption support using a built-in mbedTLS implementation.
 * It can be used independently or by protocol modules (MQTT, HTTP, etc.)
 *
 * 能力范围（基于内置 mbedTLS 2.28.x LTS）：
 *   - TLS 1.0 / 1.1 / 1.2（DTLS 1.0 / 1.2 可选）
 *   - 客户端 / 服务端双角色、单向与双向(mTLS)证书认证
 *   - CA(文件/目录)、本端证书+私钥(含口令)、CRL 吊销检查
 *   - SNI + 证书主机名校验；多证书注册与 SNI 选择回调
 *   - PSK 预共享密钥、自定义密码套件
 *   - ALPN 协议协商
 *   - 会话复用：客户端会话导出/恢复；服务端会话票据与会话缓存
 *   - 握手超时 / 非阻塞握手
 *   - 协商结果查询(协议版本 / 密码套件 / ALPN)
 *
 * 不支持：TLS 1.3（协议核心移除，另见构建说明）、OCSP(Stapling/在线吊销查询)、
 *         证书签发/密钥生成、服务端多密钥类型同时呈现(RSA+ECDSA 双栈)。
 *
 * @note Based on mbedTLS (https://github.com/Mbed-TLS/mbedTLS) - Apache 2.0 / GPL v2.0
 */
#ifndef NETLEAF_TLS_H
#define NETLEAF_TLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

#ifndef NL_API
#define NL_API
#endif

#ifdef _WIN32
    #ifdef NL_TLS_EXPORTS
        #define NL_TLS_API __declspec(dllexport)
    #elif defined(NL_TLS_STATIC)
        #define NL_TLS_API
    #else
        #define NL_TLS_API __declspec(dllimport)
    #endif
#else
    #define NL_TLS_API
#endif

#define NL_TLS_VERSION              "1.1.0"
#define NL_TLS_VERSION_MAJOR        1
#define NL_TLS_VERSION_MINOR        1
#define NL_TLS_VERSION_PATCH        0

// ============================================================
// TLS Configuration
// ============================================================

typedef enum {
    NL_TLS_PROTO_TLS1_0 = 0,   /**< 默认值；min_proto/max_proto 传 0 表示使用默认版本 */
    NL_TLS_PROTO_TLS1_1 = 1,
    NL_TLS_PROTO_TLS1_2 = 2,
    NL_TLS_PROTO_TLS1_3 = 3,   /**< 暂不可用：会被钳制为 TLS1.2（见实现说明） */
    NL_TLS_PROTO_DTLS1_0 = 4,  /**< 仅在 use_dtls=1 时可用 */
    NL_TLS_PROTO_DTLS1_2 = 5   /**< 仅在 use_dtls=1 时可用 */
} nl_tls_protocol_t;

typedef struct nl_tls_config {
    const char*          ca_file;       /**< CA 证书文件路径 */
    const char*          ca_path;       /**< CA 证书目录路径 */
    const char*          cert_file;     /**< 本端证书：客户端证书 / 服务端证书 */
    const char*          key_file;      /**< 本端私钥：客户端私钥 / 服务端私钥 */
    const char*          key_pass;      /**< 私钥口令(加密私钥时使用) */
    const char*          crl_file;      /**< 证书吊销列表(CRL)文件路径；NULL 表示不做吊销检查 */
    int                  verify_peer;   /**< 客户端：是否校验对端(服务端)证书 (0 or 1) */
    int                  client_auth;   /**< 服务端：是否要求并校验客户端证书 (0 or 1) */
    int                  is_server;     /**< 1 = 服务端角色；0/缺省 = 客户端角色 */
    int                  use_dtls;      /**< 1 = DTLS(数据报)；0/缺省 = TLS(流) */
    int                  handshake_timeout_ms; /**< 握手超时(毫秒)；0 = 不设超时 */
    nl_tls_protocol_t    min_proto;     /**< 最低协议版本 */
    nl_tls_protocol_t    max_proto;     /**< 最高协议版本 */
    // PSK 预共享密钥(提供后走 PSK 密码套件；需与对端一致)
    const unsigned char* psk;           /**< PSK 密钥字节 */
    size_t               psk_len;       /**< PSK 密钥长度 */
    const char*          psk_identity;  /**< PSK 身份(客户端发送 / 服务端匹配) */
    // 自定义密码套件：以 0 结尾的套件 ID 数组(mbedTLS ID)；NULL 表示用默认
    const int*           ciphersuites;
    // ALPN 协议列表：以 NULL 结尾的字符串数组；NULL 表示不协商
    const char* const*   alpn;
    // 会话复用
    int                  enable_session_tickets; /**< 服务端：启用会话票据 (0/1) */
    int                  session_cache_size;     /**< 服务端：会话缓存条目上限；0 = 不启用 */
} nl_tls_config_t;

// ============================================================
// TLS Result Codes
// ============================================================

typedef enum {
    NL_TLS_OK             =  0,
    NL_TLS_ERROR          = -1,
    NL_TLS_BAD_CA         = -2,
    NL_TLS_BAD_CERT       = -3,
    NL_TLS_BAD_KEY        = -4,
    NL_TLS_ALLOC_FAIL     = -5,
    NL_TLS_HANDSHAKE      = -6,
    NL_TLS_READ           = -7,
    NL_TLS_WRITE          = -8,
    NL_TLS_NOT_INIT       = -9,
    NL_TLS_INVALID_STATE  = -10,
    NL_TLS_BAD_PSK        = -11,
    NL_TLS_BAD_CRL        = -12,
    NL_TLS_BAD_ALPN       = -13,
    NL_TLS_BAD_CIPHER     = -14,
    NL_TLS_TIMEOUT        = -15,
    NL_TLS_NO_SESSION     = -16,
    NL_TLS_WANT_READ      = -20,  /**< 需要先读取再重试（非阻塞 I/O，非错误） */
    NL_TLS_WANT_WRITE     = -21,  /**< 需要先写入再重试（非阻塞 I/O，非错误） */
    NL_TLS_UNKNOWN_ERROR  = -99
} nl_tls_result_t;

// ============================================================
// TLS Context / Session
// ============================================================

typedef struct nl_tls_ctx     nl_tls_ctx_t;
/** 不透明的可序列化会话句柄，用于跨连接恢复(会话复用) */
typedef struct nl_tls_session nl_tls_session_t;

/**
 * SNI 选择回调：握手收到 SNI 时调用，返回已注册证书名(见 nl_tls_add_cert)，
 * 框架据此切换本端证书；name 为客户端请求的主机名。
 */
typedef const char* (*nl_tls_sni_cb)(const char* name, void* user_data);

// ============================================================
// Public API
// ============================================================

/**
 * @brief Get TLS module information
 */
NL_TLS_API nl_module_info_t* nl_tls_get_module_info(void);

/**
 * @brief Initialize TLS module (register with module system)
 */
NL_TLS_API int nl_tls_init(void);

/**
 * @brief Get TLS library version
 */
NL_TLS_API const char* nl_tls_version(void);

/**
 * @brief Check if TLS is available
 */
NL_TLS_API int nl_tls_is_available(void);

/**
 * @brief Create a TLS context
 */
NL_TLS_API nl_tls_ctx_t* nl_tls_create(void);

/**
 * @brief Destroy a TLS context
 */
NL_TLS_API void nl_tls_destroy(nl_tls_ctx_t* ctx);

/**
 * @brief Configure TLS context with certificates and options
 * @note  可重复调用：每次会先释放上次加载的证书/私钥/CRL/SNI 证书，避免泄漏与叠加。
 */
NL_TLS_API int nl_tls_configure(nl_tls_ctx_t* ctx, const nl_tls_config_t* cfg);

/**
 * @brief Set verification options
 * @note  客户端：控制是否校验服务端证书；
 *        服务端：控制是否要求并校验客户端证书(client_auth 等效)。
 */
NL_TLS_API int nl_tls_set_verify(nl_tls_ctx_t* ctx, int verify_peer);

/**
 * @brief 设置对端服务器主机名
 *
 * 用于 TLS SNI 扩展以及证书主机名校验。当 verify_peer 打开时必须在
 * nl_tls_handshake() 之前调用，否则证书主机名无法校验。
 *
 * @param hostname 服务器主机名；传 NULL 表示清除已设置的主机名
 */
NL_TLS_API int nl_tls_set_hostname(nl_tls_ctx_t* ctx, const char* hostname);

/**
 * @brief 启用/关闭 DTLS(数据报传输)
 * @note  必须在 nl_tls_handshake() 之前调用。
 */
NL_TLS_API int nl_tls_set_dtls(nl_tls_ctx_t* ctx, int enable);

/**
 * @brief 设置 ALPN 协议列表(以 NULL 结尾)
 */
NL_TLS_API int nl_tls_set_alpn(nl_tls_ctx_t* ctx, const char* const* protocols);

/**
 * @brief 读取协商后的 ALPN 协议(NULL 表示未协商/不可用)
 */
NL_TLS_API const char* nl_tls_get_alpn(nl_tls_ctx_t* ctx);

/**
 * @brief 设置自定义密码套件(以 0 结尾的 mbedTLS 套件 ID 数组)
 */
NL_TLS_API int nl_tls_set_ciphersuites(nl_tls_ctx_t* ctx, const int* ciphersuites);

/**
 * @brief 设置 PSK 预共享密钥与身份
 */
NL_TLS_API int nl_tls_set_psk(nl_tls_ctx_t* ctx, const unsigned char* psk,
                              size_t psk_len, const char* identity);

/**
 * @brief 设置 CRL 吊销列表文件(用于校验对端证书是否被吊销)
 */
NL_TLS_API int nl_tls_set_crl(nl_tls_ctx_t* ctx, const char* crl_file);

/**
 * @brief 服务端：启用/关闭会话票据
 */
NL_TLS_API int nl_tls_set_session_tickets(nl_tls_ctx_t* ctx, int enable);

/**
 * @brief 服务端：启用会话缓存
 * @param max_entries 缓存条目上限；<=0 表示不启用
 */
NL_TLS_API int nl_tls_set_session_cache(nl_tls_ctx_t* ctx, int max_entries);

/**
 * @brief 注册一张本端证书(用于多域名/多证书)；name 为逻辑名(供 SNI 回调返回)
 * @note  必须在 nl_tls_handshake() 之前调用。
 */
NL_TLS_API int nl_tls_add_cert(nl_tls_ctx_t* ctx, const char* name,
                               const char* cert_file, const char* key_file);

/**
 * @brief 设置 SNI 选择回调
 */
NL_TLS_API int nl_tls_set_sni_callback(nl_tls_ctx_t* ctx, nl_tls_sni_cb cb,
                                       void* user_data);

/**
 * @brief Perform TLS handshake
 */
NL_TLS_API int nl_tls_handshake(nl_tls_ctx_t* ctx, int sock);

/**
 * @brief 执行握手并设置超时(毫秒)。支持阻塞与非阻塞 socket。
 */
NL_TLS_API int nl_tls_handshake_ex(nl_tls_ctx_t* ctx, int sock, int timeout_ms);

/**
 * @brief Send data through TLS connection
 */
NL_TLS_API int nl_tls_send(nl_tls_ctx_t* ctx, const void* buf, size_t len);

/**
 * @brief Receive data through TLS connection
 */
NL_TLS_API int nl_tls_recv(nl_tls_ctx_t* ctx, void* buf, size_t len);

/**
 * @brief Close TLS connection gracefully
 */
NL_TLS_API int nl_tls_close(nl_tls_ctx_t* ctx);

/**
 * @brief Check if TLS connection is active
 */
NL_TLS_API int nl_tls_is_active(const nl_tls_ctx_t* ctx);

/**
 * @brief Get peer certificate information
 */
NL_TLS_API const char* nl_tls_get_peer_cert(nl_tls_ctx_t* ctx);

// ============================================================
// Session Resumption
// ============================================================

/**
 * @brief 导出当前(已握手)会话，供后续连接用 nl_tls_set_session() 恢复
 * @param out 成功时返回新建句柄，需用 nl_tls_session_free() 释放
 */
NL_TLS_API int nl_tls_get_session(nl_tls_ctx_t* ctx, nl_tls_session_t** out);

/**
 * @brief 在握手前注入会话以尝试复用
 */
NL_TLS_API int nl_tls_set_session(nl_tls_ctx_t* ctx, const nl_tls_session_t* session);

/**
 * @brief 释放会话句柄
 */
NL_TLS_API void nl_tls_session_free(nl_tls_session_t* session);

/**
 * @brief 本次握手是否复用了既有会话
 */
NL_TLS_API int nl_tls_session_reused(const nl_tls_ctx_t* ctx);

// ============================================================
// Negotiation Info
// ============================================================

/**
 * @brief 协商后的协议版本字符串(如 "TLSv1.2")
 */
NL_TLS_API const char* nl_tls_get_proto_version(const nl_tls_ctx_t* ctx);

/**
 * @brief 协商后的密码套件名称
 */
NL_TLS_API const char* nl_tls_get_ciphersuite(const nl_tls_ctx_t* ctx);

/**
 * @brief Get TLS error string
 */
NL_TLS_API const char* nl_tls_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* NETLEAF_TLS_H */
