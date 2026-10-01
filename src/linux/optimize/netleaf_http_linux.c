#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/random.h>

/*
 * Linux 平台 HTTP/1/2/3 适配层（纯 HTTP，无 TLS）。
 *
 * 设计：纯逻辑函数已抽到平台无关的 src/http/netleaf_http_common.c，
 * 本文件只保留平台相关部分：
 *  - 平台句柄/线程/锁宏
 *  - I/O 函数表（nl_http_io_t：TCP 写 h2 帧、UDP 发 QUIC 报文）
 *  - nl_http_server（H1 生命周期）+ 平台线程 + 生命周期
 *  - QUIC 连接 ID RNG（getrandom/urandom/rand）
 *
 * HTTPS/TLS 专属代码已拆到 src/http/netleaf_http_tls_linux.c，
 * 由独立 CMake 目标 netleaf_https 编译并链接 netleaf_tls3/tls2。
 */

/* 平台句柄/线程/锁宏：须在本头 include 前定义，供公共结构体使用 */
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
#include "netleaf_optimize.h"
#include "http/netleaf_http_internal.h"

#include <netdb.h>
#include <sys/types.h>
#ifndef __ANDROID__
#include <sys/random.h>
#endif

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
 * I/O 函数表：TCP 写 h2 帧 / UDP 发 QUIC 报文
 * ============================================================ */
static int io_write_h2_frame(struct nl_http_io* io, nl_http_handle_t handle,
                             const uint8_t* data, size_t len) {
    ssize_t written = write(handle, data, len);
    return (written < 0) ? -1 : (int)written;
}

static int io_udp_send(struct nl_http_io* io, nl_http_handle_t handle,
                       const uint8_t* data, size_t len,
                       void* client_addr, size_t client_addr_len) {
    ssize_t sent = sendto(handle, data, len, 0,
                          (struct sockaddr*)client_addr, client_addr_len);
    return (sent < 0) ? -1 : 0;
}

static struct nl_http_io nl_http_io = {
    .write_h2_frame = io_write_h2_frame,
    .udp_send       = io_udp_send,
};

/* ============================================================
 * QUIC 连接 ID RNG（平台相关：getrandom/urandom/rand）
 * ============================================================ */
void quic_generate_conn_id(nl_quic_conn_id_t* conn_id, uint8_t len) {
    if (!conn_id) return;
    if (len > QUIC_MAX_CONN_ID_LEN) len = QUIC_MAX_CONN_ID_LEN;
    conn_id->len = len;

#ifdef __ANDROID__
    /* Bionic/NDK 不提供 getrandom()（需 API 23+ 的 syscall），直接读 /dev/urandom */
    FILE* fp = fopen("/dev/urandom", "rb");
    if (fp) {
        size_t read = fread(conn_id->data, 1, len, fp);
        fclose(fp);
        if (read < len) {
            for (uint8_t i = (uint8_t)read; i < len; i++)
                conn_id->data[i] = (uint8_t)(rand() & 0xFF);
        }
    } else {
        for (uint8_t i = 0; i < len; i++)
            conn_id->data[i] = (uint8_t)(rand() & 0xFF);
    }
    return;
#else
    ssize_t bytes_read = getrandom(conn_id->data, len, 0);

    /* getrandom 失败时回退 /dev/urandom */
    if (bytes_read < 0 || (size_t)bytes_read < len) {
        FILE* fp = fopen("/dev/urandom", "rb");
        if (fp) {
            size_t read = fread(conn_id->data, 1, len, fp);
            fclose(fp);
            if (read < len) {
                for (uint8_t i = (uint8_t)read; i < len; i++)
                    conn_id->data[i] = (uint8_t)(rand() & 0xFF);
            }
        } else {
            for (uint8_t i = 0; i < len; i++)
                conn_id->data[i] = (uint8_t)(rand() & 0xFF);
        }
    }
#endif
}

/* 线程参数 */
typedef struct {
    int client_fd;
    nl_http_handler handler;
    void* user_data;
} http2_client_args_t;

