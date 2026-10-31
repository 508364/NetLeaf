#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <pthread.h>

/*
 * Linux 平台 HTTPS（TLS）适配层。
 *
 * 设计：纯 HTTP/HTTPS 逻辑已抽到 src/http/netleaf_http_common.c（平台无关），
 * POSIX 生命周期 / 线程 / H1/H2 客户端处理在 src/linux/optimize/netleaf_http_linux.c，
 * 本文件只承载 TLS/HTTPS 专属代码：
 *   - TLS 后端抽象宏（NL_HTTPS_USE_TLS3 切换 tls3 / tls2，句柄为 int 直接传递）
 *   - nl_http_server_enable_tls（在已有 HTTP server 上附加服务端 TLS 配置）
 *   - nl_http_client_connect / close / request（TCP+TLS 握手 + 加密请求/响应）
 *
 * CMake 层：netleaf_https 目标仅编译本文件，链接 netleaf_tls3/tls2。
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
#define NL_HTTPS_TLS_get_alpn  nl_tls3_get_alpn
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
#define NL_HTTPS_TLS_get_alpn  nl_tls2_get_alpn
#endif

/* 平台句柄/线程/锁宏：须在 include internal.h 前定义（同 netleaf_http_linux.c） */
#define NL_HTTPS_SOCK     int
#define NL_HTTPS_CLOSOBJ(s) close(s)
#define NL_HTTPS_INVALID  (-1)
#define NL_HTTPS_SOCK_VALID(s) ((s) >= 0)
#define NL_HTTP_THREAD    pthread_t
#define NL_HTTP_MUTEX     pthread_mutex_t
#define NL_HTTP_MUTEX_INIT(m)    pthread_mutex_init(m, NULL)
#define NL_HTTP_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#define NL_HTTP_MUTEX_LOCK(m)    pthread_mutex_lock(m)
#define NL_HTTP_MUTEX_UNLOCK(m)  pthread_mutex_unlock(m)

/* 内部库源码构建：调用原长名实现体不产生 deprecated 告警 */
#define NL_HTTP_INTERNAL_BUILD
#include "netleaf_http.h"
#include "http/netleaf_http_internal.h"

/* HTTPS 客户端：封装 socket + TLS 上下文 + 目标主机名 */
struct nl_http_client {
    int sock;
    NL_HTTPS_TLS_CTX* tls_ctx;
    char* hostname;
};

int nl_http_server_enable_tls(nl_http_server_t* server,
                              const nl_http_tls_server_cfg_t* cfg) {
    /* v3.1.0：核心库 nl_http_server_t 由 netleaf_core 分配，布局不含 TLS
     * 字段且其 accept 线程不感知 TLS——历史实现向本视图的 tls_cfg/tls_enabled
     * 写入实为越界写，且配置从未被消费。现固定返回不可用，服务端 TLS /
     * HTTPS-2 请使用本文件提供的 nlh_tls_server_*。 */
    (void)server; (void)cfg;
    return NL_HTTP_TLS_UNAVAILABLE;
}

nl_http_client_t* nl_http_client_connect(const char* host, int port,
                                          const nl_http_tls_client_cfg_t* cfg) {
    if (!NL_HTTPS_TLS_is_available()) return NULL;
    if (!host || !cfg) return NULL;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        close(sock);
        return NULL;
    }
    struct sockaddr_in* sa = (struct sockaddr_in*)res->ai_addr;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = sa->sin_family;
    addr.sin_port   = sa->sin_port;
    addr.sin_addr   = sa->sin_addr;
    freeaddrinfo(res);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return NULL;
    }

    NL_HTTPS_TLS_CTX* tls_ctx = NL_HTTPS_TLS_create();
    if (!tls_ctx) {
        close(sock);
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
        NL_HTTPS_TLS_handshake_ex(tls_ctx, sock, 10000) != NL_HTTPS_TLS_OK) {
        NL_HTTPS_TLS_destroy(tls_ctx);
        close(sock);
        return NULL;
    }

    nl_http_client_t* client = calloc(1, sizeof(nl_http_client_t));
    if (!client) {
        NL_HTTPS_TLS_destroy(tls_ctx);
        close(sock);
        return NULL;
    }

    client->sock      = sock;
    client->tls_ctx   = tls_ctx;
    client->hostname  = strdup(host);
    if (!client->hostname) {
        NL_HTTPS_TLS_destroy(tls_ctx);
        close(sock);
        free(client);
        return NULL;
    }

    return client;
}

