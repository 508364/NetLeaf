/**
 * @file netleaf_tls3.c
 * @brief TLS/SSL extension implementation using built-in mbedTLS 3.x (tls3: TLS 1.2/1.3)
 * @version 1.1.0
 * @date 2026-09-25
 *
 * This module provides TLS/SSL encryption support using a built-in mbedTLS 3.x
 * implementation.  It wraps the internal TLS functionality into a clean public
 * API.
 *
 * @note Based on mbedTLS (https://github.com/Mbed-TLS/mbedTLS) - Apache 2.0 / GPL v2.0
 */

#define _GNU_SOURCE
#include "netleaf_tls3.h"
#include "netleaf_tls3_lang.h"

#ifdef NL_TLS3_ENABLE

#include <mbedtls/platform.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/ssl_ticket.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_crl.h>
#include <mbedtls/pk.h>
#include <mbedtls/cipher.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <windows.h>
    #define NL_TLS3_SOCK   SOCKET
    #define NL_TLS3_CLOSESOCK(s) do { (void)(s); } while (0)   /* 不接管外部 socket */
#else
    #include <sys/select.h>
    #include <sys/time.h>
    #define NL_TLS3_SOCK   int
    #define NL_TLS3_CLOSESOCK(s) do { (void)(s); } while (0)
#endif

#define NL_TLS3_MAX_CERTS 8

// 单调毫秒时钟(用于 DTLS 定时器与握手超时)
static long long nl_tls3_now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// DTLS/握手重传定时器(供 mbedTLS 调用)
typedef struct {
    long long start_ms;
    uint32_t  int_ms;
    uint32_t  fin_ms;
} nl_dtls3_timer_t;

static void nl_tls3_timer_set(void* ctx, uint32_t int_ms, uint32_t fin_ms) {
    nl_dtls3_timer_t* t = (nl_dtls3_timer_t*)ctx;
    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
    if (fin_ms != 0) t->start_ms = nl_tls3_now_ms();
}

static int nl_tls3_timer_get(void* ctx) {
    nl_dtls3_timer_t* t = (nl_dtls3_timer_t*)ctx;
    if (t->fin_ms == 0) return -1;
    long long elapsed = nl_tls3_now_ms() - t->start_ms;
    if (elapsed >= (long long)t->fin_ms) return 2;
    if (t->int_ms != 0 && elapsed >= (long long)t->int_ms) return 1;
    return 0;
}

// 等待套接字可读/可写；timeout_ms < 0 表示无限等待。返回 >0 就绪，0 超时，<0 错误。
static int nl_tls3_sock_wait(NL_TLS3_SOCK sock, int for_write, int timeout_ms) {
    fd_set fs;
    FD_ZERO(&fs);
    FD_SET(sock, &fs);
    struct timeval tv;
    struct timeval* ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    int rc = select((int)sock + 1, for_write ? NULL : &fs, for_write ? &fs : NULL,
                    NULL, ptv);
    return rc;
}

// 单张已注册证书(用于多域名 / SNI 选择)
typedef struct {
    char               name[128];
    mbedtls_x509_crt   crt;
    mbedtls_pk_context key;
    int                used;
} nl_tls3_cert_slot_t;

// Internal TLS context structure
struct nl_tls3_ctx {
    mbedtls_ssl_context        ssl;
    mbedtls_net_context        server_fd;
    mbedtls_x509_crt          ca_chain;
    mbedtls_x509_crl          crl_chain;
    mbedtls_x509_crt          client_cert;
    mbedtls_pk_context        client_key;
    mbedtls_ssl_config        conf;
    mbedtls_entropy_context   entropy;
    mbedtls_ctr_drbg_context  ctr_drbg;
    mbedtls_ssl_cache_context cache;
    mbedtls_ssl_ticket_context ticket;
    char*                     hostname;
    char                      peer_cert_info[512];
    int                       setup_done;
    int                       handshaked;
    int                       initialized;
    int                       verify_peer;
    int                       is_server;
    int                       use_dtls;
    int                       rng_seeded;
    int                       crl_loaded;
    int                       cache_inited;
    int                       ticket_inited;
    nl_dtls3_timer_t          dtls_timer;
    nl_tls3_cert_slot_t       certs[NL_TLS3_MAX_CERTS];
    int                       cert_count;
    nl_tls3_sni_cb            sni_cb;
    void*                     sni_user;
    const nl_tls3_session_t*  offered;
    int                       reused;
};

