#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/*
 * Windows 平台 HTTP/1/2 适配层（纯 HTTP，无 TLS）。
 *
 * 设计：纯逻辑函数（解析/hpack/h2 帧/访问器）已抽到平台无关的
 * src/http/netleaf_http_common.c，本文件只保留平台相关部分：
 *   - 平台句柄/线程/锁宏（NL_HTTPS_SOCK / NL_HTTP_THREAD / NL_HTTP_MUTEX / 锁钩子）
 *   - I/O 函数表（nl_http_io_t：Winsock TCP 写 h2 帧）
 *   - nl_http_server（H1 生命周期）+ 平台线程 + 生命周期
 *
 * HTTPS/TLS 专属代码已拆到 src/http/netleaf_http_tls_windows.c，
 * 由独立 CMake 目标 netleaf_https 编译并链接 netleaf_tls3/tls2。
 * 本文件定义 nl_http_server 结构（仅 HTTP 字段，无 TLS 字段）；
 * netleaf_https 库中同结构带 TLS 字段的定义由 _tls 文件提供。
 * 原 H3 / QUIC 代码已随 QUIC/HTTP3 功能一并迁出主库（见 src/quic 扩展）。
 */

/* 平台句柄/线程/锁宏：须在本头 include 前定义，供公共结构体使用。
 * Winsock SOCKET 为 uint32_t，TLS API 用 int 传句柄，经宏做双向转换。 */
#define NL_HTTPS_SOCK     SOCKET
#define NL_HTTPS_CLOSOBJ(s) closesocket(s)
#define NL_HTTPS_INVALID  INVALID_SOCKET
#define NL_HTTPS_SOCK_VALID(s) ((s) != INVALID_SOCKET)
#define NL_HTTPS_TO_INT(s)     ((int)(s))
#define NL_HTTPS_FROM_INT(i)   ((SOCKET)(i))
#define NL_HTTP_THREAD    HANDLE
#define NL_HTTP_MUTEX     CRITICAL_SECTION
#define NL_HTTP_MUTEX_INIT(m)    InitializeCriticalSection(m)
#define NL_HTTP_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#define NL_HTTP_MUTEX_LOCK(m)    EnterCriticalSection(m)
#define NL_HTTP_MUTEX_UNLOCK(m)  LeaveCriticalSection(m)

/* 内部库源码构建：调用原长名实现体不产生 deprecated 告警 */
#define NL_HTTP_INTERNAL_BUILD
#include "netleaf_http.h"
#include "netleaf_optimize.h"
#include "http/netleaf_http_internal.h"

/* 仅本文件内可见（public 头只用 opaque 句柄） */
struct nl_http_server {
    NL_HTTPS_SOCK fd;
    int port;
    int running;
    int enable_http2;
    NL_HTTP_THREAD thread;
    nl_http_handler handler;
    void* user_data;
};

/* 客户端句柄（HTTP-only 构建中不使用 TLS，保留占位结构） */
struct nl_http_client {
    int unused;
};

/* ============================================================
 * I/O 函数表：TCP 写 h2 帧（Winsock）
 * ============================================================ */
static int io_write_h2_frame(struct nl_http_io* io, nl_http_handle_t handle,
                             const uint8_t* data, size_t len) {
    (void)io;
    int written = send(handle, (const char*)data, (int)len, 0);
    return (written < 0) ? -1 : written;
}

static struct nl_http_io nl_http_io = {
    .write_h2_frame = io_write_h2_frame,
};

/* ============================================================
 * H1 / H2 客户端线程（Win32 DWORD WINAPI）
 * ============================================================ */
typedef struct {
    SOCKET client_fd;
    nl_http_handler handler;
    void* user_data;
} http2_client_args_t;