void nl_http_client_close(nl_http_client_t* client) {
    if (!client) return;
    if (client->tls_ctx) NL_HTTPS_TLS_destroy(client->tls_ctx);
    close(client->sock);
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
    int req_len = 0;
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

    if (NL_HTTPS_TLS_send(client->tls_ctx, req_buf, req_len) != req_len) {
        return NL_HTTP_TLS_UNAVAILABLE;
    }

    if (body && body_len > 0) {
        if (NL_HTTPS_TLS_send(client->tls_ctx, body, body_len) != (int)body_len)
            return NL_HTTP_TLS_UNAVAILABLE;
    }

    if (out_buf && out_buf_size > 0) {
        size_t total = 0;
        while (total < out_buf_size - 1) {
            int n = NL_HTTPS_TLS_recv(client->tls_ctx, out_buf + total,
                                out_buf_size - 1 - total);
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

/* ============================================================
 * TLS 服务端（HTTPS / HTTPS-2）
 *
 * 独立于核心库 nl_http_server_t：accept 后每连接执行 TLS 握手，
 * 按 ALPN 协商结果分流——"h2" 走 HTTP/2 帧循环（TLS 收发），
 * 其余走 HTTP/1.1（TLS 收发）。H2/H1 帧与解析逻辑复用核心库
 * netleaf_core 导出的公共函数（netleaf_http_internal.h）。
 * ============================================================ */

struct nlh_tls_server {
    int fd;
    int port;
    int running;
    int enable_http2;
    pthread_t thread;
    nl_http_handler handler;
    void* user_data;
    /* 路径字符串自持（create 时复制，避免 cfg 悬垂） */
    char* cert_file;
    char* key_file;
    char* key_pass;
    int client_auth;
};

/* TLS 版 h2 写帧回调：io->user 持有 TLS 上下文 */
static int io_tls_write_h2_frame(struct nl_http_io* io, nl_http_handle_t handle,
                                 const uint8_t* data, size_t len) {
    (void)handle;
    NL_HTTPS_TLS_CTX* tls = (NL_HTTPS_TLS_CTX*)io->user;
    int n = NL_HTTPS_TLS_send(tls, (const char*)data, len);
    return (n < 0) ? -1 : n;
}

/* HTTP/2 over TLS 会话（服务端视角） */
static void nlh_tls_h2_session(NL_HTTPS_TLS_CTX* tls, int sock,
                               nl_http_handler handler, void* user_data) {
    struct nl_http2_connection conn;
    h2_connection_init(&conn, sock);
    NL_HTTP_MUTEX_INIT(&conn.mutex);

    struct nl_http_io io;
    memset(&io, 0, sizeof(io));
    io.write_h2_frame = io_tls_write_h2_frame;
    io.user = tls;

    char buffer[BUFFER_SIZE];
    int bytes_read = NL_HTTPS_TLS_recv(tls, buffer, sizeof(buffer));

    if (bytes_read >= H2_PREFACE_LEN && memcmp(buffer, h2_preface, H2_PREFACE_LEN) == 0) {
        conn.preface_sent = 1;
        h2_send_settings(&io, sock, &conn);

        size_t remaining = (size_t)bytes_read - H2_PREFACE_LEN;
        if (remaining > 0) {
            h2_process_frame(&io, sock, &conn, (uint8_t*)buffer + H2_PREFACE_LEN,
                             remaining, handler, user_data);
        }

        while (1) {
            bytes_read = NL_HTTPS_TLS_recv(tls, buffer, sizeof(buffer));
            if (bytes_read <= 0) break;

            size_t offset = 0;
            while (offset < (size_t)bytes_read) {
                if (offset + 9 > (size_t)bytes_read) break;

                struct nl_h2_frame_header header;
                h2_frame_header_read((uint8_t*)buffer + offset, &header);
                if (offset + 9 + header.length > (size_t)bytes_read) break;

                h2_process_frame(&io, sock, &conn, (uint8_t*)buffer + offset,
                                 9 + header.length, handler, user_data);
                offset += 9 + header.length;
            }
        }
    }

    NL_HTTP_MUTEX_DESTROY(&conn.mutex);
    h2_connection_cleanup(&conn);
}

/* HTTP/1.1 over TLS 会话（keep-alive 语义同核心库 http1_client_thread） */
static void nlh_tls_h1_session(NL_HTTPS_TLS_CTX* tls,
                               nl_http_handler handler, void* user_data) {
    int keep_alive = 1;
    while (keep_alive) {
        char buffer[BUFFER_SIZE];
        size_t total = 0;
        while (total < sizeof(buffer) - 1) {
            int n = NL_HTTPS_TLS_recv(tls, buffer + total, sizeof(buffer) - 1 - total);
            if (n <= 0) { total = 0; break; }
            total += (size_t)n;
            buffer[total] = '\0';
            if (nl_http_find_terminator(buffer, total) != -1) break;
        }
        if (total == 0) break;

        nl_http_request_t req;
        nl_http_response_t resp;
        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));
        resp.status = 200;
        int parsed = 0;

        if (nl_http_parse_request(&req, buffer, total) == 0) {
            parsed = 1;
            const char* conn_hdr = nl_http_request_get_header(&req, "Connection");
            if (conn_hdr && strcasecmp(conn_hdr, "close") == 0) keep_alive = 0;
            if (req.version == NL_HTTP_VERSION_1_0) {
                const char* ka = nl_http_request_get_header(&req, "Keep-Alive");
                if (!ka) keep_alive = 0;
            }
            if (handler) {
                handler(&req, &resp, user_data);
            } else {
                nl_http_response_set_body(&resp,
                    "<html><body><h1>Hello, NetLeaf HTTPS/1.1!</h1></body></html>", 58);
            }
        } else {
            keep_alive = 0;
        }

        char* response_data = NULL;
        size_t response_len = 0;
        if (keep_alive && parsed)
            nl_http_response_set_header(&resp, "Connection", "keep-alive");
        nl_http_generate_response_http1(&resp, &response_data, &response_len);
        if (response_data) {
            if (response_len > 0) NL_HTTPS_TLS_send(tls, response_data, response_len);
            free(response_data);
        }
        if (req.body) free(req.body);
    }
}

typedef struct {
    int sock;
    nlh_tls_server_t* srv;
} nlh_tls_conn_args_t;

static void* nlh_tls_conn_thread(void* arg) {
    nlh_tls_conn_args_t* args = (nlh_tls_conn_args_t*)arg;
    int sock = args->sock;
    nlh_tls_server_t* srv = args->srv;
    free(args);

    NL_HTTPS_TLS_CTX* tls = NL_HTTPS_TLS_create();
    if (!tls) { fprintf(stderr, "[nlh-tls] TLS_create 失败\n"); close(sock); return NULL; }

    NL_HTTPS_TLS_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cert_file = srv->cert_file;
    cfg.key_file  = srv->key_file;
    cfg.key_pass  = srv->key_pass;
    cfg.is_server = 1;
    cfg.client_auth = srv->client_auth;
    cfg.handshake_timeout_ms = 10000;
    static const char* const alpn_full[] = { "h2", "http/1.1", NULL };
    static const char* const alpn_h1[]   = { "http/1.1", NULL };
    cfg.alpn = srv->enable_http2 ? alpn_full : alpn_h1;

    int rc_cfg = NL_HTTPS_TLS_configure(tls, &cfg);
    if (rc_cfg != NL_HTTPS_TLS_OK) {
        fprintf(stderr, "[nlh-tls] configure 失败 rc=%d\n", rc_cfg);
        NL_HTTPS_TLS_destroy(tls);
        close(sock);
        return NULL;
    }
    int rc_hs = NL_HTTPS_TLS_handshake_ex(tls, sock, 10000);
    if (rc_hs != NL_HTTPS_TLS_OK) {
        fprintf(stderr, "[nlh-tls] handshake 失败 rc=%d\n", rc_hs);
        NL_HTTPS_TLS_destroy(tls);
        close(sock);
        return NULL;
    }

    const char* proto = NL_HTTPS_TLS_get_alpn(tls);
    if (proto && strcmp(proto, "h2") == 0) {
        nlh_tls_h2_session(tls, sock, srv->handler, srv->user_data);
    } else {
        nlh_tls_h1_session(tls, srv->handler, srv->user_data);
    }

    NL_HTTPS_TLS_destroy(tls);
    close(sock);
    return NULL;
}

static void* nlh_tls_server_thread(void* arg) {
    nlh_tls_server_t* srv = (nlh_tls_server_t*)arg;

    while (srv->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(srv->fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock >= 0) {
            nlh_tls_conn_args_t* args = malloc(sizeof(nlh_tls_conn_args_t));
            if (!args) { close(client_sock); continue; }
            args->sock = client_sock;
            args->srv = srv;
            pthread_t th;
            if (pthread_create(&th, NULL, nlh_tls_conn_thread, args) != 0) {
                free(args);
                close(client_sock);
            } else {
                pthread_detach(th);
            }
        }
    }
    return NULL;
}

nlh_tls_server_t* nlh_tls_server_create(int port, const nl_http_tls_server_cfg_t* cfg) {
    if (!NL_HTTPS_TLS_is_available()) return NULL;
    if (!cfg || !cfg->cert_file || !cfg->key_file) return NULL;

    nlh_tls_server_t* srv = calloc(1, sizeof(nlh_tls_server_t));
    if (!srv) return NULL;

    srv->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->fd < 0) { free(srv); return NULL; }

    int opt = 1;
    setsockopt(srv->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(srv->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(srv->fd); free(srv); return NULL;
    }
    if (listen(srv->fd, SOMAXCONN) < 0) {
        close(srv->fd); free(srv); return NULL;
    }

    srv->port = port;
    srv->enable_http2 = 1;
    srv->cert_file = strdup(cfg->cert_file);
    srv->key_file  = strdup(cfg->key_file);
    srv->key_pass  = cfg->key_pass ? strdup(cfg->key_pass) : NULL;
    srv->client_auth = cfg->client_auth;
    if (!srv->cert_file || !srv->key_file) {
        nlh_tls_server_destroy(srv);
        return NULL;
    }
    return srv;
}

void nlh_tls_server_set_handler(nlh_tls_server_t* srv, nl_http_handler handler, void* user_data) {
    if (!srv) return;
    srv->handler = handler;
    srv->user_data = user_data;
}

void nlh_tls_server_enable_http2(nlh_tls_server_t* srv, int enable) {
    if (srv) srv->enable_http2 = enable ? 1 : 0;
}

int nlh_tls_server_start(nlh_tls_server_t* srv) {
    if (!srv || srv->running) return -1;
    srv->running = 1;
    if (pthread_create(&srv->thread, NULL, nlh_tls_server_thread, srv) != 0) {
        srv->running = 0;
        return -1;
    }
    return 0;
}

void nlh_tls_server_stop(nlh_tls_server_t* srv) {
    if (!srv || !srv->running) return;
    srv->running = 0;
    close(srv->fd);
    srv->fd = -1;
    pthread_join(srv->thread, NULL);
}

void nlh_tls_server_destroy(nlh_tls_server_t* srv) {
    if (!srv) return;
    if (srv->running) nlh_tls_server_stop(srv);
    if (srv->fd >= 0) close(srv->fd);
    free(srv->cert_file);
    free(srv->key_file);
    free(srv->key_pass);
    free(srv);
}

/* ============================================================
 * TLS 短名（nlh_*，随 netleaf_https 导出）：与公共头声明一一对应。
 * 核心库的 shortnames.c 因 NL_HTTPS_ENABLE 门控不含 TLS 转发，
 * 这些符号由本扩展补全。
 * ============================================================ */

int nlh_server_enable_tls(nl_http_server_t* server, const nl_http_tls_server_cfg_t* cfg) {
    return nl_http_server_enable_tls(server, cfg);
}

nl_http_client_t* nlh_client_connect(const char* host, int port,
                                      const nl_http_tls_client_cfg_t* cfg) {
    return nl_http_client_connect(host, port, cfg);
}

void nlh_client_close(nl_http_client_t* client) {
    nl_http_client_close(client);
}

int nlh_client_request(nl_http_client_t* client,
                       const char* method, const char* path,
                       const char* body, size_t body_len,
                       char* out_buf, size_t out_buf_size,
                       size_t* out_body_len) {
    return nl_http_client_request(client, method, path, body, body_len,
                                  out_buf, out_buf_size, out_body_len);
}