typedef struct {
    int client_fd;
    nl_http_server_t* server;
} http1_client_args_t;

/* ============================================================
 * 平台线程
 * ============================================================ */
static void* http2_client_thread(void* arg) {
    http2_client_args_t* args = (http2_client_args_t*)arg;
    int client_fd = args->client_fd;
    nl_http_handler handler = args->handler;
    void* user_data = args->user_data;
    free(args);

    struct nl_http2_connection conn;
    h2_connection_init(&conn, client_fd);
    NL_HTTP_MUTEX_INIT(&conn.mutex);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer));

    if (bytes_read >= H2_PREFACE_LEN && memcmp(buffer, h2_preface, H2_PREFACE_LEN) == 0) {
        conn.preface_sent = 1;
        h2_send_settings(&nl_http_io, client_fd, &conn);

        size_t remaining = bytes_read - H2_PREFACE_LEN;
        if (remaining > 0) {
            h2_process_frame(&nl_http_io, client_fd, &conn, (uint8_t*)buffer + H2_PREFACE_LEN, remaining, handler, user_data);
        }

        while (1) {
            bytes_read = read(client_fd, buffer, sizeof(buffer));
            if (bytes_read <= 0) break;

            size_t offset = 0;
            while (offset < (size_t)bytes_read) {
                if (offset + 9 > (size_t)bytes_read) break;

                struct nl_h2_frame_header header;
                h2_frame_header_read((uint8_t*)buffer + offset, &header);

                if (offset + 9 + header.length > (size_t)bytes_read) break;

                h2_process_frame(&nl_http_io, client_fd, &conn, (uint8_t*)buffer + offset, 9 + header.length, handler, user_data);
                offset += 9 + header.length;
            }
        }
    }

    NL_HTTP_MUTEX_DESTROY(&conn.mutex);
    close(client_fd);
    return NULL;
}

static void* http1_client_thread(void* arg) {
    http1_client_args_t* args = (http1_client_args_t*)arg;
    int client_fd = args->client_fd;
    nl_http_server_t* server = args->server;
    free(args);

    char buffer[BUFFER_SIZE];
    int has_more = 1;
    size_t total = 0;
    while (has_more && total < sizeof(buffer) - 1) {
        ssize_t bytes_read = read(client_fd, buffer + total, sizeof(buffer) - 1 - total);
        if (bytes_read <= 0) break;
        total += bytes_read;
        /* 收到完整请求头即可停止 */
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
            write(client_fd, response_data, response_len);
            free(response_data);
        }

        if (req.body) free(req.body);
    }

    close(client_fd);
    return NULL;
}

static void* server_thread(void* arg) {
    nl_http_server_t* server = (nl_http_server_t*)arg;

    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd >= 0) {
            if (server->enable_http2) {
                http2_client_args_t* args = malloc(sizeof(http2_client_args_t));
                args->client_fd = client_fd;
                args->handler = server->handler;
                args->user_data = server->user_data;
                pthread_t thread;
                pthread_create(&thread, NULL, http2_client_thread, args);
                pthread_detach(thread);
            } else {
                http1_client_args_t* args = malloc(sizeof(http1_client_args_t));
                args->client_fd = client_fd;
                args->server = server;
                pthread_t thread;
                pthread_create(&thread, NULL, http1_client_thread, args);
                pthread_detach(thread);
            }
        }
    }

    return NULL;
}

static void* http2_server_thread(void* arg) {
    nl_http2_server_t* server = (nl_http2_server_t*)arg;

    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd >= 0) {
            http2_client_args_t* args = malloc(sizeof(http2_client_args_t));
            args->client_fd = client_fd;
            args->handler = server->handler;
            args->user_data = server->user_data;

            pthread_t thread;
            pthread_create(&thread, NULL, http2_client_thread, args);
            pthread_detach(thread);
        }
    }

    return NULL;
}

/* ============================================================
 * H1 生命周期
 * ============================================================ */
nl_http_server_t* nl_http_server_create(int port) {
    nl_http_server_t* server = calloc(1, sizeof(nl_http_server_t));
    if (!server) return NULL;

    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->fd < 0) {
        free(server);
        return NULL;
    }

    int opt = 1;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }

    if (listen(server->fd, SOMAXCONN) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }

    server->port = port;
    return server;
}