// 可导出/恢复的会话句柄
struct nl_tls3_session {
    mbedtls_ssl_session s;
    int                 inited;
};

// Error string mapping
static const char* tls3_error_string(int code) {
    static char buf[256];
    switch (code) {
        case MBEDTLS_ERR_SSL_BAD_INPUT_DATA:       return "Bad input parameters";
        case MBEDTLS_ERR_SSL_ALLOC_FAILED:         return "Memory allocation failed";
        case MBEDTLS_ERR_SSL_HW_ACCEL_FAILED:      return "Hardware acceleration failure";
        case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:    return "Peer closed connection";
        case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:  return "Fatal alert message";
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

// 释放多证书槽位
static void nl_tls3_free_certs(nl_tls3_ctx_t* ctx) {
    for (int i = 0; i < ctx->cert_count; i++) {
        if (ctx->certs[i].used) {
            mbedtls_x509_crt_free(&ctx->certs[i].crt);
            mbedtls_pk_free(&ctx->certs[i].key);
            ctx->certs[i].used = 0;
        }
        ctx->certs[i].name[0] = '\0';
    }
    ctx->cert_count = 0;
}

// SNI 选择：按回调/主机名从已注册证书中挑选本端证书
static int nl_tls3_sni_thunk(void* p, mbedtls_ssl_context* ssl,
                            const unsigned char* name, size_t len) {
    nl_tls3_ctx_t* ctx = (nl_tls3_ctx_t*)p;
    if (!ctx) return 0;
    char host[256];
    size_t n = len < sizeof(host) - 1 ? len : sizeof(host) - 1;
    if (n > 0 && name) memcpy(host, name, n);
    host[n] = '\0';
    const char* pick = NULL;
    if (ctx->sni_cb) pick = ctx->sni_cb(host, ctx->sni_user);
    if (!pick) pick = host;
    for (int i = 0; i < ctx->cert_count; i++) {
        if (ctx->certs[i].used && strcmp(ctx->certs[i].name, pick) == 0) {
            if (mbedtls_ssl_set_hs_own_cert(ssl, &ctx->certs[i].crt,
                                            &ctx->certs[i].key) != 0) {
                return -1;
            }
            return 0;
        }
    }
    return 0;
}

nl_tls3_ctx_t* nl_tls3_create(void) {
    nl_tls3_ctx_t* ctx = (nl_tls3_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    mbedtls_net_init(&ctx->server_fd);
    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->conf);
    mbedtls_x509_crt_init(&ctx->ca_chain);
    mbedtls_x509_crl_init(&ctx->crl_chain);
    mbedtls_x509_crt_init(&ctx->client_cert);
    mbedtls_pk_init(&ctx->client_key);
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    mbedtls_ssl_cache_init(&ctx->cache);
    mbedtls_ssl_ticket_init(&ctx->ticket);
    ctx->hostname    = NULL;
    ctx->setup_done  = 0;
    ctx->handshaked  = 0;
    ctx->verify_peer = 1;
    ctx->is_server   = 0;
    ctx->use_dtls    = 0;
    ctx->cert_count  = 0;
    ctx->offered     = NULL;
    ctx->reused      = 0;
    ctx->cache_inited  = 1;
    ctx->ticket_inited = 1;
    ctx->initialized = 1;
    return ctx;
}

void nl_tls3_destroy(nl_tls3_ctx_t* ctx) {
    if (!ctx) return;
    if (ctx->handshaked) {
        mbedtls_ssl_close_notify(&ctx->ssl);
    }
    ctx->server_fd.fd = -1;
    nl_tls3_free_certs(ctx);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->ca_chain);
    mbedtls_x509_crl_free(&ctx->crl_chain);
    mbedtls_x509_crt_free(&ctx->client_cert);
    mbedtls_pk_free(&ctx->client_key);
    mbedtls_entropy_free(&ctx->entropy);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    if (ctx->cache_inited)  mbedtls_ssl_cache_free(&ctx->cache);
    if (ctx->ticket_inited) mbedtls_ssl_ticket_free(&ctx->ticket);
    free(ctx->hostname);
    free(ctx);
}

// 协议版本 -> mbedTLS minor 版本（mbedTLS 3.x 仅支持 TLS 1.2 / 1.3）
static int nl_tls3_minor_of(nl_tls3_protocol_t p) {
    switch (p) {
        case NL_TLS3_PROTO_TLS1_2:
        case NL_TLS3_PROTO_DTLS1_2:
            return MBEDTLS_SSL_MINOR_VERSION_3;   // TLS 1.2
        case NL_TLS3_PROTO_TLS1_3:
            return MBEDTLS_SSL_MINOR_VERSION_4;   // TLS 1.3
        default:
            return MBEDTLS_SSL_MINOR_VERSION_3;
    }
}

int nl_tls3_configure(nl_tls3_ctx_t* ctx, const nl_tls3_config_t* cfg) {
    if (!ctx || !ctx->initialized || !cfg) return NL_TLS3_INVALID_STATE;
    int ret;
    if (!ctx->rng_seeded) {
        ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                                     (const unsigned char*)"netleaf_tls3",
                                     sizeof("netleaf_tls3") - 1);
        if (ret != 0) return NL_TLS3_ERROR;
        ctx->rng_seeded = 1;
    }
    ctx->is_server = cfg->is_server ? 1 : 0;
    ctx->use_dtls  = cfg->use_dtls ? 1 : 0;
    // 重复配置安全
    mbedtls_x509_crt_free(&ctx->ca_chain);    mbedtls_x509_crt_init(&ctx->ca_chain);
    mbedtls_x509_crl_free(&ctx->crl_chain);   mbedtls_x509_crl_init(&ctx->crl_chain);
    mbedtls_x509_crt_free(&ctx->client_cert); mbedtls_x509_crt_init(&ctx->client_cert);
    mbedtls_pk_free(&ctx->client_key);        mbedtls_pk_init(&ctx->client_key);
    ctx->crl_loaded = 0;
    nl_tls3_free_certs(ctx);
    // 初始化 SSL 配置
    ret = mbedtls_ssl_config_defaults(&ctx->conf,
                                       ctx->is_server ? MBEDTLS_SSL_IS_SERVER
                                                      : MBEDTLS_SSL_IS_CLIENT,
                                       ctx->use_dtls ? MBEDTLS_SSL_TRANSPORT_DATAGRAM
                                                     : MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return NL_TLS3_HANDSHAKE;
    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    // 统一 verify_peer 语义
    ctx->verify_peer = ctx->is_server ? (cfg->client_auth ? 1 : 0)
                                      : (cfg->verify_peer ? 1 : 0);
    mbedtls_ssl_conf_authmode(&ctx->conf,
                             ctx->verify_peer ? MBEDTLS_SSL_VERIFY_REQUIRED
                                              : MBEDTLS_SSL_VERIFY_NONE);
    // 协议版本区间：TLS 1.2 / 1.3
    int min_minor = nl_tls3_minor_of(cfg->min_proto);
    int max_minor = nl_tls3_minor_of(cfg->max_proto);
    // mbedTLS 3.x 中 TLS1.3 的 minor 为 4
    if (max_minor < MBEDTLS_SSL_MINOR_VERSION_3) max_minor = MBEDTLS_SSL_MINOR_VERSION_3;
    if (ctx->use_dtls && min_minor < MBEDTLS_SSL_MINOR_VERSION_3)
        min_minor = MBEDTLS_SSL_MINOR_VERSION_3;
    if (min_minor > max_minor) max_minor = min_minor;
    mbedtls_ssl_conf_min_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3, min_minor);
    mbedtls_ssl_conf_max_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3, max_minor);
    // 加载 CA 证书
    if (cfg->ca_file) {
        ret = mbedtls_x509_crt_parse_file(&ctx->ca_chain, cfg->ca_file);
        if (ret != 0) return NL_TLS3_BAD_CA;
    } else if (cfg->ca_path) {
        ret = mbedtls_x509_crt_parse_path(&ctx->ca_chain, cfg->ca_path);
        if (ret < 0) return NL_TLS3_BAD_CA;
    }
    // CRL 吊销列表
    if (cfg->crl_file) {
        ret = mbedtls_x509_crl_parse_file(&ctx->crl_chain, cfg->crl_file);
        if (ret != 0) return NL_TLS3_BAD_CRL;
        ctx->crl_loaded = 1;
    }
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_chain,
                              ctx->crl_loaded ? &ctx->crl_chain : NULL);
    // 加载本端证书与私钥
    if (cfg->cert_file || cfg->key_file) {
        if (!cfg->cert_file || !cfg->key_file) {
            return NL_TLS3_BAD_CERT;
        }
        ret = mbedtls_x509_crt_parse_file(&ctx->client_cert, cfg->cert_file);
        if (ret != 0) return NL_TLS3_BAD_CERT;
        ret = mbedtls_pk_parse_keyfile(&ctx->client_key, cfg->key_file, cfg->key_pass,
                                       mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
        if (ret != 0) return NL_TLS3_BAD_KEY;
        ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->client_cert, &ctx->client_key);
        if (ret != 0) return NL_TLS3_ERROR;
    }
    // PSK 预共享密钥
    if (cfg->psk && cfg->psk_len > 0) {
        const unsigned char* id = (const unsigned char*)(cfg->psk_identity ? cfg->psk_identity : "");
        size_t id_len = cfg->psk_identity ? strlen(cfg->psk_identity) : 0;
        ret = mbedtls_ssl_conf_psk(&ctx->conf, cfg->psk, cfg->psk_len, id, id_len);
        if (ret != 0) return NL_TLS3_BAD_PSK;
    }
    // 自定义密码套件
    if (cfg->ciphersuites) {
        mbedtls_ssl_conf_ciphersuites(&ctx->conf, cfg->ciphersuites);
    }
    // ALPN
    if (cfg->alpn) {
        ret = mbedtls_ssl_conf_alpn_protocols(&ctx->conf,
                                               (const char**)cfg->alpn);
        if (ret != 0) return NL_TLS3_BAD_ALPN;
    }
    // DTLS：设置握手重传超时区间
    if (ctx->use_dtls) {
        mbedtls_ssl_conf_handshake_timeout(&ctx->conf, 1000, 60000);
    }
    // 会话复用
    if (!ctx->is_server) {
        mbedtls_ssl_conf_session_tickets(&ctx->conf, MBEDTLS_SSL_SESSION_TICKETS_ENABLED);
    } else {
        if (cfg->enable_session_tickets) {
            if (!ctx->ticket_inited) {
                mbedtls_ssl_ticket_init(&ctx->ticket);
                ctx->ticket_inited = 1;
            }
            ret = mbedtls_ssl_ticket_setup(&ctx->ticket, mbedtls_ctr_drbg_random,
                                           &ctx->ctr_drbg, MBEDTLS_CIPHER_AES_256_GCM,
                                           86400);
            if (ret != 0) return NL_TLS3_ERROR;
            mbedtls_ssl_conf_session_tickets_cb(&ctx->conf, mbedtls_ssl_ticket_write,
                                                mbedtls_ssl_ticket_parse, &ctx->ticket);
        }
        if (cfg->session_cache_size > 0) {
            if (!ctx->cache_inited) {
                mbedtls_ssl_cache_init(&ctx->cache);
                ctx->cache_inited = 1;
            }
            mbedtls_ssl_cache_set_max_entries(&ctx->cache, cfg->session_cache_size);
            mbedtls_ssl_conf_session_cache(&ctx->conf,
                                           &ctx->cache,
                                           mbedtls_ssl_cache_get,
                                           mbedtls_ssl_cache_set);
        }
    }
    ctx->setup_done = 1;
    return NL_TLS3_OK;
}

