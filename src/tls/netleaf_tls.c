/**
 * @file netleaf_tls.c
 * @brief TLS/SSL extension implementation using built-in mbedTLS
 * @version 1.1.0
 * @date 2026-09-25
 *
 * This module provides TLS/SSL encryption support using a built-in mbedTLS implementation.
 * It wraps the internal TLS functionality into a clean public API.
 *
 * @note Based on mbedTLS (https://github.com/Mbed-TLS/mbedTLS) - Apache 2.0 / GPL v2.0
 */

#define _GNU_SOURCE
#include "netleaf_tls.h"
#include "netleaf_tls_lang.h"

#ifdef NL_TLS_ENABLE

#include <mbedtls/platform.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_cache.h>
#include <mbedtls/ssl_ticket.h>
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
    #define NL_TLS_SOCK   SOCKET
    #define NL_TLS_CLOSESOCK(s) do { (void)(s); } while (0)   /* 不接管外部 socket */
#else
    #include <sys/select.h>
    #include <sys/time.h>
    #define NL_TLS_SOCK   int
    #define NL_TLS_CLOSESOCK(s) do { (void)(s); } while (0)
#endif

#define NL_TLS_MAX_CERTS 8

// 单调毫秒时钟(用于 DTLS 定时器与握手超时)
static long long nl_tls_now_ms(void) {
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
} nl_dtls_timer_t;

static void nl_tls_timer_set(void* ctx, uint32_t int_ms, uint32_t fin_ms) {
    nl_dtls_timer_t* t = (nl_dtls_timer_t*)ctx;
    t->int_ms = int_ms;
    t->fin_ms = fin_ms;
    if (fin_ms != 0) t->start_ms = nl_tls_now_ms();
}

static int nl_tls_timer_get(void* ctx) {
    nl_dtls_timer_t* t = (nl_dtls_timer_t*)ctx;
    if (t->fin_ms == 0) return -1;
    long long elapsed = nl_tls_now_ms() - t->start_ms;
    if (elapsed >= (long long)t->fin_ms) return 2;
    if (t->int_ms != 0 && elapsed >= (long long)t->int_ms) return 1;
    return 0;
}

// 等待套接字可读/可写；timeout_ms < 0 表示无限等待。返回 >0 就绪，0 超时，<0 错误。
static int nl_tls_sock_wait(NL_TLS_SOCK sock, int for_write, int timeout_ms) {
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
} nl_tls_cert_slot_t;

// Internal TLS context structure
struct nl_tls_ctx {
    mbedtls_ssl_context       ssl;
    mbedtls_net_context       server_fd;
    mbedtls_x509_crt          ca_chain;
    mbedtls_x509_crl          crl_chain;
    mbedtls_x509_crt          client_cert;
    mbedtls_pk_context        client_key;
    mbedtls_ssl_config        conf;
    mbedtls_entropy_context   entropy;
    mbedtls_ctr_drbg_context  ctr_drbg;
    mbedtls_ssl_cache_context cache;
    mbedtls_ssl_ticket_context ticket;
    char*                     hostname;                    // 期望的对端主机名（SNI/证书校验）
    char                      peer_cert_info[512];         // 最近一次查询的对端证书信息缓冲
    int                       setup_done;                  // mbedtls_ssl_setup 是否已执行
    int                       handshaked;                  // 握手是否已成功完成
    int                       initialized;
    int                       verify_peer;                 // 客户端校验对端 / 服务端校验客户端
    int                       is_server;                   // 0 = 客户端，1 = 服务端
    int                       use_dtls;                    // 0 = TLS(流)，1 = DTLS(数据报)
    int                       rng_seeded;
    int                       crl_loaded;
    int                       cache_inited;
    int                       ticket_inited;
    nl_dtls_timer_t           dtls_timer;
    nl_tls_cert_slot_t        certs[NL_TLS_MAX_CERTS];
    int                       cert_count;
    nl_tls_sni_cb             sni_cb;
    void*                     sni_user;
    // 会话复用
    const nl_tls_session_t*   offered;                     // 调用方注入(借用，握手时深拷贝)
    int                       reused;                      // 本次握手是否复用
};

// 可导出/恢复的会话句柄
struct nl_tls_session {
    mbedtls_ssl_session s;
    int                 inited;
};

// Error string mapping
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
        case MBEDTLS_ERR_SSL_BAD_HS_PROTOCOL_VERSION: return "Bad protocol version";
        default:
            mbedtls_strerror(code, buf, sizeof(buf));
            return buf;
    }
}

