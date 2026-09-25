/**
 * @file netleaf_mqtt_tls.c
 * @brief MQTT TLS implementation using built-in mbedTLS
 * @version 1.0.0
 * @date 2026-09-19
 *
 * This module provides TLS/SSL encryption support for MQTT connections.
 * It uses a built-in mbedTLS implementation for cross-platform compatibility.
 *
 * @note Based on mbedTLS (https://github.com/Mbed-TLS/mbedTLS) - Apache 2.0 / GPL v2.0
 */

#define _GNU_SOURCE
#define NL_MQTT_TLS_STATIC
#include "netleaf_mqtt_tls.h"

#ifdef NL_MQTT_TLS_ENABLE

#include <mbedtls/platform.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <string.h>
#include <stdio.h>

struct nl_mqtt_tls_ctx {
    mbedtls_ssl_context       ssl;
    mbedtls_net_context       server_fd;
    mbedtls_x509_crt          ca_chain;
    mbedtls_x509_crt          client_cert;
    mbedtls_pk_context        client_key;
    mbedtls_ssl_config        conf;
    mbedtls_entropy_context   entropy;
    mbedtls_ctr_drbg_context  ctr_drbg;
    int                       initialized;
    int                       verify_peer;
    /* mbedtls_ssl_setup() 每个 ssl 上下文只允许成功调用一次，
       用于避免重连时重复 setup 造成资源泄漏/未定义行为 */
    int                       setup_done;
    /* SNI 与证书主机名校验使用的主机名（本结构持有副本并负责释放） */
    char*                     hostname;
};

static const char* tls_error_string(int code) {
    static char buf[256];
    switch (code) {
        case MBEDTLS_ERR_SSL_BAD_INPUT_DATA:       return "Bad input parameters";
        case MBEDTLS_ERR_SSL_ALLOC_FAILED:         return "Memory allocation failed";
        case MBEDTLS_ERR_SSL_HW_ACCEL_FAILED:      return "Hardware acceleration failure";
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:    return "Peer closed connection";
        case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:  return "Fatal alert message";
        case MBEDTLS_ERR_SSL_CERTIFICATE_REQUIRED: return "Certificate required";
        case MBEDTLS_ERR_SSL_PRIVATE_KEY_REQUIRED: return "Private key required";
        case MBEDTLS_ERR_SSL_CONN_EOF:             return "Connection EOF";
        case MBEDTLS_ERR_SSL_CLIENT_RECONNECT:     return "Client reconnect";
        case MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE:   return "Unexpected message";
        case MBEDTLS_ERR_SSL_INVALID_RECORD:       return "Invalid record";
        case MBEDTLS_ERR_SSL_TIMEOUT:              return "Timeout";
        default:
            mbedtls_strerror(code, buf, sizeof(buf));
            return buf;
    }
}

nl_mqtt_tls_ctx_t* nl_mqtt_tls_create(void) {
    nl_mqtt_tls_ctx_t* ctx = (nl_mqtt_tls_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    // 初始化各 mbedTLS 子上下文（这些函数不返回错误码）
    mbedtls_net_init(&ctx->server_fd);
    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->conf);
    mbedtls_x509_crt_init(&ctx->ca_chain);
    mbedtls_x509_crt_init(&ctx->client_cert);
    mbedtls_pk_init(&ctx->client_key);
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    ctx->initialized = 0;
    return ctx;
}

void nl_mqtt_tls_destroy(nl_mqtt_tls_ctx_t* ctx) {
    if (!ctx) return;
    // server_fd 只是外部 fd 的包装：重置为 -1，释放流程绝不关闭该 fd
    ctx->server_fd.fd = -1;
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->ca_chain);
    mbedtls_x509_crt_free(&ctx->client_cert);
    mbedtls_pk_free(&ctx->client_key);
    mbedtls_entropy_free(&ctx->entropy);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    free(ctx->hostname);
    free(ctx);
}

int nl_mqtt_tls_set_hostname(nl_mqtt_tls_ctx_t* ctx, const char* hostname) {
    if (!ctx) return NL_MQTT_TLS_ERROR;

    /* 先在堆上准备新副本，成功后再替换，避免失败时丢失旧值 */
    char* copy = NULL;
    if (hostname && hostname[0] != '\0') {
        size_t n = strlen(hostname);
        copy = (char*)malloc(n + 1);
        if (!copy) return NL_MQTT_TLS_ALLOC_FAIL;
        memcpy(copy, hostname, n + 1);
    }

    free(ctx->hostname);
    ctx->hostname = copy;
    return NL_MQTT_TLS_OK;
}