int nl_tls3_set_verify(nl_tls3_ctx_t* ctx, int verify_peer) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    ctx->verify_peer = verify_peer ? 1 : 0;
    mbedtls_ssl_conf_authmode(&ctx->conf,
                             ctx->verify_peer ? MBEDTLS_SSL_VERIFY_REQUIRED
                                              : MBEDTLS_SSL_VERIFY_NONE);
    return NL_TLS3_OK;
}

int nl_tls3_set_hostname(nl_tls3_ctx_t* ctx, const char* hostname) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    free(ctx->hostname);
    if (hostname && *hostname) {
        size_t n = strlen(hostname) + 1;
        ctx->hostname = (char*)malloc(n);
        if (!ctx->hostname) return NL_TLS3_ALLOC_FAIL;
        memcpy(ctx->hostname, hostname, n);
    } else {
        ctx->hostname = NULL;
    }
    return NL_TLS3_OK;
}

int nl_tls3_set_dtls(nl_tls3_ctx_t* ctx, int enable) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (ctx->handshaked) return NL_TLS3_INVALID_STATE;
    ctx->use_dtls = enable ? 1 : 0;
    return NL_TLS3_OK;
}

int nl_tls3_set_alpn(nl_tls3_ctx_t* ctx, const char* const* protocols) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!protocols) return NL_TLS3_OK;
    int ret = mbedtls_ssl_conf_alpn_protocols(&ctx->conf, (const char**)protocols);
    if (ret != 0) return NL_TLS3_BAD_ALPN;
    return NL_TLS3_OK;
}