// 释放多证书槽位
static void nl_tls_free_certs(nl_tls_ctx_t* ctx) {
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
static int nl_tls_sni_thunk(void* p, mbedtls_ssl_context* ssl,
                            const unsigned char* name, size_t len) {
    nl_tls_ctx_t* ctx = (nl_tls_ctx_t*)p;
    if (!ctx) return 0;

    char host[256];
    size_t n = len < sizeof(host) - 1 ? len : sizeof(host) - 1;
    if (n > 0 && name) memcpy(host, name, n);
    host[n] = '\0';

    const char* pick = NULL;
    if (ctx->sni_cb) pick = ctx->sni_cb(host, ctx->sni_user);
    if (!pick) pick = host;   // 缺省：按主机名匹配注册名

    for (int i = 0; i < ctx->cert_count; i++) {
        if (ctx->certs[i].used && strcmp(ctx->certs[i].name, pick) == 0) {
            if (mbedtls_ssl_set_hs_own_cert(ssl, &ctx->certs[i].crt,
                                            &ctx->certs[i].key) != 0) {
                return -1;
            }
            return 0;
        }
    }
    return 0;   // 无匹配：沿用默认本端证书
}

nl_tls_ctx_t* nl_tls_create(void) {
    nl_tls_ctx_t* ctx = (nl_tls_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    // 初始化各 mbedTLS 子上下文（这些函数不返回错误码）
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
    // cache/ticket 已在上面完成 init，这里标记为已初始化，防止 configure 重复 init
    ctx->cache_inited  = 1;
    ctx->ticket_inited = 1;
    ctx->initialized = 1;

    return ctx;
}

void nl_tls_destroy(nl_tls_ctx_t* ctx) {
    if (!ctx) return;

    // 仅当握手完成过才向对端发送 close_notify
    if (ctx->handshaked) {
        mbedtls_ssl_close_notify(&ctx->ssl);
    }

    // server_fd 只是外部 fd 的包装：重置为 -1，避免释放流程误关外部 fd
    ctx->server_fd.fd = -1;

    nl_tls_free_certs(ctx);

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

// 协议版本 -> mbedTLS minor 版本
static int nl_tls_minor_of(nl_tls_protocol_t p) {
    switch (p) {
        case NL_TLS_PROTO_TLS1_1:  return MBEDTLS_SSL_MINOR_VERSION_2;
        case NL_TLS_PROTO_TLS1_2:
        case NL_TLS_PROTO_TLS1_3:
        case NL_TLS_PROTO_DTLS1_2: return MBEDTLS_SSL_MINOR_VERSION_3;
        case NL_TLS_PROTO_DTLS1_0: return MBEDTLS_SSL_MINOR_VERSION_1;
        default:                   return MBEDTLS_SSL_MINOR_VERSION_1;   // TLS1.0 / DTLS1.0
    }
}

int nl_tls_configure(nl_tls_ctx_t* ctx, const nl_tls_config_t* cfg) {
    if (!ctx || !ctx->initialized || !cfg) return NL_TLS_INVALID_STATE;

    int ret;

    // 初始化随机数发生器；种子长度按字符串字面量的实际长度计算，避免越界读取
    if (!ctx->rng_seeded) {
        ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func, &ctx->entropy,
                                     (const unsigned char*)"netleaf_tls",
                                     sizeof("netleaf_tls") - 1);
        if (ret != 0) return NL_TLS_ERROR;
        ctx->rng_seeded = 1;
    }

    ctx->is_server = cfg->is_server ? 1 : 0;
    ctx->use_dtls  = cfg->use_dtls ? 1 : 0;

    // 重复配置安全：先释放上次加载的证书/私钥/CRL/多证书
    mbedtls_x509_crt_free(&ctx->ca_chain);   mbedtls_x509_crt_init(&ctx->ca_chain);
    mbedtls_x509_crl_free(&ctx->crl_chain);  mbedtls_x509_crl_init(&ctx->crl_chain);
    mbedtls_x509_crt_free(&ctx->client_cert);mbedtls_x509_crt_init(&ctx->client_cert);
    mbedtls_pk_free(&ctx->client_key);       mbedtls_pk_init(&ctx->client_key);
    ctx->crl_loaded = 0;
    nl_tls_free_certs(ctx);

    // 初始化 SSL 配置（客户端/服务端角色 / 传输类型 / 默认预设）
    ret = mbedtls_ssl_config_defaults(&ctx->conf,
                                       ctx->is_server ? MBEDTLS_SSL_IS_SERVER
                                                      : MBEDTLS_SSL_IS_CLIENT,
                                       ctx->use_dtls ? MBEDTLS_SSL_TRANSPORT_DATAGRAM
                                                     : MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return NL_TLS_HANDSHAKE;

    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    // 统一 verify_peer 语义：服务端等价于 client_auth
    ctx->verify_peer = ctx->is_server ? (cfg->client_auth ? 1 : 0)
                                      : (cfg->verify_peer ? 1 : 0);
    mbedtls_ssl_conf_authmode(&ctx->conf,
                             ctx->verify_peer ? MBEDTLS_SSL_VERIFY_REQUIRED
                                              : MBEDTLS_SSL_VERIFY_NONE);

    // 协议版本区间：TLS/DTLS 1.0 - 1.2。
    // 当前内置 mbedTLS 未启用可用的 TLS1.3 服务端路径，故上限钳制为 1.2；
    // min_proto/max_proto 传 0 视为“使用默认值”。
    int min_minor = nl_tls_minor_of(cfg->min_proto);
    int max_minor = MBEDTLS_SSL_MINOR_VERSION_3;
    if (cfg->max_proto == NL_TLS_PROTO_TLS1_1) max_minor = MBEDTLS_SSL_MINOR_VERSION_2;
    if (cfg->max_proto == NL_TLS_PROTO_TLS1_0 ||
        cfg->max_proto == NL_TLS_PROTO_DTLS1_0) max_minor = MBEDTLS_SSL_MINOR_VERSION_1;
    if (ctx->use_dtls && min_minor < MBEDTLS_SSL_MINOR_VERSION_1)
        min_minor = MBEDTLS_SSL_MINOR_VERSION_1;
    if (min_minor > max_minor) max_minor = min_minor;
    mbedtls_ssl_conf_min_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3, min_minor);
    mbedtls_ssl_conf_max_version(&ctx->conf, MBEDTLS_SSL_MAJOR_VERSION_3, max_minor);

    // 加载 CA 证书(文件或目录)
    if (cfg->ca_file) {
        ret = mbedtls_x509_crt_parse_file(&ctx->ca_chain, cfg->ca_file);
        if (ret != 0) return NL_TLS_BAD_CA;
    } else if (cfg->ca_path) {
        ret = mbedtls_x509_crt_parse_path(&ctx->ca_chain, cfg->ca_path);
        if (ret < 0) return NL_TLS_BAD_CA;
    }

    // CRL 吊销列表(与 CA 链分开持有，一并交给 mbedTLS 做吊销校验)
    if (cfg->crl_file) {
        ret = mbedtls_x509_crl_parse_file(&ctx->crl_chain, cfg->crl_file);
        if (ret != 0) return NL_TLS_BAD_CRL;
        ctx->crl_loaded = 1;
    }
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_chain,
                              ctx->crl_loaded ? &ctx->crl_chain : NULL);

    // 加载本端证书与私钥：解析失败必须返回错误码，不能静默忽略
    if (cfg->cert_file || cfg->key_file) {
        if (!cfg->cert_file || !cfg->key_file) {
            return NL_TLS_BAD_CERT;   // 证书与私钥必须成对提供
        }
        ret = mbedtls_x509_crt_parse_file(&ctx->client_cert, cfg->cert_file);
        if (ret != 0) return NL_TLS_BAD_CERT;
        ret = mbedtls_pk_parse_keyfile(&ctx->client_key, cfg->key_file, cfg->key_pass);
        if (ret != 0) return NL_TLS_BAD_KEY;
        ret = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->client_cert, &ctx->client_key);
        if (ret != 0) return NL_TLS_ERROR;
    }

    // PSK 预共享密钥
    if (cfg->psk && cfg->psk_len > 0) {
        const unsigned char* id = (const unsigned char*)(cfg->psk_identity ? cfg->psk_identity : "");
        size_t id_len = cfg->psk_identity ? strlen(cfg->psk_identity) : 0;
        ret = mbedtls_ssl_conf_psk(&ctx->conf, cfg->psk, cfg->psk_len, id, id_len);
        if (ret != 0) return NL_TLS_BAD_PSK;
    }

    // 自定义密码套件
    if (cfg->ciphersuites) {
        mbedtls_ssl_conf_ciphersuites(&ctx->conf, cfg->ciphersuites);
    }

    // ALPN
    if (cfg->alpn) {
        ret = mbedtls_ssl_conf_alpn_protocols(&ctx->conf, cfg->alpn);
        if (ret != 0) return NL_TLS_BAD_ALPN;
    }

    // DTLS：设置握手重传超时区间
    if (ctx->use_dtls) {
        mbedtls_ssl_conf_handshake_timeout(&ctx->conf, 1000, 60000);
    }

    // 会话复用：客户端接受票据；服务端启用票据/缓存
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
            if (ret != 0) return NL_TLS_ERROR;
            mbedtls_ssl_conf_session_tickets_cb(&ctx->conf, mbedtls_ssl_ticket_write,
                                                mbedtls_ssl_ticket_parse, &ctx->ticket);
        }
        if (cfg->session_cache_size > 0) {
            if (!ctx->cache_inited) {
                mbedtls_ssl_cache_init(&ctx->cache);
                ctx->cache_inited = 1;
            }
            mbedtls_ssl_cache_set_max_entries(&ctx->cache, cfg->session_cache_size);
            mbedtls_ssl_conf_session_cache(&ctx->conf, &ctx->cache,
                                           mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
        }
    }

    return NL_TLS_OK;
}