int nl_mqtt_tls_configure(nl_mqtt_tls_ctx_t* ctx,
                           const nl_mqtt_tls_config_t* cfg) {
    if (!ctx || !cfg) return NL_MQTT_TLS_ERROR;

    int ret;

    /* conf 即将被重建：若此前已完成 setup，则释放 ssl 上下文以便用新 conf
       重新 setup，避免继续沿用旧配置或对同一上下文重复 setup */
    if (ctx->setup_done) {
        mbedtls_ssl_free(&ctx->ssl);
        mbedtls_ssl_init(&ctx->ssl);
        ctx->setup_done = 0;
    }

    // 初始化随机数发生器；种子长度按字符串字面量的实际长度计算，避免越界读取
    ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                                 (const unsigned char*)"netleaf_mqtt_tls",
                                 sizeof("netleaf_mqtt_tls") - 1);
    if (ret != 0) {
        return NL_MQTT_TLS_ERROR;
    }

    ret = mbedtls_ssl_config_defaults(
        &ctx->conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT
    );
    if (ret != 0) {
        return NL_MQTT_TLS_ERROR;
    }

    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    if (cfg->ca_file || cfg->ca_path) {
        ret = mbedtls_x509_crt_parse_file(&ctx->ca_chain,
            cfg->ca_file ? cfg->ca_file : cfg->ca_path);
        if (ret != 0) {
            return NL_MQTT_TLS_BAD_CA;
        }
        mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_chain, NULL);
    }

    if (cfg->cert_file && cfg->key_file) {
        ret = mbedtls_x509_crt_parse_file(&ctx->client_cert, cfg->cert_file);
        if (ret != 0) {
            return NL_MQTT_TLS_BAD_CERT;
        }
        ret = mbedtls_pk_parse_keyfile(&ctx->client_key, cfg->key_file,
                                        cfg->key_pass ? cfg->key_pass : "");
        if (ret != 0) {
            return NL_MQTT_TLS_BAD_KEY;
        }
        ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->client_cert,
                                         &ctx->client_key);
        if (ret != 0) {
            return NL_MQTT_TLS_ERROR;
        }
    }

    ctx->verify_peer = cfg->verify_peer;
    if (!cfg->verify_peer) {
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }

    /* 协议版本区间：TLS1.0 - TLS1.2。
     * 当前内置 mbedTLS 未编译 TLS1.3（MBEDTLS_SSL_MINOR_VERSION_4 不可用），
     * 上限一律钳制为 TLS1.2；cfg->min_version/max_version 采用 nl_tls_protocol_t
     * 编号（1=TLS1.1, 2=TLS1.2, 3=TLS1.3），0 表示使用默认值。 */
    int min_minor = MBEDTLS_SSL_MINOR_VERSION_1; /* 默认最小 TLS1.0 */
    int max_minor = MBEDTLS_SSL_MINOR_VERSION_3; /* 默认最大 TLS1.2 */
    if (cfg->min_version >= 1 && cfg->min_version <= 3) min_minor = cfg->min_version + 1;
    if (cfg->max_version >= 1 && cfg->max_version <= 3) max_minor = cfg->max_version + 1;
    /* TLS1.3（minor 4）暂不支持，统一钳制到 TLS1.2 */
    if (min_minor > MBEDTLS_SSL_MINOR_VERSION_3) min_minor = MBEDTLS_SSL_MINOR_VERSION_3;
    if (max_minor > MBEDTLS_SSL_MINOR_VERSION_3) max_minor = MBEDTLS_SSL_MINOR_VERSION_3;
    if (min_minor > max_minor) max_minor = min_minor;
    mbedtls_ssl_conf_min_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3, min_minor);
    mbedtls_ssl_conf_max_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3, max_minor);

    ctx->initialized = 1;
    return NL_MQTT_TLS_OK;
}