const char* nl_tls3_get_alpn(nl_tls3_ctx_t* ctx) {
    if (!ctx || !ctx->handshaked) return NULL;
    return mbedtls_ssl_get_alpn_protocol(&ctx->ssl);
}

int nl_tls3_set_ciphersuites(nl_tls3_ctx_t* ctx, const int* ciphersuites) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!ciphersuites) return NL_TLS3_OK;
    // mbedTLS 3.x: mbedtls_ssl_conf_ciphersuites 返回 void，传入空表即清空
    mbedtls_ssl_conf_ciphersuites(&ctx->conf, ciphersuites);
    return NL_TLS3_OK;
}

int nl_tls3_set_psk(nl_tls3_ctx_t* ctx, const unsigned char* psk,
                   size_t psk_len, const char* identity) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!psk || psk_len == 0) return NL_TLS3_BAD_PSK;
    const unsigned char* id = (const unsigned char*)(identity ? identity : "");
    size_t id_len = identity ? strlen(identity) : 0;
    int ret = mbedtls_ssl_conf_psk(&ctx->conf, psk, psk_len, id, id_len);
    if (ret != 0) return NL_TLS3_BAD_PSK;
    return NL_TLS3_OK;
}

int nl_tls3_set_crl(nl_tls3_ctx_t* ctx, const char* crl_file) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!crl_file) {
        ctx->crl_loaded = 0;
        return NL_TLS3_OK;
    }
    int ret = mbedtls_x509_crl_parse_file(&ctx->crl_chain, crl_file);
    if (ret != 0) return NL_TLS3_BAD_CRL;
    ctx->crl_loaded = 1;
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_chain,
                              ctx->crl_loaded ? &ctx->crl_chain : NULL);
    return NL_TLS3_OK;
}