int nl_tls_set_verify(nl_tls_ctx_t* ctx, int verify_peer) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    // 客户端：是否校验服务端证书；服务端：是否要求并校验客户端证书。
    ctx->verify_peer = verify_peer ? 1 : 0;
    mbedtls_ssl_conf_authmode(&ctx->conf,
                              ctx->verify_peer ? MBEDTLS_SSL_VERIFY_REQUIRED
                                               : MBEDTLS_SSL_VERIFY_NONE);
    return NL_TLS_OK;
}

int nl_tls_set_hostname(nl_tls_ctx_t* ctx, const char* hostname) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;

    char* copy = NULL;
    if (hostname) {
        size_t len = strlen(hostname);
        if (len == 0 || len > 255) return NL_TLS_ERROR;   // 主机名长度非法
        copy = (char*)malloc(len + 1);
        if (!copy) return NL_TLS_ALLOC_FAIL;
        memcpy(copy, hostname, len + 1);
    }

    free(ctx->hostname);
    ctx->hostname = copy;
    return NL_TLS_OK;
}

int nl_tls_set_dtls(nl_tls_ctx_t* ctx, int enable) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    if (ctx->setup_done) return NL_TLS_INVALID_STATE;   // 必须握手前设置
    ctx->use_dtls = enable ? 1 : 0;
    // 传输类型随 ssl_setup 生效，这里同时更新配置
    ctx->conf.transport = ctx->use_dtls ? MBEDTLS_SSL_TRANSPORT_DATAGRAM
                                        : MBEDTLS_SSL_TRANSPORT_STREAM;
    return NL_TLS_OK;
}