int nl_mqtt_tls_handshake(nl_mqtt_tls_ctx_t* ctx, int sock) {
    if (!ctx || !ctx->initialized) return NL_MQTT_TLS_NOT_INIT;

    int ret;
    if (!ctx->setup_done) {
        /* mbedtls_ssl_setup() 每个上下文只允许成功调用一次 */
        ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
        if (ret != 0) {
            return NL_MQTT_TLS_ERROR;
        }
        ctx->setup_done = 1;
    } else {
        /* 重连场景：复用同一个 ssl 上下文，复位会话状态后重新握手 */
        ret = mbedtls_ssl_session_reset(&ctx->ssl);
        if (ret != 0) {
            return NL_MQTT_TLS_ERROR;
        }
    }

    /* 设置 SNI 与证书主机名（服务器证书校验依赖该主机名） */
    if (ctx->hostname) {
        ret = mbedtls_ssl_set_hostname(&ctx->ssl, ctx->hostname);
        if (ret != 0) {
            return NL_MQTT_TLS_ERROR;
        }
    }

    ctx->server_fd.fd = sock;
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->server_fd,
                        mbedtls_net_send, mbedtls_net_recv, NULL);

    ret = mbedtls_ssl_handshake(&ctx->ssl);
    if (ret != 0) {
        return NL_MQTT_TLS_HANDSHAKE;
    }

    if (ctx->verify_peer) {
        uint32_t flags = mbedtls_ssl_get_verify_result(&ctx->ssl);
        if (flags != 0) {
            return NL_MQTT_TLS_BAD_CERT;
        }
    }

    return NL_MQTT_TLS_OK;
}

int nl_mqtt_tls_send(nl_mqtt_tls_ctx_t* ctx, const void* buf, size_t len) {
    if (!ctx) return NL_MQTT_TLS_ERROR;
    int ret = mbedtls_ssl_write(&ctx->ssl, (const unsigned char*)buf, len);
    /* 非阻塞 I/O 的重试语义必须保留：WANT_* 不是写错误，
     * 返回 0 表示本次未写出任何数据，调用方稍后重试。 */
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (ret < 0) {
        return NL_MQTT_TLS_WRITE;
    }
    return ret;
}

int nl_mqtt_tls_recv(nl_mqtt_tls_ctx_t* ctx, void* buf, size_t len) {
    if (!ctx) return NL_MQTT_TLS_ERROR;
    int ret = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)buf, len);
    /* 非阻塞 I/O 的重试语义必须保留：WANT_* 不是读错误，
     * 返回 0 表示当前无数据可读，调用方稍后重试。 */
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return NL_MQTT_TLS_READ;
    }
    return ret;
}

int nl_mqtt_tls_close(nl_mqtt_tls_ctx_t* ctx) {
    if (!ctx) return NL_MQTT_TLS_ERROR;
    int ret = mbedtls_ssl_close_notify(&ctx->ssl);
    if (ret != 0 && ret != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return NL_MQTT_TLS_ERROR;
    }
    return NL_MQTT_TLS_OK;
}

int nl_mqtt_tls_is_active(const nl_mqtt_tls_ctx_t* ctx) {
    return ctx ? ctx->initialized : 0;
}

const char* nl_mqtt_tls_strerror(int err) {
    return tls_error_string(err);
}

#else /* NL_MQTT_TLS_ENABLE */

#include <stdlib.h>

struct nl_mqtt_tls_ctx { int dummy; };

nl_mqtt_tls_ctx_t* nl_mqtt_tls_create(void) { return NULL; }
void nl_mqtt_tls_destroy(nl_mqtt_tls_ctx_t* ctx) { (void)ctx; }
int nl_mqtt_tls_configure(nl_mqtt_tls_ctx_t* ctx,
                           const nl_mqtt_tls_config_t* cfg) {
    (void)ctx; (void)cfg; return NL_MQTT_TLS_NOT_INIT;
}
int nl_mqtt_tls_set_hostname(nl_mqtt_tls_ctx_t* ctx, const char* hostname) {
    (void)ctx; (void)hostname; return NL_MQTT_TLS_NOT_INIT;
}
int nl_mqtt_tls_handshake(nl_mqtt_tls_ctx_t* ctx, int sock) {
    (void)ctx; (void)sock; return NL_MQTT_TLS_NOT_INIT;
}
int nl_mqtt_tls_send(nl_mqtt_tls_ctx_t* ctx, const void* buf, size_t len) {
    (void)ctx; (void)buf; (void)len; return NL_MQTT_TLS_NOT_INIT;
}
int nl_mqtt_tls_recv(nl_mqtt_tls_ctx_t* ctx, void* buf, size_t len) {
    (void)ctx; (void)buf; (void)len; return NL_MQTT_TLS_NOT_INIT;
}
int nl_mqtt_tls_close(nl_mqtt_tls_ctx_t* ctx) {
    (void)ctx; return NL_MQTT_TLS_NOT_INIT;
}
int nl_mqtt_tls_is_active(const nl_mqtt_tls_ctx_t* ctx) {
    (void)ctx; return 0;
}
const char* nl_mqtt_tls_strerror(int err) {
    (void)err; return "TLS not available";
}

#endif /* NL_MQTT_TLS_ENABLE */