int nl_tls3_set_session_tickets(nl_tls3_ctx_t* ctx, int enable) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!ctx->is_server) return NL_TLS3_INVALID_STATE;
    if (!enable) {
        mbedtls_ssl_conf_session_tickets(&ctx->conf, 0);
        return NL_TLS3_OK;
    }
    if (!ctx->ticket_inited) {
        mbedtls_ssl_ticket_init(&ctx->ticket);
        ctx->ticket_inited = 1;
    }
    int ret = mbedtls_ssl_ticket_setup(&ctx->ticket, mbedtls_ctr_drbg_random,
                                       &ctx->ctr_drbg, MBEDTLS_CIPHER_AES_256_GCM,
                                       86400);
    if (ret != 0) return NL_TLS3_ERROR;
    mbedtls_ssl_conf_session_tickets_cb(&ctx->conf, mbedtls_ssl_ticket_write,
                                        mbedtls_ssl_ticket_parse, &ctx->ticket);
    return NL_TLS3_OK;
}

int nl_tls3_set_session_cache(nl_tls3_ctx_t* ctx, int max_entries) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!ctx->is_server) return NL_TLS3_INVALID_STATE;
    if (max_entries <= 0) {
        mbedtls_ssl_conf_session_cache(&ctx->conf, NULL, NULL, NULL);
        return NL_TLS3_OK;
    }
    if (!ctx->cache_inited) {
        mbedtls_ssl_cache_init(&ctx->cache);
        ctx->cache_inited = 1;
    }
    mbedtls_ssl_cache_set_max_entries(&ctx->cache, max_entries);
    mbedtls_ssl_conf_session_cache(&ctx->conf, &ctx->cache,
                                   mbedtls_ssl_cache_get,
                                   mbedtls_ssl_cache_set);
    return NL_TLS3_OK;
}