int nl_tls_set_alpn(nl_tls_ctx_t* ctx, const char* const* protocols) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    if (!protocols) return NL_TLS_OK;
    if (mbedtls_ssl_conf_alpn_protocols(&ctx->conf, protocols) != 0) return NL_TLS_BAD_ALPN;
    return NL_TLS_OK;
}

const char* nl_tls_get_alpn(nl_tls_ctx_t* ctx) {
    if (!ctx || !ctx->initialized || !ctx->handshaked) return NULL;
    return mbedtls_ssl_get_alpn_protocol(&ctx->ssl);
}

int nl_tls_set_ciphersuites(nl_tls_ctx_t* ctx, const int* ciphersuites) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    if (!ciphersuites || ciphersuites[0] == 0) return NL_TLS_BAD_CIPHER;
    mbedtls_ssl_conf_ciphersuites(&ctx->conf, ciphersuites);
    return NL_TLS_OK;
}

int nl_tls_set_psk(nl_tls_ctx_t* ctx, const unsigned char* psk,
                   size_t psk_len, const char* identity) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    if (!psk || psk_len == 0 || !identity) return NL_TLS_BAD_PSK;
    if (mbedtls_ssl_conf_psk(&ctx->conf, psk, psk_len,
                             (const unsigned char*)identity, strlen(identity)) != 0) {
        return NL_TLS_BAD_PSK;
    }
    return NL_TLS_OK;
}

int nl_tls_set_crl(nl_tls_ctx_t* ctx, const char* crl_file) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    mbedtls_x509_crl_free(&ctx->crl_chain);
    mbedtls_x509_crl_init(&ctx->crl_chain);
    ctx->crl_loaded = 0;
    if (crl_file) {
        if (mbedtls_x509_crl_parse_file(&ctx->crl_chain, crl_file) != 0) return NL_TLS_BAD_CRL;
        ctx->crl_loaded = 1;
    }
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_chain,
                              ctx->crl_loaded ? &ctx->crl_chain : NULL);
    return NL_TLS_OK;
}

int nl_tls_set_session_tickets(nl_tls_ctx_t* ctx, int enable) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    if (!ctx->is_server) {
        mbedtls_ssl_conf_session_tickets(&ctx->conf,
            enable ? MBEDTLS_SSL_SESSION_TICKETS_ENABLED
                   : MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
        return NL_TLS_OK;
    }
    if (!enable) return NL_TLS_OK;   // 服务端缺省即不发票据
    if (!ctx->ticket_inited) {
        mbedtls_ssl_ticket_init(&ctx->ticket);
        ctx->ticket_inited = 1;
    }
    if (mbedtls_ssl_ticket_setup(&ctx->ticket, mbedtls_ctr_drbg_random,
                                 &ctx->ctr_drbg, MBEDTLS_CIPHER_AES_256_GCM, 86400) != 0) {
        return NL_TLS_ERROR;
    }
    mbedtls_ssl_conf_session_tickets_cb(&ctx->conf, mbedtls_ssl_ticket_write,
                                        mbedtls_ssl_ticket_parse, &ctx->ticket);
    return NL_TLS_OK;
}

