#define _CRT_RAND_S
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

/*
 * Windows 平台 HTTP/1/2/3 适配层（纯 HTTP，无 TLS）。
 *
 * 设计：纯逻辑函数（解析/hpack/h2 帧/h3/quic-varint/访问器）已抽到平台无关的
 * src/http/netleaf_http_common.c，本文件只保留平台相关部分：
 *   - 平台句柄/线程/锁宏（NL_HTTPS_SOCK / NL_HTTP_THREAD / NL_HTTP_MUTEX / 锁钩子）
 *   - I/O 函数表（nl_http_io_t：Winsock TCP 写 h2 帧、UDP 发 QUIC 报文）
 *   - nl_http_server（H1 生命周期）+ 平台线程 + 生命周期
 *   - QUIC 连接 ID RNG（BCryptGenRandom/rand_s 回退）
 *
 * HTTPS/TLS 专属代码已拆到 src/http/netleaf_http_tls_windows.c，
 * 由独立 CMake 目标 netleaf_https 编译并链接 netleaf_tls3/tls2。
 * 本文件定义 nl_http_server 结构（仅 HTTP 字段，无 TLS 字段）；
 * netleaf_https 库中同结构带 TLS 字段的定义由 _tls 文件提供。
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
    int enable_http3;
    NL_HTTP_THREAD thread;
    nl_http_handler handler;
    void* user_data;
};

/* 客户端句柄（HTTP-only 构建中不使用 TLS，保留占位结构） */
struct nl_http_client {
    int unused;
};

/* ============================================================
 * I/O 函数表：TCP 写 h2 帧 / UDP 发 QUIC 报文（Winsock）
 * ============================================================ */
static int io_write_h2_frame(struct nl_http_io* io, nl_http_handle_t handle,
                             const uint8_t* data, size_t len) {
    (void)io;
    int written = send(handle, (const char*)data, (int)len, 0);
    return (written < 0) ? -1 : written;
}

static int io_udp_send(struct nl_http_io* io, nl_http_handle_t handle,
                       const uint8_t* data, size_t len,
                       void* client_addr, size_t client_addr_len) {
    (void)io;
    int sent = sendto(handle, (const char*)data, (int)len, 0,
                      (struct sockaddr*)client_addr, (int)client_addr_len);
    return (sent < 0) ? -1 : 0;
}

static struct nl_http_io nl_http_io = {
    .write_h2_frame = io_write_h2_frame,
    .udp_send       = io_udp_send,
};

/* QUIC 连接 ID RNG：BCryptGenRandom，失败回退 rand_s */
void quic_generate_conn_id(nl_quic_conn_id_t* conn_id, uint8_t len) {
    if (!conn_id) return;
    if (len > QUIC_MAX_CONN_ID_LEN) len = QUIC_MAX_CONN_ID_LEN;
    conn_id->len = len;

    NTSTATUS status = BCryptGenRandom(NULL, conn_id->data, len,
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        for (uint8_t i = 0; i < len; i++) {
            unsigned int rv;
            rand_s(&rv);
            conn_id->data[i] = (uint8_t)(rv & 0xFF);
        }
    }
}

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
    closesocket(client_fd);
    return 0;
}

typedef struct {
    SOCKET client_fd;
    nl_http_server_t* server;
} http1_client_args_t;

