#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
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

/* SOCKET -> int 供 TLS API（底层 mbedtls 用 int 句柄） */
#define NL_HTTPS_TO_INT(s)     ((int)(s))
#define NL_HTTPS_FROM_INT(i)   ((SOCKET)(i))

/* 平台句柄/线程/锁宏：须在 include internal.h 前定义（同 netleaf_http_windows.c） */
#define NL_HTTPS_SOCK     SOCKET
#define NL_HTTPS_CLOSOBJ(s) closesocket(s)
#define NL_HTTPS_INVALID  INVALID_SOCKET
#define NL_HTTPS_SOCK_VALID(s) ((s) != INVALID_SOCKET)
#define NL_HTTP_THREAD    HANDLE
#define NL_HTTP_MUTEX     CRITICAL_SECTION
#define NL_HTTP_MUTEX_INIT(m)    InitializeCriticalSection(m)
#define NL_HTTP_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#define NL_HTTP_MUTEX_LOCK(m)    EnterCriticalSection(m)
#define NL_HTTP_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)

/* 内部库源码构建：调用原长名实现体不产生 deprecated 告警 */
#define NL_HTTP_INTERNAL_BUILD
#include "netleaf_http.h"
#include "http/netleaf_http_internal.h"

/* nl_log_error 定义于 netleaf_core（src/common/nl_logsys.c），无属性声明
 * 经 netleaf_core 导入库解析（同 linux 适配层做法）。 */
void nl_log_error(const char* format, ...);

/* HTTPS/HTTP 客户端：封装 socket + TLS 上下文 + 目标主机名。
 * tls_ctx == NULL 表示明文 HTTP（cfg 传 NULL 时，v3.1.0 起支持）。 */
struct nl_http_client {
    SOCKET sock;
    NL_HTTPS_TLS_CTX* tls_ctx;
    char* hostname;
};