int nl_tls_set_session_cache(nl_tls_ctx_t* ctx, int max_entries) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    if (max_entries <= 0) return NL_TLS_OK;   // 服务端缺省即不启用
    if (!ctx->cache_inited) {
        mbedtls_ssl_cache_init(&ctx->cache);
        ctx->cache_inited = 1;
    }
    mbedtls_ssl_cache_set_max_entries(&ctx->cache, max_entries);
    mbedtls_ssl_conf_session_cache(&ctx->conf, &ctx->cache,
                                   mbedtls_ssl_cache_get, mbedtls_ssl_cache_set);
    return NL_TLS_OK;
}

int nl_tls_add_cert(nl_tls_ctx_t* ctx, const char* name,
                    const char* cert_file, const char* key_file) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    if (!name || !cert_file || !key_file) return NL_TLS_BAD_CERT;
    if (ctx->cert_count >= NL_TLS_MAX_CERTS) return NL_TLS_ERROR;

    nl_tls_cert_slot_t* slot = &ctx->certs[ctx->cert_count];
    mbedtls_x509_crt_init(&slot->crt);
    mbedtls_pk_init(&slot->key);
    slot->used = 0;

    if (mbedtls_x509_crt_parse_file(&slot->crt, cert_file) != 0) {
        mbedtls_x509_crt_free(&slot->crt);
        mbedtls_pk_free(&slot->key);
        return NL_TLS_BAD_CERT;
    }
    if (mbedtls_pk_parse_keyfile(&slot->key, key_file, NULL) != 0) {
        mbedtls_x509_crt_free(&slot->crt);
        mbedtls_pk_free(&slot->key);
        return NL_TLS_BAD_KEY;
    }
    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->used = 1;
    ctx->cert_count++;

    // 注册 SNI 回调(服务端)，使握手时能按主机名切换本端证书
    if (ctx->is_server) {
        mbedtls_ssl_conf_sni(&ctx->conf, nl_tls_sni_thunk, ctx);
    }
    return NL_TLS_OK;
}

int nl_tls_set_sni_callback(nl_tls_ctx_t* ctx, nl_tls_sni_cb cb, void* user_data) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    ctx->sni_cb   = cb;
    ctx->sni_user = user_data;
    mbedtls_ssl_conf_sni(&ctx->conf, nl_tls_sni_thunk, ctx);
    return NL_TLS_OK;
}

// 设置底层传输(生物)。timeout_ms > 0 时启用读超时(阻塞/非阻塞均适用)。
static void nl_tls_setup_bio(nl_tls_ctx_t* ctx, int sock, int timeout_ms) {
    ctx->server_fd.fd = sock;
    if (timeout_ms > 0) {
        mbedtls_ssl_conf_read_timeout(&ctx->conf, (uint32_t)timeout_ms);
        mbedtls_ssl_set_bio(&ctx->ssl, &ctx->server_fd, mbedtls_net_send,
                            mbedtls_net_recv, NULL);
    } else {
        mbedtls_ssl_set_bio(&ctx->ssl, &ctx->server_fd, mbedtls_net_send,
                            mbedtls_net_recv, NULL);
    }
}