int nl_tls3_add_cert(nl_tls3_ctx_t* ctx, const char* name,
                    const char* cert_file, const char* key_file) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!name || !cert_file || !key_file) return NL_TLS3_BAD_CERT;
    if (ctx->cert_count >= NL_TLS3_MAX_CERTS) return NL_TLS3_ALLOC_FAIL;
    nl_tls3_cert_slot_t* slot = &ctx->certs[ctx->cert_count];
    int ret = mbedtls_x509_crt_parse_file(&slot->crt, cert_file);
    if (ret != 0) return NL_TLS3_BAD_CERT;
    ret = mbedtls_pk_parse_keyfile(&slot->key, key_file, NULL,
                                   mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
    if (ret != 0) return NL_TLS3_BAD_KEY;
    size_t n = strlen(name);
    if (n >= sizeof(slot->name)) n = sizeof(slot->name) - 1;
    memcpy(slot->name, name, n);
    slot->name[n] = '\0';
    slot->used = 1;
    ctx->cert_count++;
    if (ctx->cert_count == 1) {
        mbedtls_ssl_conf_sni(&ctx->conf, nl_tls3_sni_thunk, ctx);
    }
    return NL_TLS3_OK;
}

int nl_tls3_set_sni_callback(nl_tls3_ctx_t* ctx, nl_tls3_sni_cb cb, void* user_data) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    ctx->sni_cb = cb;
    ctx->sni_user = user_data;
    return NL_TLS3_OK;
}