static DWORD WINAPI http1_client_thread(LPVOID arg) {
    http1_client_args_t* args = (http1_client_args_t*)arg;
    SOCKET client_fd = args->client_fd;
    nl_http_server_t* server = args->server;
    free(args);

    char buffer[BUFFER_SIZE];
    int has_more = 1;
    size_t total = 0;
    while (has_more && total < sizeof(buffer) - 1) {
        int bytes_read = recv(client_fd, buffer + total,
                              (int)(sizeof(buffer) - 1 - total), 0);
        if (bytes_read <= 0) break;
        total += bytes_read;
        has_more = (nl_http_find_terminator(buffer, total) == -1);
    }
    buffer[total] = '\0';

    if (total > 0) {
        nl_http_request_t req;
        nl_http_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = 200;

        if (nl_http_parse_request(&req, buffer, total) == 0) {
            if (server && server->handler) {
                server->handler(&req, &resp, server->user_data);
            } else {
                nl_http_response_set_body(&resp, "<html><body><h1>Hello, NetLeaf HTTP/1.1!</h1></body></html>", 53);
            }
        }

        char* response_data = NULL;
        size_t response_len = 0;
        nl_http_generate_response_http1(&resp, &response_data, &response_len);

        if (response_data) {
            send(client_fd, response_data, (int)response_len, 0);
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

void nl_http_server_enable_http3(nl_http_server_t* server, int enable) {
    if (server) server->enable_http3 = enable;
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
 * ============================================================ */

/* ============================================================
 * H3 / QUIC 服务端
 * ============================================================ */

/* 平台补齐：local_params / send_buffer / 锁（公共 h3_connection_init 只
 * 负责 memset + client_addr 拷贝 + hpack + 链表挂载，此处补齐平台部分）。 */
static void h3_connection_lock_init(struct nl_http3_connection* conn) {
    conn->local_params.initial_max_data = QUIC_INITIAL_MAX_DATA;
    conn->local_params.initial_max_stream_data_bidi_local = QUIC_INITIAL_MAX_STREAM_DATA;
    conn->local_params.initial_max_stream_data_bidi_remote = QUIC_INITIAL_MAX_STREAM_DATA;
    conn->local_params.initial_max_stream_data_uni = QUIC_INITIAL_MAX_STREAM_DATA;
    conn->local_params.initial_max_streams_bidi = QUIC_DEFAULT_MAX_STREAMS_BIDI;
    conn->local_params.initial_max_streams_uni = QUIC_DEFAULT_MAX_STREAMS_UNI;
    conn->local_params.max_idle_timeout = QUIC_DEFAULT_MAX_IDLE_TIMEOUT;
    conn->local_params.ack_delay_exponent = 3;
    conn->local_params.max_ack_delay = 25;
    conn->local_params.active_connection_id_limit = 8;
    uint8_t* sb = malloc(H3_BUFFER_SIZE);
    if (sb) {
        conn->send_buffer = sb;
        conn->send_buffer_size = H3_BUFFER_SIZE;
        conn->send_buffer_len = 0;
    }
    NL_HTTP_MUTEX_INIT(&conn->mutex);
}

static DWORD WINAPI http3_server_thread(LPVOID arg) {
    nl_http3_server_t* server = (nl_http3_server_t*)arg;

    uint8_t buffer[H3_BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int client_len = sizeof(client_addr);

    while (server->running) {
        int bytes_read = recvfrom(server->fd, (char*)buffer, sizeof(buffer), 0,
                                  (struct sockaddr*)&client_addr, &client_len);

        if (bytes_read > 0) {
            /* 匹配现有连接（平台 sockaddr 比较） */
            struct nl_http3_connection* conn = NULL;
            for (struct nl_http3_connection* curr = server->connections; curr; curr = curr->next) {
                struct sockaddr_in* ca = (struct sockaddr_in*)curr->client_addr;
                if (ca && ca->sin_addr.s_addr == client_addr.sin_addr.s_addr &&
                    ca->sin_port == client_addr.sin_port) {
                    conn = curr;
                    break;
                }
            }

            if (!conn) {
                conn = calloc(1, sizeof(struct nl_http3_connection));
                if (conn) {
                    h3_connection_init(conn, server->fd, &client_addr,
                                       (size_t)client_len, server);
                    h3_connection_lock_init(conn);
                }
            }

            if (conn) {
                NL_HTTP_MUTEX_LOCK(&conn->mutex);
                h3_process_quic_packet(&nl_http_io, server, conn, buffer,
                                       (size_t)bytes_read,
                                       server->handler, server->user_data);
                NL_HTTP_MUTEX_UNLOCK(&conn->mutex);
            }
        }
    }

    /* 清理连接 */
    struct nl_http3_connection* c = server->connections;
    while (c) {
        struct nl_http3_connection* next = c->next;
        NL_HTTP_MUTEX_DESTROY(&c->mutex);
        h3_connection_free(c);
        c = next;
    }

    return 0;
}

nl_http3_server_t* nl_http3_server_create(int port) {
    nl_http3_server_t* server = calloc(1, sizeof(nl_http3_server_t));
    if (!server) return NULL;

    server->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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

    server->port = port;

    /* 默认传输参数 */
    server->default_params.initial_max_data = QUIC_INITIAL_MAX_DATA;
    server->default_params.initial_max_stream_data_bidi_local = QUIC_INITIAL_MAX_STREAM_DATA;
    server->default_params.initial_max_stream_data_bidi_remote = QUIC_INITIAL_MAX_STREAM_DATA;
    server->default_params.initial_max_stream_data_uni = QUIC_INITIAL_MAX_STREAM_DATA;
    server->default_params.initial_max_streams_bidi = QUIC_DEFAULT_MAX_STREAMS_BIDI;
    server->default_params.initial_max_streams_uni = QUIC_DEFAULT_MAX_STREAMS_UNI;
    server->default_params.max_idle_timeout = QUIC_DEFAULT_MAX_IDLE_TIMEOUT;
    server->default_params.ack_delay_exponent = 3;
    server->default_params.max_ack_delay = 25;
    server->default_params.active_connection_id_limit = 8;

    quic_generate_conn_id(&server->server_conn_id, 8);
    return server;
}

void nl_http3_server_destroy(nl_http3_server_t* server) {
    if (!server) return;
    if (NL_HTTPS_SOCK_VALID(server->fd)) NL_HTTPS_CLOSOBJ(server->fd);
    free(server);
}

int nl_http3_server_start(nl_http3_server_t* server) {
    if (!server || server->running) return -1;
    server->running = 1;
    server->thread = CreateThread(NULL, 0, http3_server_thread, server, 0, NULL);
    return server->thread ? 0 : -1;
}

void nl_http3_server_stop(nl_http3_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    NL_HTTPS_CLOSOBJ(server->fd);
    if (server->thread) {
        WaitForSingleObject(server->thread, INFINITE);
        CloseHandle(server->thread);
    }
}

void nl_http3_server_set_handler(nl_http3_server_t* server, nl_http_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}
