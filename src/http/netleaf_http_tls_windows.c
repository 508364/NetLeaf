#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wctype.h>

/*
 * Windows 平台 HTTPS（TLS）适配层。
 *
 * 设计：纯 HTTP/HTTPS 逻辑已抽到 src/http/netleaf_http_common.c（平台无关），
 * Winsock 生命周期 / 线程 / H1/H2 客户端处理在 src/windows/optimize/netleaf_http_windows.c，
 * 本文件只承载 TLS/HTTPS 专属代码：
 *   - TLS 后端抽象宏（NL_HTTPS_USE_TLS3 切换 tls3 / tls2，句柄经 SOCKET<->int 双向转换）
 *   - nl_http_server_enable_tls（在已有 HTTP server 上附加服务端 TLS 配置）
 *   - nl_http_client_connect / close / request（TCP+TLS 握手 + 加密请求/响应）
 *
 * CMake 层：netleaf_https 目标仅编译本文件（不链接 netleaf_http），
 * 依赖 nl_http_server_t（由 netleaf_http 提供）与 netleaf_tls3/tls2。
 */

/* 底层 TLS 后端由 NL_HTTPS_USE_TLS3 切换（CMake 编译定义驱动） */
#if defined(NL_HTTPS_USE_TLS3)
#define NL_HTTPS_TLS_CTX       nl_tls3_ctx_t
#define NL_HTTPS_TLS_CONFIG    nl_tls3_config_t
#define NL_HTTPS_TLS_OK        NL_TLS3_OK
#define NL_HTTPS_TLS_create    nl_tls3_create
#define NL_HTTPS_TLS_destroy   nl_tls3_destroy
#define NL_HTTPS_TLS_configure nl_tls3_configure
#define NL_HTTPS_TLS_set_hostname nl_tls3_set_hostname
#define NL_HTTPS_TLS_handshake_ex nl_tls3_handshake_ex
#define NL_HTTPS_TLS_send      nl_tls3_send
#define NL_HTTPS_TLS_recv      nl_tls3_recv
#define NL_HTTPS_TLS_is_available nl_tls3_is_available
#else
#define NL_HTTPS_TLS_CTX       nl_tls2_ctx_t
#define NL_HTTPS_TLS_CONFIG    nl_tls2_config_t
#define NL_HTTPS_TLS_OK        NL_TLS2_OK
#define NL_HTTPS_TLS_create    nl_tls2_create
#define NL_HTTPS_TLS_destroy   nl_tls2_destroy
#define NL_HTTPS_TLS_configure nl_tls2_configure
#define NL_HTTPS_TLS_set_hostname nl_tls2_set_hostname
#define NL_HTTPS_TLS_handshake_ex nl_tls2_handshake_ex
#define NL_HTTPS_TLS_send      nl_tls2_send
#define NL_HTTPS_TLS_recv      nl_tls2_recv
#define NL_HTTPS_TLS_is_available nl_tls2_is_available
#endif

/* SOCKET -> int 供 TLS API（底层 mbedtls 用 int 句柄） */
#define NL_HTTPS_TO_INT(s) ((int)(s))

/* 内部库源码构建：调用原长名实现体不产生 deprecated 告警 */
#define NL_HTTP_INTERNAL_BUILD
#include "netleaf_http.h"

/* netleaf_http_windows.c 定义的 server 结构（含 TLS 字段） */
struct nl_http_server {
    SOCKET fd;
    int port;
    int running;
    int enable_http2;
    int enable_http3;
    HANDLE thread;
    nl_http_handler handler;
    void* user_data;
    /* 本库（netleaf_https）启用 TLS 时存在 */
    NL_HTTPS_TLS_CONFIG tls_cfg;
    int tls_enabled;
};

/* HTTPS 客户端：封装 TCP socket + TLS 上下文 + 目标主机名 */
struct nl_http_client {
    SOCKET sock;
    NL_HTTPS_TLS_CTX* tls_ctx;
    char* hostname;
};

int nl_http_server_enable_tls(nl_http_server_t* server,
                              const nl_http_tls_server_cfg_t* cfg) {
    if (!NL_HTTPS_TLS_is_available()) return NL_HTTP_TLS_UNAVAILABLE;
    if (!server || !cfg || !cfg->cert_file || !cfg->key_file)
        return NL_HTTP_TLS_UNAVAILABLE;
    memset(&server->tls_cfg, 0, sizeof(server->tls_cfg));
    server->tls_cfg.cert_file   = cfg->cert_file;
    server->tls_cfg.key_file    = cfg->key_file;
    server->tls_cfg.key_pass    = cfg->key_pass;
    server->tls_cfg.is_server   = 1;
    server->tls_cfg.client_auth = cfg->client_auth;
    server->tls_cfg.handshake_timeout_ms = 10000;
    server->tls_enabled = 1;
    return 0;
}