/* 客户端发送：TLS 循环加密发送（mbedTLS 单次约 16KB 上限），明文循环 send。 */
static int nlh_client_send(nl_http_client_t* client, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n;
        if (client->tls_ctx) {
            n = NL_HTTPS_TLS_send(client->tls_ctx, data + off, len - off);
        } else {
            n = send(client->sock, data + off, (int)(len - off), 0);
        }
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* 客户端接收：分 TLS / 明文；返回读到的字节数，<=0 表示对端关闭/出错。 */
static int nlh_client_recv(nl_http_client_t* client, char* buf, size_t len) {
    if (client->tls_ctx)
        return NL_HTTPS_TLS_recv(client->tls_ctx, buf, (int)len);
    return recv(client->sock, buf, (int)len, 0);
}

int nl_http_server_enable_tls(nl_http_server_t* server,
                              const nl_http_tls_server_cfg_t* cfg) {
    /* v3.1.0：核心库 nl_http_server_t 由 netleaf_core 分配，布局不含 TLS
     * 字段且其 accept 线程不感知 TLS——历史实现向本视图的 tls_cfg/tls_enabled
     * 写入实为越界写，且配置从未被消费。现固定返回不可用，服务端 TLS /
     * HTTPS-2 请使用本文件提供的 nlh_tls_server_*。 */
    (void)server; (void)cfg;
    nl_log_error("[nlh] nlh_server_enable_tls 已停用（返回 UNAVAILABLE），"
                 "服务端 TLS 请改用 nlh_tls_server_*");
    return NL_HTTP_TLS_UNAVAILABLE;
}

nl_http_client_t* nl_http_client_connect(const char* host, int port,
                                          const nl_http_tls_client_cfg_t* cfg) {
    /* cfg == NULL → 明文 HTTP（v3.1.0）；非 NULL → TLS 握手。 */
    if (cfg && !NL_HTTPS_TLS_is_available()) return NULL;
    if (!host) return NULL;

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

    NL_HTTPS_TLS_CTX* tls_ctx = NULL;
    if (cfg) {
        tls_ctx = NL_HTTPS_TLS_create();
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
    }

    nl_http_client_t* client = calloc(1, sizeof(nl_http_client_t));
    if (!client) {
        if (tls_ctx) NL_HTTPS_TLS_destroy(tls_ctx);
        closesocket(sock);
        return NULL;
    }

    client->sock    = sock;
    client->tls_ctx = tls_ctx;
    client->hostname = _strdup(host);
    if (!client->hostname) {
        if (tls_ctx) NL_HTTPS_TLS_destroy(tls_ctx);
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

    if (nlh_client_send(client, req_buf, (size_t)req_len) != 0)
        return NL_HTTP_TLS_UNAVAILABLE;

    if (body && body_len > 0) {
        if (nlh_client_send(client, body, body_len) != 0)
            return NL_HTTP_TLS_UNAVAILABLE;
    }

    if (out_buf && out_buf_size > 0) {
        size_t total = 0;
        while (total < out_buf_size - 1) {
            int n = nlh_client_recv(client, out_buf + total,
                                    out_buf_size - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
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
    SOCKET fd;
    int port;
    int running;
    int enable_http2;
    HANDLE thread;
    nl_http_handler handler;
    void* user_data;
    /* 路径字符串自持（create 时复制，避免 cfg 悬垂） */
    char* cert_file;
    char* key_file;
    char* key_pass;
    int client_auth;
};

/* TLS 版 h2 写帧回调：io->user 持有 TLS 上下文。
 * mbedTLS 单次 send 约 16KB 上限，一帧 9+16384 字节会被部分写，
 * 必须循环发完，否则对端收到截断帧后连接挂死。 */
static int io_tls_write_h2_frame(struct nl_http_io* io, nl_http_handle_t handle,
                                 const uint8_t* data, size_t len) {
    (void)handle;
    NL_HTTPS_TLS_CTX* tls = (NL_HTTPS_TLS_CTX*)io->user;
    size_t off = 0;
    while (off < len) {
        int n = NL_HTTPS_TLS_send(tls, (const char*)data + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return (int)len;
}

/* HTTP/2 over TLS 会话（服务端视角） */
static void nlh_tls_h2_session(NL_HTTPS_TLS_CTX* tls, SOCKET sock,
                               nl_http_handler handler, void* user_data) {
    struct nl_http2_connection conn;
    h2_connection_init(&conn, NL_HTTPS_TO_INT(sock));
    NL_HTTP_MUTEX_INIT(&conn.mutex);

    struct nl_http_io io;
    memset(&io, 0, sizeof(io));
    io.write_h2_frame = io_tls_write_h2_frame;
    io.user = tls;

    /* 累积缓冲：TLS record 边界与 H2 帧边界无关，帧头与载荷可能被
     * 拆分到多次 recv（客户端分帧头/HPACK 两次写即触发），必须跨
     * recv 拼帧后再解析，否则不完整帧会被静默丢弃、请求永不响应 */
    size_t cap = BUFFER_SIZE;
    uint8_t* acc = (uint8_t*)malloc(cap);
    if (!acc) {
        NL_HTTP_MUTEX_DESTROY(&conn.mutex);
        h2_connection_cleanup(&conn);
        return;
    }
    size_t have = 0;
    int preface_done = 0;

    for (;;) {
        if (have == cap) {          /* 半帧积压：扩容等待后续数据 */
            size_t ncap = cap * 2;
            uint8_t* nacc = (uint8_t*)realloc(acc, ncap);
            if (!nacc) break;
            acc = nacc;
            cap = ncap;
        }
        int n = NL_HTTPS_TLS_recv(tls, (char*)acc + have, (int)(cap - have));
        if (n <= 0) break;
        have += (size_t)n;

        size_t offset = 0;
        if (!preface_done) {
            if (have < H2_PREFACE_LEN) continue;
            if (memcmp(acc, h2_preface, H2_PREFACE_LEN) != 0) break;  /* 非法客户端 */
            preface_done = 1;
            conn.preface_sent = 1;
            h2_send_settings(&io, NL_HTTPS_TO_INT(sock), &conn);
            offset = H2_PREFACE_LEN;
        }

        while (offset + 9 <= have) {
            struct nl_h2_frame_header header;
            h2_frame_header_read(acc + offset, &header);
            if (header.length > H2_MAX_FRAME_SIZE) { have = 0; goto done; } /* 协议违规 */
            if (offset + 9 + header.length > have) break;  /* 半帧，留待下次 recv */

            h2_process_frame(&io, NL_HTTPS_TO_INT(sock), &conn, acc + offset,
                             9 + header.length, handler, user_data);
            offset += 9 + header.length;
        }
        if (offset > 0) {           /* 已消费部分前移，保留半帧 */
            memmove(acc, acc + offset, have - offset);
            have -= offset;
        }
    }
done:
    free(acc);
    NL_HTTP_MUTEX_DESTROY(&conn.mutex);
    h2_connection_cleanup(&conn);
}

/* HTTP/1.1 over TLS 会话（keep-alive 语义同核心库 http1_client_thread） */

/* 单个请求上限：防御恶意超大 Content-Length 把内存吃光 */
#define NL_HTTPS_MAX_REQUEST (64u * 1024u * 1024u)

static long nlh_tls_recv_cb(void* ctx, char* buf, size_t len) {
    return (long)NL_HTTPS_TLS_recv((NL_HTTPS_TLS_CTX*)ctx, buf, len);
}

static int nlh_tls_write_cb(void* ctx, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = NL_HTTPS_TLS_send((NL_HTTPS_TLS_CTX*)ctx, data + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void nlh_tls_h1_session(NL_HTTPS_TLS_CTX* tls,
                               nl_http_handler handler, void* user_data) {
    int keep_alive = 1;
    while (keep_alive) {
        size_t total = 0;
        /* 读满整个请求：头部 + Content-Length 指定的 body。
         * 只读到头部终止符会让带体请求（注册/登录/上传）拿不到 body。 */
        char* buffer = nl_http_read_request_full(nlh_tls_recv_cb, tls,
                                                 nlh_tls_write_cb, tls,
                                                 BUFFER_SIZE, NL_HTTPS_MAX_REQUEST,
                                                 &total);
        if (!buffer || total == 0) { free(buffer); break; }

        /* 请求体超上限（64MiB）：回 413 关连接，不把截断数据当完整请求（v3.1.0） */
        if (!nl_http_request_body_complete(buffer, total)) {
            static const char r413[] = "HTTP/1.1 413 Request Entity Too Large\r\n"
                                       "Connection: close\r\nContent-Length: 0\r\n\r\n";
            nlh_tls_write_cb(tls, r413, sizeof(r413) - 1);
            free(buffer);
            break;
        }

        /* req/resp 头值已动态分配（结构体约 9KB）；保留堆分配（v3.1.0） */
        nl_http_request_t* req = (nl_http_request_t*)malloc(sizeof(*req));
        nl_http_response_t* resp = (nl_http_response_t*)malloc(sizeof(*resp));
        if (!req || !resp) { free(req); free(resp); free(buffer); break; }
        memset(req, 0, sizeof(*req));
        memset(resp, 0, sizeof(*resp));
        resp->status = 200;
        int parsed = 0;

        int parse_rc = nl_http_parse_request(req, buffer, total);
        if (parse_rc == 0) {
            parsed = 1;
            const char* conn_hdr = nl_http_request_get_header(req, "Connection");
            if (conn_hdr && strcasecmp(conn_hdr, "close") == 0) keep_alive = 0;
            if (req->version == NL_HTTP_VERSION_1_0) {
                const char* ka = nl_http_request_get_header(req, "Keep-Alive");
                if (!ka) keep_alive = 0;
            }
            if (handler) {
                handler(req, resp, user_data);
            } else {
                nl_http_response_set_body(resp,
                    "<html><body><h1>Hello, NetLeaf HTTPS/1.1!</h1></body></html>", 58);
            }
        } else if (parse_rc == -2) {
            /* 头字段超限：回 431 关连接，不再静默截断（v3.1.0） */
            keep_alive = 0;
            resp->status = 431;
            nl_http_response_set_header(resp, "Connection", "close");
            nl_http_response_set_body(resp, "", 0);
        } else {
            keep_alive = 0;
        }

        char* response_data = NULL;
        size_t response_len = 0;
        if (keep_alive && parsed)
            nl_http_response_set_header(resp, "Connection", "keep-alive");
        nl_http_generate_response_http1(resp, &response_data, &response_len);
        if (response_data && response_len > 0)
            nlh_tls_write_cb(tls, response_data, response_len); /* 循环部分写：mbedTLS 单次约 16KB 上限，只调一次会截断大响应 */
        free(response_data);
        /* 文件响应：头部已带 Content-Length，正文在此分块加密发出
         * （TLS 无法使用内核 TransmitFile 零拷贝） */
        if (resp->file_path) {
            nl_http_stream_file_chunks(resp->file_path, resp->file_size, nlh_tls_write_cb, tls);
            free(resp->file_path);
            resp->file_path = NULL;
        }
        if (resp->body) free(resp->body);
        nl_http_headers_free(resp->headers, resp->header_count);
        if (req->body) free(req->body);
        nl_http_headers_free(req->headers, req->header_count);
        free(req);
        free(resp);
        free(buffer);
    }
}

typedef struct {
    SOCKET sock;
    nlh_tls_server_t* srv;
} nlh_tls_conn_args_t;

static DWORD WINAPI nlh_tls_conn_thread(LPVOID arg) {
    nlh_tls_conn_args_t* args = (nlh_tls_conn_args_t*)arg;
    SOCKET sock = args->sock;
    nlh_tls_server_t* srv = args->srv;
    free(args);

    NL_HTTPS_TLS_CTX* tls = NL_HTTPS_TLS_create();
    if (!tls) { closesocket(sock); return 0; }

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

    if (NL_HTTPS_TLS_configure(tls, &cfg) != NL_HTTPS_TLS_OK ||
        NL_HTTPS_TLS_handshake_ex(tls, NL_HTTPS_TO_INT(sock), 10000) != NL_HTTPS_TLS_OK) {
        NL_HTTPS_TLS_destroy(tls);
        closesocket(sock);
        return 0;
    }

    const char* proto = NL_HTTPS_TLS_get_alpn(tls);
    if (proto && strcmp(proto, "h2") == 0) {
        nlh_tls_h2_session(tls, sock, srv->handler, srv->user_data);
    } else {
        nlh_tls_h1_session(tls, srv->handler, srv->user_data);
    }

    NL_HTTPS_TLS_destroy(tls);
    closesocket(sock);
    return 0;
}

static DWORD WINAPI nlh_tls_server_thread(LPVOID arg) {
    nlh_tls_server_t* srv = (nlh_tls_server_t*)arg;

    while (srv->running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        SOCKET client_sock = accept(srv->fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock != INVALID_SOCKET) {
            /* H2 需要：新连接禁用 Nagle，否则小帧/响应头会被 TCP 缓冲拖慢 */
            int _nd = 1;
            setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&_nd, sizeof(_nd));
            nlh_tls_conn_args_t* args = malloc(sizeof(nlh_tls_conn_args_t));
            if (!args) { closesocket(client_sock); continue; }
            args->sock = client_sock;
            args->srv = srv;
            HANDLE th = CreateThread(NULL, 0, nlh_tls_conn_thread, args, 0, NULL);
            if (th) {
                CloseHandle(th);
            } else {
                free(args);
                closesocket(client_sock);
            }
        }
    }
    return 0;
}

nlh_tls_server_t* nlh_tls_server_create(int port, const nl_http_tls_server_cfg_t* cfg) {
    if (!NL_HTTPS_TLS_is_available()) return NULL;
    if (!cfg || !cfg->cert_file || !cfg->key_file) return NULL;

    nlh_tls_server_t* srv = calloc(1, sizeof(nlh_tls_server_t));
    if (!srv) return NULL;

    srv->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv->fd == INVALID_SOCKET) { free(srv); return NULL; }

    BOOL opt = TRUE;
    setsockopt(srv->fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(srv->fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(srv->fd); free(srv); return NULL;
    }
    if (listen(srv->fd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(srv->fd); free(srv); return NULL;
    }

    srv->port = port;
    srv->enable_http2 = 1;
    srv->cert_file = _strdup(cfg->cert_file);
    srv->key_file  = _strdup(cfg->key_file);
    srv->key_pass  = cfg->key_pass ? _strdup(cfg->key_pass) : NULL;
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
    srv->thread = CreateThread(NULL, 0, nlh_tls_server_thread, srv, 0, NULL);
    if (!srv->thread) {
        srv->running = 0;
        return -1;
    }
    return 0;
}

void nlh_tls_server_stop(nlh_tls_server_t* srv) {
    if (!srv || !srv->running) return;
    srv->running = 0;
    NL_HTTPS_CLOSOBJ(srv->fd);
    srv->fd = INVALID_SOCKET;
    WaitForSingleObject(srv->thread, INFINITE);
    CloseHandle(srv->thread);
    srv->thread = NULL;
}

void nlh_tls_server_destroy(nlh_tls_server_t* srv) {
    if (!srv) return;
    if (srv->running) nlh_tls_server_stop(srv);
    if (srv->fd != INVALID_SOCKET) closesocket(srv->fd);
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