int nl_tls_handshake_ex(nl_tls_ctx_t* ctx, int sock, int timeout_ms) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;

    int ret;

    // 首次握手前必须先把 ssl 与 conf 绑定，否则后续操作都会失败
    if (!ctx->setup_done) {
        ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
        if (ret != 0) return NL_TLS_HANDSHAKE;
        ctx->setup_done = 1;
    }

    // 设置 SNI 与证书主机名（verify_peer 打开时必需，否则主机名无法校验）
    if (ctx->hostname) {
        ret = mbedtls_ssl_set_hostname(&ctx->ssl, ctx->hostname);
        if (ret != 0) return NL_TLS_HANDSHAKE;
    }

    // 注入会话(仅客户端)：mbedtls_ssl_set_session 会深拷贝
    ctx->reused = 0;
    if (!ctx->is_server && ctx->offered) {
        if (mbedtls_ssl_set_session(&ctx->ssl, &ctx->offered->s) == 0) {
            ctx->reused = 1;   // 已提出复用请求；握手后据 id 复核
        }
    }

    nl_tls_setup_bio(ctx, sock, timeout_ms);
    if (ctx->use_dtls) {
        mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->dtls_timer,
                                 nl_tls_timer_set, nl_tls_timer_get);
    }

    long long deadline = timeout_ms > 0 ? nl_tls_now_ms() + timeout_ms : 0;

    for (int spins = 0; ; spins++) {
        ret = mbedtls_ssl_handshake(&ctx->ssl);
        if (ret == 0) break;

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            int remain = -1;
            if (timeout_ms > 0) {
                remain = (int)(deadline - nl_tls_now_ms());
                if (remain <= 0) return NL_TLS_TIMEOUT;
            } else if (spins > 1000000) {
                return NL_TLS_HANDSHAKE;   // 兜底：避免非阻塞 socket 上无限自旋
            }
            // 等待套接字就绪，避免忙等；失败视为连接错误
            if (nl_tls_sock_wait((NL_TLS_SOCK)sock,
                                 ret == MBEDTLS_ERR_SSL_WANT_WRITE, remain) <= 0) {
                if (timeout_ms > 0 && nl_tls_now_ms() >= deadline) return NL_TLS_TIMEOUT;
            }
            continue;
        }

        if (ret == MBEDTLS_ERR_SSL_TIMEOUT) return NL_TLS_TIMEOUT;
        return NL_TLS_HANDSHAKE;
    }

    ctx->handshaked = 1;

    // 会话复用复核：仅会话 ID 方式的恢复可被可靠识别(票据方式留待调用方按需判断)
    if (ctx->reused && ctx->offered) {
        const mbedtls_ssl_session* neg = mbedtls_ssl_get_session_pointer(&ctx->ssl);
        ctx->reused = (neg && neg->id_len > 0 &&
                       neg->id_len == ctx->offered->s.id_len &&
                       memcmp(neg->id, ctx->offered->s.id, neg->id_len) == 0) ? 1 : 0;
    }

    return NL_TLS_OK;
}

int nl_tls_handshake(nl_tls_ctx_t* ctx, int sock) {
    return nl_tls_handshake_ex(ctx, sock, 0);
}

int nl_tls_send(nl_tls_ctx_t* ctx, const void* buf, size_t len) {
    if (!ctx) return NL_TLS_ERROR;
    if (!ctx->initialized) return NL_TLS_NOT_INIT;

    int ret = mbedtls_ssl_write(&ctx->ssl, (const unsigned char*)buf, len);
    // 非阻塞 I/O 下“需要重试”的语义必须保留，不能当作写错误吞掉
    if (ret == MBEDTLS_ERR_SSL_WANT_READ)  return NL_TLS_WANT_READ;
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) return NL_TLS_WANT_WRITE;
    if (ret < 0) {
        return NL_TLS_WRITE;
    }
    return ret;
}

int nl_tls_recv(nl_tls_ctx_t* ctx, void* buf, size_t len) {
    if (!ctx) return NL_TLS_ERROR;
    if (!ctx->initialized) return NL_TLS_NOT_INIT;

    int ret = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)buf, len);
    // 非阻塞 I/O 下“需要重试”的语义必须保留，不能当作读错误吞掉
    if (ret == MBEDTLS_ERR_SSL_WANT_READ)  return NL_TLS_WANT_READ;
    if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) return NL_TLS_WANT_WRITE;
    if (ret < 0) {
        return NL_TLS_READ;
    }
    return ret;
}

int nl_tls_close(nl_tls_ctx_t* ctx) {
    if (!ctx) return NL_TLS_ERROR;
    if (!ctx->initialized) return NL_TLS_NOT_INIT;

    if (ctx->handshaked) {
        mbedtls_ssl_close_notify(&ctx->ssl);
    }
    // server_fd 只是外部 fd 的包装：这里只解除绑定，绝不调用 mbedtls_net_free，
    // 以免关闭由调用方拥有并负责关闭的套接字
    NL_TLS_CLOSESOCK(ctx->server_fd.fd);
    ctx->server_fd.fd = -1;
    return NL_TLS_OK;
}

int nl_tls_is_active(const nl_tls_ctx_t* ctx) {
    if (!ctx || !ctx->initialized) return 0;
    return ctx->ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER;
}

const char* nl_tls_get_peer_cert(nl_tls_ctx_t* ctx) {
    if (!ctx || !ctx->initialized) return NULL;

    const mbedtls_x509_crt* cert = mbedtls_ssl_get_peer_cert(&ctx->ssl);
    if (!cert) return NULL;

    // 写入上下文自带的缓冲区并检查返回值，避免使用非线程安全的 static 缓冲
    int ret = mbedtls_x509_crt_info(ctx->peer_cert_info,
                                    sizeof(ctx->peer_cert_info) - 1, "", cert);
    if (ret <= 0) return NULL;

    ctx->peer_cert_info[sizeof(ctx->peer_cert_info) - 1] = '\0';
    return ctx->peer_cert_info;
}