void nl_http_server_destroy(nl_http_server_t* server) {
    if (!server) return;
    if (server->fd >= 0) close(server->fd);
    free(server);
}

int nl_http_server_start(nl_http_server_t* server) {
    if (!server || server->running) return -1;
    server->running = 1;
    return pthread_create(&server->thread, NULL, server_thread, server);
}

void nl_http_server_stop(nl_http_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    close(server->fd);
    pthread_join(server->thread, NULL);
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

/* ============================================================
 * H2 生命周期
 * ============================================================ */
nl_http2_server_t* nl_http2_server_create(int port) {
    nl_http2_server_t* server = calloc(1, sizeof(nl_http2_server_t));
    if (!server) return NULL;

    server->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->fd < 0) {
        free(server);
        return NULL;
    }

    int opt = 1;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }

    if (listen(server->fd, SOMAXCONN) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }

    server->port = port;
    return server;
}

void nl_http2_server_destroy(nl_http2_server_t* server) {
    if (!server) return;
    if (server->fd >= 0) close(server->fd);
    free(server);
}

int nl_http2_server_start(nl_http2_server_t* server) {
    if (!server || server->running) return -1;
    server->running = 1;
    return pthread_create(&server->thread, NULL, http2_server_thread, server);
}

void nl_http2_server_stop(nl_http2_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    close(server->fd);
    pthread_join(server->thread, NULL);
}

void nl_http2_server_set_handler(nl_http2_server_t* server, nl_http_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}

/* ============================================================
 * HTTPS（TLS）已拆到 src/http/netleaf_http_tls_linux.c，由独立
 * CMake 目标 netleaf_https 编译并链接 netleaf_tls3/tls2。
 * 本文件（netleaf_http 库）不再包含 TLS API。
 * ============================================================ */

/* ============================================================
 * H3/QUIC 服务端线程
 *
 * 说明：本线程负责收 UDP、匹配/新建连接、调用公共层 h3_process_quic_packet。
 * 连接匹配沿用 sockaddr_in 比较；新建连接的默认值 + hpack + RNG + 锁初始化
 * 由公共 h3_connection_init + NL_HTTP_MUTEX_INIT 完成。
 * ============================================================ */
static void h3_connection_lock_init(struct nl_http3_connection* conn) {
    /* local_params / send_buffer / 链表挂载由公共 h3_connection_init 处理 */
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

static void* http3_server_thread(void* arg) {
    nl_http3_server_t* server = (nl_http3_server_t*)arg;

    uint8_t buffer[H3_BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (server->running) {
        ssize_t bytes_read = recvfrom(server->fd, buffer, sizeof(buffer), 0,
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
                    h3_connection_init(conn, server->fd, &client_addr, client_len, server);
                    h3_connection_lock_init(conn);
                }
            }

            if (conn) {
                NL_HTTP_MUTEX_LOCK(&conn->mutex);
                h3_process_quic_packet(&nl_http_io, server, conn, buffer, bytes_read,
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

    return NULL;
}

/* ============================================================
 * H3 生命周期
 * ============================================================ */
nl_http3_server_t* nl_http3_server_create(int port) {
    nl_http3_server_t* server = calloc(1, sizeof(nl_http3_server_t));
    if (!server) return NULL;

    server->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->fd < 0) {
        free(server);
        return NULL;
    }

    int opt = 1;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->fd);
        free(server);
        return NULL;
    }

    server->port = port;

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
    if (server->fd >= 0) close(server->fd);
    free(server);
}

int nl_http3_server_start(nl_http3_server_t* server) {
    if (!server || server->running) return -1;
    server->running = 1;
    return pthread_create(&server->thread, NULL, http3_server_thread, server);
}

void nl_http3_server_stop(nl_http3_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    close(server->fd);
    pthread_join(server->thread, NULL);
}

void nl_http3_server_set_handler(nl_http3_server_t* server, nl_http_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}