static DWORD WINAPI http2_client_thread(LPVOID arg) {
    http2_client_args_t* args = (http2_client_args_t*)arg;
    SOCKET client_fd = args->client_fd;
    nl_http_handler handler = args->handler;
    void* user_data = args->user_data;
    free(args);

    struct nl_http2_connection conn;
    h2_connection_init(&conn, client_fd);
    NL_HTTP_MUTEX_INIT(&conn.mutex);

    char buffer[BUFFER_SIZE];
    int bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);

    if (bytes_read >= H2_PREFACE_LEN && memcmp(buffer, h2_preface, H2_PREFACE_LEN) == 0) {
        conn.preface_sent = 1;
        h2_send_settings(&nl_http_io, client_fd, &conn);

        size_t remaining = bytes_read - H2_PREFACE_LEN;
        if (remaining > 0) {
            h2_process_frame(&nl_http_io, client_fd, &conn,
                             (uint8_t*)buffer + H2_PREFACE_LEN, remaining,
                             handler, user_data);
        }

        while (1) {
            bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
            if (bytes_read <= 0) break;

            size_t offset = 0;
            while (offset < (size_t)bytes_read) {
                if (offset + 9 > (size_t)bytes_read) break;

                struct nl_h2_frame_header header;
                h2_frame_header_read((uint8_t*)buffer + offset, &header);
                if (offset + 9 + header.length > (size_t)bytes_read) break;

                h2_process_frame(&nl_http_io, client_fd, &conn,
                                 (uint8_t*)buffer + offset, 9 + header.length,
                                 handler, user_data);
                offset += 9 + header.length;
            }
        }
    }

    NL_HTTP_MUTEX_DESTROY(&conn.mutex);
    h2_connection_cleanup(&conn);
    closesocket(client_fd);
    return 0;
}

typedef struct {
    SOCKET client_fd;
    nl_http_server_t* server;
} http1_client_args_t;

/* 读取单个完整 HTTP/1 请求到 buf（容量 cap）。成功返回实际读取字节数，
 * 连接关闭/出错返回 <=0。配合 nl_http_find_terminator 做增量解析。
 * （原为 http1_client_thread 内的 GNU 嵌套函数，-Wpedantic 报
 * "ISO C forbids nested functions"，提升为文件级 static。） */
static long nl_http_read_request_win(SOCKET fd, char* buf, size_t cap, size_t* total) {
    *total = 0;
    while (*total < cap - 1) {
        int n = recv(fd, buf + *total, (int)(cap - 1 - *total), 0);
        if (n <= 0) return (long)*total;
        *total += (size_t)n;
        buf[*total] = '\0';
        if (nl_http_find_terminator(buf, *total) != -1) return (long)*total;
    }
    return (long)*total;
}

static DWORD WINAPI http1_client_thread(LPVOID arg) {
    http1_client_args_t* args = (http1_client_args_t*)arg;
    SOCKET client_fd = args->client_fd;
    nl_http_server_t* server = args->server;
    free(args);

    /* 取长补短：旧路径（核心库 nl_handle_client）支持 keep-alive 连接复用，
     * 新路径原先"读完即 close"。此处改为 Connection 感知：默认 keep-alive，
     * 请求头显式指定 close 或为 HTTP/1.0 无 keep-alive 时关闭。 */
    int keep_alive = 1;
    while (keep_alive) {
        char buffer[BUFFER_SIZE];
        size_t total = 0;
        long got = nl_http_read_request_win(client_fd, buffer, sizeof(buffer), &total);
        if (got <= 0 || total == 0) break;

        nl_http_request_t req;
        nl_http_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = 200;
        int parsed = 0;

        if (nl_http_parse_request(&req, buffer, total) == 0) {
            parsed = 1;
            const char* conn = nl_http_request_get_header(&req, "Connection");
            if (conn && strcasecmp(conn, "close") == 0) {
                keep_alive = 0;
            }
            if (req.version == NL_HTTP_VERSION_1_0) {
                const char* ka = nl_http_request_get_header(&req, "Keep-Alive");
                if (!ka) keep_alive = 0;
            }

            if (server && server->handler) {
                server->handler(&req, &resp, server->user_data);
            } else {
                nl_http_response_set_body(&resp, "<html><body><h1>Hello, NetLeaf HTTP/1.1!</h1></body></html>", 53);
            }
        }

        char* response_data = NULL;
        size_t response_len = 0;
        if (keep_alive && parsed) nl_http_response_set_header(&resp, "Connection", "keep-alive");
        nl_http_generate_response_http1(&resp, &response_data, &response_len);

        if (response_data) {
            if (response_len > 0) send(client_fd, response_data, (int)response_len, 0);
            free(response_data);
        }

        if (req.body) free(req.body);
    }

    closesocket(client_fd);
    return 0;
}

/* ============================================================
 * H1 / H2 服务端线程 + 生命周期（Win32）
 * ============================================================ */