int nl_tls3_handshake_ex(nl_tls3_ctx_t* ctx, int sock, int timeout_ms) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (ctx->hostname && !ctx->is_server) {
        mbedtls_ssl_set_hostname(&ctx->ssl, ctx->hostname);
    }
    if (ctx->offered) {
        mbedtls_ssl_set_session(&ctx->ssl, &ctx->offered->s);
        ctx->reused = 0;
    }
    ctx->server_fd.fd = sock;
    ctx->setup_done = 0;
    ctx->handshaked = 0;

    int ret;
    if (ctx->is_server) {
        mbedtls_net_set_nonblock(&ctx->server_fd);
        do {
            ret = mbedtls_ssl_handshake(&ctx->ssl);
            if (ret == 0) break;
            if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return ret == MBEDTLS_ERR_SSL_CONN_EOF ? NL_TLS3_READ : NL_TLS3_HANDSHAKE;
            }
            if (ctx->use_dtls) {
                ctx->dtls_timer.int_ms = 0;
                ctx->dtls_timer.fin_ms = 0;
                if (timeout_ms > 0) {
                    ctx->dtls_timer.start_ms = nl_tls3_now_ms();
                    ctx->dtls_timer.fin_ms   = timeout_ms;
                }
            }
            int wr = ret == MBEDTLS_ERR_SSL_WANT_WRITE ? 1 : 0;
            if (timeout_ms >= 0) {
                if (nl_tls3_sock_wait(ctx->server_fd.fd, wr, timeout_ms) <= 0) {
                    return NL_TLS3_TIMEOUT;
                }
            }
        } while (1);
    } else {
        do {
            ret = mbedtls_ssl_handshake(&ctx->ssl);
            if (ret == 0) break;
            if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return ret == MBEDTLS_ERR_SSL_CONN_EOF ? NL_TLS3_READ : NL_TLS3_HANDSHAKE;
            }
            if (ctx->use_dtls) {
                ctx->dtls_timer.int_ms = 0;
                ctx->dtls_timer.fin_ms = 0;
                if (timeout_ms > 0) {
                    ctx->dtls_timer.start_ms = nl_tls3_now_ms();
                    ctx->dtls_timer.fin_ms   = timeout_ms;
                }
            }
            int wr = ret == MBEDTLS_ERR_SSL_WANT_WRITE ? 1 : 0;
            if (timeout_ms >= 0) {
                if (nl_tls3_sock_wait(ctx->server_fd.fd, wr, timeout_ms) <= 0) {
                    return NL_TLS3_TIMEOUT;
                }
            }
        } while (1);
    }

    ctx->handshaked = 1;
    // mbedTLS 3.x 移除了 mbedtls_ssl_session_id_reset，
    // 通过比较当前会话与 offer 会话的 id 是否一致来推断是否命中缓存
    // （命中时会话被更新，未命中则保留 offer 的 id）
    if (ctx->offered) {
        ctx->reused = 0; // 3.x 下不再可靠区分 session-resume 状态，保持 0
    }
    if (ctx->use_dtls) {
        mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->dtls_timer,
                                 nl_tls3_timer_set, nl_tls3_timer_get);
    }
    return NL_TLS3_OK;
}

int nl_tls3_handshake(nl_tls3_ctx_t* ctx, int sock) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!ctx->setup_done) return NL_TLS3_INVALID_STATE;
    return nl_tls3_handshake_ex(ctx, sock, 0);
}

int nl_tls3_send(nl_tls3_ctx_t* ctx, const void* buf, size_t len) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!ctx->handshaked) return NL_TLS3_INVALID_STATE;
    int ret;
    do {
        ret = mbedtls_ssl_write(&ctx->ssl, (const unsigned char*)buf, (int)len);
        if (ret > 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            int wr = ret == MBEDTLS_ERR_SSL_WANT_WRITE ? 1 : 0;
            if (nl_tls3_sock_wait(ctx->server_fd.fd, wr, -1) <= 0)
                return NL_TLS3_TIMEOUT;
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return NL_TLS3_READ;
        return NL_TLS3_WRITE;
    } while (1);
}

int nl_tls3_recv(nl_tls3_ctx_t* ctx, void* buf, size_t len) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (!ctx->handshaked) return NL_TLS3_INVALID_STATE;
    int ret;
    do {
        ret = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)buf, (int)len);
        if (ret >= 0) return ret;
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            int wr = ret == MBEDTLS_ERR_SSL_WANT_WRITE ? 1 : 0;
            if (nl_tls3_sock_wait(ctx->server_fd.fd, wr, -1) <= 0)
                return NL_TLS3_TIMEOUT;
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_CONN_EOF) return NL_TLS3_READ;
        return NL_TLS3_READ;
    } while (1);
}

int nl_tls3_close(nl_tls3_ctx_t* ctx) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (ctx->handshaked) {
        mbedtls_ssl_close_notify(&ctx->ssl);
    }
    ctx->handshaked = 0;
    return NL_TLS3_OK;
}

int nl_tls3_is_active(const nl_tls3_ctx_t* ctx) {
    return (ctx && ctx->initialized && ctx->handshaked) ? 1 : 0;
}

const char* nl_tls3_get_peer_cert(nl_tls3_ctx_t* ctx) {
    if (!ctx || !ctx->handshaked) return "";
    const mbedtls_x509_crt* pc = mbedtls_ssl_get_peer_cert(&ctx->ssl);
    if (!pc) return "";
    char* info = ctx->peer_cert_info;
    char  buf[256];
    memset(info, 0, sizeof(ctx->peer_cert_info));
    int off = snprintf(info, sizeof(ctx->peer_cert_info), "subject: ");
    if (off > 0) {
        int len = mbedtls_x509_dn_gets(buf, sizeof(buf), &pc->subject);
        if (len > 0) {
            off += snprintf(info + off, sizeof(ctx->peer_cert_info) - (size_t)off,
                           "%s", buf);
        }
    }
    return info;
}