// ============================================================
// Session Resumption
// ============================================================

int nl_tls_get_session(nl_tls_ctx_t* ctx, nl_tls_session_t** out) {
    if (!ctx || !ctx->initialized || !out) return NL_TLS_INVALID_STATE;
    *out = NULL;
    if (!ctx->handshaked) return NL_TLS_NO_SESSION;

    nl_tls_session_t* s = (nl_tls_session_t*)calloc(1, sizeof(*s));
    if (!s) return NL_TLS_ALLOC_FAIL;
    mbedtls_ssl_session_init(&s->s);
    s->inited = 1;

    if (mbedtls_ssl_get_session(&ctx->ssl, &s->s) != 0) {
        mbedtls_ssl_session_free(&s->s);
        free(s);
        return NL_TLS_NO_SESSION;
    }
    *out = s;
    return NL_TLS_OK;
}

int nl_tls_set_session(nl_tls_ctx_t* ctx, const nl_tls_session_t* session) {
    if (!ctx || !ctx->initialized) return NL_TLS_INVALID_STATE;
    if (ctx->is_server) return NL_TLS_INVALID_STATE;   // 仅客户端可通过 API 恢复
    // 借用调用方的会话句柄；实际深拷贝发生在握手时(mbedtls_ssl_set_session)。
    // 因此该句柄必须存活至本次握手完成。
    ctx->offered = session;
    return NL_TLS_OK;
}

void nl_tls_session_free(nl_tls_session_t* session) {
    if (!session) return;
    if (session->inited) mbedtls_ssl_session_free(&session->s);
    free(session);
}

int nl_tls_session_reused(const nl_tls_ctx_t* ctx) {
    if (!ctx || !ctx->initialized) return 0;
    return ctx->reused ? 1 : 0;
}

// ============================================================
// Negotiation Info
// ============================================================

const char* nl_tls_get_proto_version(const nl_tls_ctx_t* ctx) {
    if (!ctx || !ctx->initialized || !ctx->handshaked) return NULL;
    return mbedtls_ssl_get_version(&ctx->ssl);
}

const char* nl_tls_get_ciphersuite(const nl_tls_ctx_t* ctx) {
    if (!ctx || !ctx->initialized || !ctx->handshaked) return NULL;
    return mbedtls_ssl_get_ciphersuite(&ctx->ssl);
}

const char* nl_tls_strerror(int err) {
    if (err == 0) return "Success";
    if (err == NL_TLS_BAD_PSK)    return "Bad pre-shared key";
    if (err == NL_TLS_BAD_CRL)    return "Bad CRL";
    if (err == NL_TLS_BAD_ALPN)   return "Bad ALPN configuration";
    if (err == NL_TLS_BAD_CIPHER) return "Bad ciphersuite configuration";
    if (err == NL_TLS_TIMEOUT)    return "Handshake timeout";
    if (err == NL_TLS_NO_SESSION) return "No session available";
    return tls_error_string(err);
}

// ============================================================
// Module Integration
// ============================================================

static nl_module_info_t g_tls_module_info = {
    .type            = NL_MODULE_TLS,
    .name            = "TLS/SSL",
    .description     = "TLS/SSL encryption support using built-in mbedTLS",
    .version         = NL_TLS_VERSION,
    .init            = NULL,
    .shutdown        = NULL,
    .platform_windows = 1,
    .platform_linux   = 1,
    .platform_macos   = 1,
    .capabilities    = NL_CAP_EXT_SYSTEM | NL_CAP_TLS,
    .lazy_load       = NULL,
    .lazy_unload     = NULL,
    .lazy_status     = NL_MODULE_LAZY_UNLOADED
};

nl_module_info_t* nl_tls_get_module_info(void) {
    return &g_tls_module_info;
}

int nl_tls_init(void) {
    nl_tls_register_lang();
    return nl_module_register(&g_tls_module_info);
}

const char* nl_tls_version(void) {
    return NL_TLS_VERSION;
}

int nl_tls_is_available(void) {
    return 1;
}

#else /* NL_TLS_ENABLE */

// Stub implementation when TLS is disabled
#include <stdlib.h>
#include <string.h>

struct nl_tls_ctx { int dummy; };
struct nl_tls_session { int dummy; };