static DWORD WINAPI server_thread(LPVOID arg) {
    nl_http_server_t* server = (nl_http_server_t*)arg;

    while (server->running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);

        SOCKET client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd != INVALID_SOCKET) {
            if (server->enable_http2) {
                http2_client_args_t* args = malloc(sizeof(http2_client_args_t));
                args->client_fd = client_fd;
                args->handler = server->handler;
                args->user_data = server->user_data;
                HANDLE thread = CreateThread(NULL, 0, http2_client_thread, args, 0, NULL);
                if (thread) CloseHandle(thread);
            } else {
                http1_client_args_t* args = malloc(sizeof(http1_client_args_t));
                args->client_fd = client_fd;
                args->server = server;
                HANDLE thread = CreateThread(NULL, 0, http1_client_thread, args, 0, NULL);
                if (thread) CloseHandle(thread);
            }
        }
    }

    return 0;
}

static DWORD WINAPI http2_server_thread(LPVOID arg) {
    nl_http2_server_t* server = (nl_http2_server_t*)arg;

    while (server->running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);

        SOCKET client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd != INVALID_SOCKET) {
            http2_client_args_t* args = malloc(sizeof(http2_client_args_t));
            args->client_fd = client_fd;
            args->handler = server->handler;
            args->user_data = server->user_data;

            HANDLE thread = CreateThread(NULL, 0, http2_client_thread, args, 0, NULL);
            if (thread) CloseHandle(thread);
        }
    }

    return 0;
}

nl_http_server_t* nl_http_server_create(int port) {
    nl_http_server_t* server = calloc(1, sizeof(nl_http_server_t));
    if (!server) return NULL;

    server->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->fd == INVALID_SOCKET) {
        free(server);
        return NULL;
    }

    BOOL opt = TRUE;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(server->fd);
        free(server);
        return NULL;
    }

    if (listen(server->fd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server->fd);
        free(server);
        return NULL;
    }

    server->port = port;
    return server;
}

void nl_http_server_destroy(nl_http_server_t* server) {
    if (!server) return;
    if (NL_HTTPS_SOCK_VALID(server->fd)) NL_HTTPS_CLOSOBJ(server->fd);
    free(server);
}

int nl_http_server_start(nl_http_server_t* server) {
    if (!server || server->running) return -1;
    server->running = 1;
    server->thread = CreateThread(NULL, 0, server_thread, server, 0, NULL);
    return server->thread ? 0 : -1;
}

void nl_http_server_stop(nl_http_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    NL_HTTPS_CLOSOBJ(server->fd);
    if (server->thread) {
        WaitForSingleObject(server->thread, INFINITE);
        CloseHandle(server->thread);
    }
}

void nl_http_server_set_handler(nl_http_server_t* server, nl_http_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}

void nl_http_server_enable_http2(nl_http_server_t* server, int enable) {
    if (server) server->enable_http2 = enable;
}

nl_http2_server_t* nl_http2_server_create(int port) {
    nl_http2_server_t* server = calloc(1, sizeof(nl_http2_server_t));
    if (!server) return NULL;

    server->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->fd == INVALID_SOCKET) {
        free(server);
        return NULL;
    }

    BOOL opt = TRUE;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(server->fd);
        free(server);
        return NULL;
    }

    if (listen(server->fd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server->fd);
        free(server);
        return NULL;
    }

    server->port = port;
    return server;
}

void nl_http2_server_destroy(nl_http2_server_t* server) {
    if (!server) return;
    if (NL_HTTPS_SOCK_VALID(server->fd)) NL_HTTPS_CLOSOBJ(server->fd);
    free(server);
}

int nl_http2_server_start(nl_http2_server_t* server) {
    if (!server || server->running) return -1;
    server->running = 1;
    server->thread = CreateThread(NULL, 0, http2_server_thread, server, 0, NULL);
    return server->thread ? 0 : -1;
}

void nl_http2_server_stop(nl_http2_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    NL_HTTPS_CLOSOBJ(server->fd);
    if (server->thread) {
        WaitForSingleObject(server->thread, INFINITE);
        CloseHandle(server->thread);
    }
}

void nl_http2_server_set_handler(nl_http2_server_t* server, nl_http_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}

/* ============================================================
 * HTTPS（TLS）已拆到 src/http/netleaf_http_tls_windows.c，由独立
 * CMake 目标 netleaf_https 编译并链接 netleaf_tls3/tls2。
 * 本文件（netleaf_http 库）不再包含 TLS API。
 *
 * 原 H3 / QUIC 服务端已随 QUIC/HTTP3 代码一并迁出主库（见 src/quic 扩展）。
 * ============================================================ */