nl_http_client_t* nl_http_client_connect(const char* host, int port,
                                          const nl_http_tls_client_cfg_t* cfg) {
    if (!NL_HTTPS_TLS_is_available()) return NULL;
    if (!host || !cfg) return NULL;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) return NULL;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        return NULL;
    }
    struct sockaddr_in* sa = (struct sockaddr_in*)res->ai_addr;
    if (connect(sock, (struct sockaddr*)sa, sizeof(struct sockaddr_in)) < 0) {
        freeaddrinfo(res);
        closesocket(sock);
        return NULL;
    }
    freeaddrinfo(res);

    NL_HTTPS_TLS_CTX* tls_ctx = NL_HTTPS_TLS_create();
    if (!tls_ctx) {
        closesocket(sock);
        return NULL;
    }

    NL_HTTPS_TLS_CONFIG tls_cfg;
    memset(&tls_cfg, 0, sizeof(tls_cfg));
    tls_cfg.ca_file    = cfg->ca_file;
    tls_cfg.cert_file  = cfg->cert_file;
    tls_cfg.key_file   = cfg->key_file;
    tls_cfg.verify_peer = cfg->verify ? 1 : 0;
    tls_cfg.is_server  = 0;
    tls_cfg.handshake_timeout_ms = 10000;

    if (NL_HTTPS_TLS_configure(tls_ctx, &tls_cfg) != NL_HTTPS_TLS_OK ||
        NL_HTTPS_TLS_set_hostname(tls_ctx, host) != NL_HTTPS_TLS_OK ||
        NL_HTTPS_TLS_handshake_ex(tls_ctx, NL_HTTPS_TO_INT(sock), 10000) != NL_HTTPS_TLS_OK) {
        NL_HTTPS_TLS_destroy(tls_ctx);
        closesocket(sock);
        return NULL;
    }

    nl_http_client_t* client = calloc(1, sizeof(nl_http_client_t));
    if (!client) {
        NL_HTTPS_TLS_destroy(tls_ctx);
        closesocket(sock);
        return NULL;
    }

    client->sock    = sock;
    client->tls_ctx = tls_ctx;
    client->hostname = _strdup(host);
    if (!client->hostname) {
        NL_HTTPS_TLS_destroy(tls_ctx);
        closesocket(sock);
        free(client);
        return NULL;
    }

    return client;
}

void nl_http_client_close(nl_http_client_t* client) {
    if (!client) return;
    if (client->tls_ctx) NL_HTTPS_TLS_destroy(client->tls_ctx);
    if (client->sock != INVALID_SOCKET) closesocket(client->sock);
    free(client->hostname);
    free(client);
}

int nl_http_client_request(nl_http_client_t* client,
                            const char* method,
                            const char* path,
                            const char* body,
                            size_t body_len,
                            char* out_buf,
                            size_t out_buf_size,
                            size_t* out_body_len) {
    if (!client || !method || !path) return NL_HTTP_TLS_UNAVAILABLE;

    char req_buf[8192];
    int req_len;
    if (body && body_len > 0) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, client->hostname, body_len);
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, path, client->hostname);
    }
    if (req_len < 0) return NL_HTTP_TLS_UNAVAILABLE;

    if (NL_HTTPS_TLS_send(client->tls_ctx, req_buf, (size_t)req_len) != req_len)
        return NL_HTTP_TLS_UNAVAILABLE;

    if (body && body_len > 0) {
        if (NL_HTTPS_TLS_send(client->tls_ctx, body, body_len) != (int)body_len)
            return NL_HTTP_TLS_UNAVAILABLE;
    }

    if (out_buf && out_buf_size > 0) {
        size_t total = 0;
        while (total < out_buf_size - 1) {
            int n = NL_HTTPS_TLS_recv(client->tls_ctx, out_buf + total,
                                (int)(out_buf_size - 1 - total));
            if (n <= 0) break;
            total += n;
        }
        out_buf[total] = '\0';
        if (out_body_len) *out_body_len = total;
    }

    if (out_buf && out_buf_size > 0) {
        char status_str[64] = {0};
        int status = 200;
        if (out_buf[0] == 'H') {
            int i = 0;
            while (out_buf[i] && out_buf[i] != '\r' && out_buf[i] != ' ' && i < 63)
                i++;
            while (out_buf[i] == ' ') i++;
            int j = i;
            while (out_buf[j] && out_buf[j] != ' ' && out_buf[j] != '\r' && j < 63)
                j++;
            if (j > i) {
                int len = j - i;
                memcpy(status_str, out_buf + i, len > 63 ? 63 : len);
                status_str[len > 63 ? 63 : len] = '\0';
                status = atoi(status_str);
            }
        }
        return status;
    }

    return 0;
}