nl_tls_ctx_t* nl_tls_create(void) { return NULL; }
void nl_tls_destroy(nl_tls_ctx_t* ctx) { (void)ctx; }
int nl_tls_configure(nl_tls_ctx_t* ctx, const nl_tls_config_t* cfg) {
    (void)ctx; (void)cfg; return NL_TLS_NOT_INIT;
}
int nl_tls_set_verify(nl_tls_ctx_t* ctx, int verify_peer) {
    (void)ctx; (void)verify_peer; return NL_TLS_NOT_INIT;
}
int nl_tls_set_hostname(nl_tls_ctx_t* ctx, const char* hostname) {
    (void)ctx; (void)hostname; return NL_TLS_NOT_INIT;
}
int nl_tls_set_dtls(nl_tls_ctx_t* ctx, int enable) {
    (void)ctx; (void)enable; return NL_TLS_NOT_INIT;
}
int nl_tls_set_alpn(nl_tls_ctx_t* ctx, const char* const* protocols) {
    (void)ctx; (void)protocols; return NL_TLS_NOT_INIT;
}
const char* nl_tls_get_alpn(nl_tls_ctx_t* ctx) { (void)ctx; return NULL; }
int nl_tls_set_ciphersuites(nl_tls_ctx_t* ctx, const int* ciphersuites) {
    (void)ctx; (void)ciphersuites; return NL_TLS_NOT_INIT;
}
int nl_tls_set_psk(nl_tls_ctx_t* ctx, const unsigned char* psk,
                   size_t psk_len, const char* identity) {
    (void)ctx; (void)psk; (void)psk_len; (void)identity; return NL_TLS_NOT_INIT;
}
int nl_tls_set_crl(nl_tls_ctx_t* ctx, const char* crl_file) {
    (void)ctx; (void)crl_file; return NL_TLS_NOT_INIT;
}
int nl_tls_set_session_tickets(nl_tls_ctx_t* ctx, int enable) {
    (void)ctx; (void)enable; return NL_TLS_NOT_INIT;
}
int nl_tls_set_session_cache(nl_tls_ctx_t* ctx, int max_entries) {
    (void)ctx; (void)max_entries; return NL_TLS_NOT_INIT;
}
int nl_tls_add_cert(nl_tls_ctx_t* ctx, const char* name,
                    const char* cert_file, const char* key_file) {
    (void)ctx; (void)name; (void)cert_file; (void)key_file; return NL_TLS_NOT_INIT;
}
int nl_tls_set_sni_callback(nl_tls_ctx_t* ctx, nl_tls_sni_cb cb, void* user_data) {
    (void)ctx; (void)cb; (void)user_data; return NL_TLS_NOT_INIT;
}
int nl_tls_handshake(nl_tls_ctx_t* ctx, int sock) {
    (void)ctx; (void)sock; return NL_TLS_NOT_INIT;
}
int nl_tls_handshake_ex(nl_tls_ctx_t* ctx, int sock, int timeout_ms) {
    (void)ctx; (void)sock; (void)timeout_ms; return NL_TLS_NOT_INIT;
}
int nl_tls_send(nl_tls_ctx_t* ctx, const void* buf, size_t len) {
    (void)ctx; (void)buf; (void)len; return NL_TLS_NOT_INIT;
}
int nl_tls_recv(nl_tls_ctx_t* ctx, void* buf, size_t len) {
    (void)ctx; (void)buf; (void)len; return NL_TLS_NOT_INIT;
}
int nl_tls_close(nl_tls_ctx_t* ctx) {
    (void)ctx; return NL_TLS_NOT_INIT;
}
int nl_tls_is_active(const nl_tls_ctx_t* ctx) {
    (void)ctx; return 0;
}
const char* nl_tls_get_peer_cert(nl_tls_ctx_t* ctx) {
    (void)ctx; return NULL;
}
int nl_tls_get_session(nl_tls_ctx_t* ctx, nl_tls_session_t** out) {
    (void)ctx; (void)out; return NL_TLS_NOT_INIT;
}
int nl_tls_set_session(nl_tls_ctx_t* ctx, const nl_tls_session_t* session) {
    (void)ctx; (void)session; return NL_TLS_NOT_INIT;
}
void nl_tls_session_free(nl_tls_session_t* session) { (void)session; }
int nl_tls_session_reused(const nl_tls_ctx_t* ctx) { (void)ctx; return 0; }
const char* nl_tls_get_proto_version(const nl_tls_ctx_t* ctx) { (void)ctx; return NULL; }
const char* nl_tls_get_ciphersuite(const nl_tls_ctx_t* ctx) { (void)ctx; return NULL; }
const char* nl_tls_strerror(int err) {
    (void)err; return "TLS not available";
}

nl_module_info_t* nl_tls_get_module_info(void) { return NULL; }
int nl_tls_init(void) { return 0; }
const char* nl_tls_version(void) { return "not available"; }
int nl_tls_is_available(void) { return 0; }

#endif /* NL_TLS_ENABLE */