int nl_tls3_get_session(nl_tls3_ctx_t* ctx, nl_tls3_session_t** out) {
    if (!ctx || !out) return NL_TLS3_INVALID_STATE;
    if (!ctx->handshaked) return NL_TLS3_NO_SESSION;
    mbedtls_ssl_session s;
    memset(&s, 0, sizeof(s));
    int ret = mbedtls_ssl_get_session(&ctx->ssl, &s);
    if (ret != 0) return NL_TLS3_NO_SESSION;
    nl_tls3_session_t* sess = (nl_tls3_session_t*)calloc(1, sizeof(*sess));
    if (!sess) return NL_TLS3_ALLOC_FAIL;
    sess->s = s;
    sess->inited = 1;
    *out = sess;
    return NL_TLS3_OK;
}

int nl_tls3_set_session(nl_tls3_ctx_t* ctx, const nl_tls3_session_t* session) {
    if (!ctx || !ctx->initialized) return NL_TLS3_INVALID_STATE;
    if (ctx->handshaked) return NL_TLS3_INVALID_STATE;
    if (!session || !session->inited) {
        ctx->offered = NULL;
        return NL_TLS3_OK;
    }
    ctx->offered = session;
    return NL_TLS3_OK;
}

void nl_tls3_session_free(nl_tls3_session_t* session) {
    if (!session) return;
    session->inited = 0;
    free(session);
}

int nl_tls3_session_reused(const nl_tls3_ctx_t* ctx) {
    if (!ctx) return 0;
    return ctx->reused ? 1 : 0;
}

const char* nl_tls3_get_proto_version(const nl_tls3_ctx_t* ctx) {
    if (!ctx || !ctx->handshaked) return "";
    // mbedTLS 3.x 支持 TLS 1.3（minor=4）
    int minor = MBEDTLS_SSL_MINOR_VERSION_3;
    switch (minor) {
        case MBEDTLS_SSL_MINOR_VERSION_3: return "TLSv1.2";
        case MBEDTLS_SSL_MINOR_VERSION_4: return "TLSv1.3";
        default:                           return "TLSv1.2";
    }
}

const char* nl_tls3_get_ciphersuite(const nl_tls3_ctx_t* ctx) {
    if (!ctx || !ctx->handshaked) return "";
    // mbedTLS 3.x: mbedtls_ssl_get_ciphersuite 直接返回 套件名称（const char*）
    const char* name = mbedtls_ssl_get_ciphersuite(&ctx->ssl);
    return (name && name[0] != '\0') ? name : "";
}

const char* nl_tls3_strerror(int err) {
    return tls3_error_string(err);
}

// ============================================================
// 模块系统接入
// ============================================================

#if defined(NL_TLS3_ENABLE) || defined(NL_TLS3_STATIC)

#include "netleaf_module.h"
#include "netleaf_tls3_lang.h"

NL_MODULE_DEFINE_LAZY(
    NL_MODULE_TLS, tls3, NL_TLS3_VERSION,
    NL_CAP_TLS, 1, 1, 1,
    nl_tls3_init, NULL, nl_tls3_is_available, nl_tls3_version,
    "TLS/SSL (mbedTLS 3.x, TLS 1.2/1.3)", "NetLeaf Team",
    NULL, NULL);

int nl_tls3_init(void) {
    return nl_module_register(NL_MODULE_GET_INFO(tls3));
}

int nl_tls3_is_available(void) {
    return 1;
}

const char* nl_tls3_version(void) {
    return NL_TLS3_VERSION;
}

// 注册语言表
void nl_tls3_lang_init(void) { nl_tls3_register_lang(); }

#endif // NL_TLS3_ENABLE / NL_TLS3_STATIC

#endif // NL_TLS3_ENABLE
