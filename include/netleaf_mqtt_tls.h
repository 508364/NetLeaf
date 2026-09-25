/**
 * @file netleaf_mqtt_tls.h
 * @brief MQTT TLS integration header (built-in mbedTLS implementation)
 * @version 1.0.0
 * @date 2026-09-19
 *
 * This module provides TLS/SSL encryption support for MQTT connections.
 * It uses a built-in mbedTLS implementation for cross-platform compatibility.
 *
 * @note Based on mbedTLS (https://github.com/Mbed-TLS/mbedTLS) - Apache 2.0 / GPL v2.0
 */
#ifndef NETLEAF_MQTT_TLS_H
#define NETLEAF_MQTT_TLS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
    #ifdef NL_MQTT_TLS_EXPORTS
        #define NL_MQTT_TLS_API __declspec(dllexport)
    #elif defined(NL_MQTT_TLS_STATIC)
        #define NL_MQTT_TLS_API
    #else
        #define NL_MQTT_TLS_API __declspec(dllimport)
    #endif
#else
    #define NL_MQTT_TLS_API
#endif

#define NL_MQTT_TLS_VERSION "1.0.0"

typedef struct nl_mqtt_tls_ctx nl_mqtt_tls_ctx_t;

typedef enum {
    NL_MQTT_TLS_OK            =  0,
    NL_MQTT_TLS_ERROR         = -1,
    NL_MQTT_TLS_BAD_CA        = -2,
    NL_MQTT_TLS_BAD_CERT      = -3,
    NL_MQTT_TLS_BAD_KEY       = -4,
    NL_MQTT_TLS_ALLOC_FAIL    = -5,
    NL_MQTT_TLS_HANDSHAKE     = -6,
    NL_MQTT_TLS_READ          = -7,
    NL_MQTT_TLS_WRITE         = -8,
    NL_MQTT_TLS_NOT_INIT      = -9
} nl_mqtt_tls_result_t;

typedef struct nl_mqtt_tls_config {
    const char* ca_file;
    const char* ca_path;
    const char* cert_file;
    const char* key_file;
    const char* key_pass;
    int         verify_peer;
    int         client_auth;
    int         min_version;
    int         max_version;
} nl_mqtt_tls_config_t;

NL_MQTT_TLS_API nl_mqtt_tls_ctx_t* nl_mqtt_tls_create(void);
NL_MQTT_TLS_API void nl_mqtt_tls_destroy(nl_mqtt_tls_ctx_t* ctx);

NL_MQTT_TLS_API int nl_mqtt_tls_configure(nl_mqtt_tls_ctx_t* ctx,
                                           const nl_mqtt_tls_config_t* cfg);

/* 设置握手时用于 SNI 与证书主机名校验的目标主机名。
 * 必须在 nl_mqtt_tls_handshake() 之前调用；传 NULL 可清除。 */
NL_MQTT_TLS_API int nl_mqtt_tls_set_hostname(nl_mqtt_tls_ctx_t* ctx,
                                             const char* hostname);

NL_MQTT_TLS_API int nl_mqtt_tls_handshake(nl_mqtt_tls_ctx_t* ctx, int sock);
NL_MQTT_TLS_API int nl_mqtt_tls_send(nl_mqtt_tls_ctx_t* ctx,
                                      const void* buf, size_t len);
NL_MQTT_TLS_API int nl_mqtt_tls_recv(nl_mqtt_tls_ctx_t* ctx,
                                      void* buf, size_t len);
NL_MQTT_TLS_API int nl_mqtt_tls_close(nl_mqtt_tls_ctx_t* ctx);
NL_MQTT_TLS_API int nl_mqtt_tls_is_active(const nl_mqtt_tls_ctx_t* ctx);
NL_MQTT_TLS_API const char* nl_mqtt_tls_strerror(int err);

#ifdef __cplusplus
}
#endif

#endif /* NETLEAF_MQTT_TLS_H */
