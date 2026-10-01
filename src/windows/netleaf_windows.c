#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "../include/netleaf.h"

#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")
#endif

#define BUFFER_SIZE 8192
#define MAX_CLIENTS 65535
#define DEFAULT_CONCURRENCY 4
/* 高压场景：Web 服务（含反向代理）worker 池默认值，受 MAX_CONCURRENCY 约束。 */
#define WEB_WORKER_DEFAULT 16
#define MAX_CONCURRENCY 64
#define MAX_WORKER_QUEUE 1024

struct nl_server {
    SOCKET fd;
    SOCKET accept_socket;
    HANDLE iocp_handle;
    nl_protocol_t protocol;
    int port;
    nl_request_handler handler;
    nl_udp_message_handler udp_handler;
    nl_udp_message_handler_v2 udp_handler_v2;
    void* user_data;
    volatile long running;
    HANDLE thread_handle;
    OVERLAPPED accept_overlapped;
    char accept_buffer[sizeof(SOCKADDR_IN) * 2 + 32];
    int concurrency;
    int num_workers;
    HANDLE* workers;
    int* queue;
    int queue_head;
    int queue_tail;
    int queue_count;
    int queue_max;
    CRITICAL_SECTION queue_lock;
    HANDLE queue_not_empty;
    HANDLE queue_not_full;
    int queue_init;
    int pool_used;
    // ---- UDP IOCP OVERLAPPED 上下文（IOCP 驱动 UDP 读，彻底摆脱裸轮询） ----
    OVERLAPPED udp_ov;
    WSABUF     udp_wsabuf;
    char       udp_buf[BUFFER_SIZE];
    // WSARecvFrom 需要的地址缓冲：size = SOCKADDR + 预留
    char       udp_addrbuf[sizeof(SOCKADDR_IN) + 32];
    // 标记 listener_thread 是否需要对 UDP socket 做 IOCP WSARecvFrom 预 post
    int        udp_iocp_active;
};

struct nl_client {
    SOCKET fd;
    nl_protocol_t protocol;
    struct sockaddr_in addr;
    int connected;
    char buffer[BUFFER_SIZE];
    size_t buffer_len;
    WSAOVERLAPPED recv_overlapped;
    WSABUF wsabuf;
};

struct nl_config {
    char data[4096];
    int count;
    CRITICAL_SECTION mutex;
};

struct nl_buffer {
    char* data;
    size_t capacity;
    size_t length;
    CRITICAL_SECTION mutex;
};

struct nl_event_loop {
    HANDLE iocp_handle;
    volatile long running;
    HANDLE thread_handle;
};

static nl_log_level_t current_log_level = NL_LOG_INFO;
static nl_log_callback log_callback = NULL;
static void* log_user_data = NULL;
static int debug_mode = 0;

static void windows_log(nl_log_level_t level, const char* fmt, ...) {
    if (level < current_log_level) return;
    
    va_list args;
    va_start(args, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    
    if (log_callback) {
        log_callback(level, msg, log_user_data);
    } else {
        const char* prefix[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(stderr, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] %s\n",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond,
                prefix[level], msg);
    }
}

// Debug mode API implementation
void nl_debug_enable(int enable) {
    debug_mode = enable;
    if (enable) {
        current_log_level = NL_LOG_DEBUG;
        windows_log(NL_LOG_INFO, "Debug mode enabled");
    } else {
        windows_log(NL_LOG_INFO, "Debug mode disabled");
    }
}

int nl_debug_is_enabled(void) {
    return debug_mode;
}

void nl_log(nl_log_level_t level, const char* format, ...) {
    if (level < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    windows_log(level, "%s", msg);
}

void nl_log_debug(const char* format, ...) {
    if (NL_LOG_DEBUG < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    windows_log(NL_LOG_DEBUG, "%s", msg);
}

void nl_log_info(const char* format, ...) {
    if (NL_LOG_INFO < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    windows_log(NL_LOG_INFO, "%s", msg);
}

void nl_log_warn(const char* format, ...) {
    if (NL_LOG_WARN < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    windows_log(NL_LOG_WARN, "%s", msg);
}

void nl_log_error(const char* format, ...) {
    if (NL_LOG_ERROR < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    windows_log(NL_LOG_ERROR, "%s", msg);
}

static int init_winsock(void) {
    static int initialized = 0;
    static WSADATA wsa_data;
    
    if (!initialized) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            return -1;
        }
        initialized = 1;
    }
    return 0;
}

static int set_reuseaddr(SOCKET fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
}

static int set_reuseport(SOCKET fd) {
    (void)fd;
    int opt;
    (void)opt;
    // SO_REUSEPORT is not available on older Windows versions
#ifdef SO_REUSEPORT
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char*)&opt, sizeof(opt));
#else
    return 0;
#endif
}

static int set_tcp_nodelay(SOCKET fd, int enable) {
    int opt = enable ? 1 : 0;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
}

static int set_tcp_keepalive(SOCKET fd, int enable, int idle, int interval, int count) {
    (void)idle; (void)interval; (void)count;
    int opt = enable ? 1 : 0;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&opt, sizeof(opt)) == SOCKET_ERROR) {
        return -1;
    }
    // For Windows, further keep-alive customization would require TCP_KEEPIDLE, etc.
    return 0;
}

static int set_buffer_sizes(SOCKET fd, int sndbuf, int rcvbuf) {
    if (sndbuf > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));
    }
    if (rcvbuf > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));
    }
    return 0;
}

static int set_broadcast(SOCKET fd, int enable) {
    int opt = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, (const char*)&opt, sizeof(opt));
}

static int set_blocking(SOCKET fd) {
    DWORD mode = 0;
    int ret = ioctlsocket(fd, FIONBIO, &mode);
    return (ret == 0) ? 0 : -1;
}

static void parse_http_request(const char* data, size_t len,
                               char* path, size_t path_size,
                               nl_http_method_t* method,
                               const char** body, size_t* body_size) {
    if (!data || len == 0) return;

    const char* header_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i+1] == '\n' && data[i+2] == '\r' && data[i+3] == '\n') {
            header_end = data + i;
            break;
        }
        if (data[i] == '\n' && data[i+1] == '\n') {
            header_end = data + i;
            break;
        }
    }
    if (!header_end) header_end = data + len;

    size_t header_len = (size_t)(header_end - data);
    char* line = (char*)malloc(header_len + 1);
    if (!line) return;
    memcpy(line, data, header_len);
    line[header_len] = '\0';

    char* space1 = strchr(line, ' ');
    char* space2 = space1 ? strchr(space1 + 1, ' ') : NULL;

    if (space1 && space2) {
        *space1 = '\0';
        *space2 = '\0';

        if (strncmp(line, "GET", 3) == 0) *method = NL_METHOD_GET;
        else if (strncmp(line, "POST", 4) == 0) *method = NL_METHOD_POST;
        else if (strncmp(line, "PUT", 3) == 0) *method = NL_METHOD_PUT;
        else if (strncmp(line, "DELETE", 6) == 0) *method = NL_METHOD_DELETE;
        else *method = NL_METHOD_GET;

        strncpy(path, space1 + 1, path_size - 1);
        path[path_size - 1] = '\0';

        size_t body_start = (size_t)(header_end - data);
        if (data[body_start] == '\r') body_start += 2; else body_start += 1;
        if (data[body_start] == '\r') body_start += 2;
        if (body_start < len) {
            *body_size = len - body_start;
            *body = data + body_start;
        } else {
            *body_size = 0;
            *body = NULL;
        }
    } else {
        *method = NL_METHOD_GET;
        *body_size = 0;
        *body = NULL;
    }

    free(line);
}

static DWORD WINAPI worker_dispatch(LPVOID lpParam) {
    nl_server_t* server = (nl_server_t*)lpParam;

    while (server->running == 1) {
        SOCKET fd = INVALID_SOCKET;
        EnterCriticalSection(&server->queue_lock);
        while (server->queue_count == 0 && server->running == 1) {
            LeaveCriticalSection(&server->queue_lock);
            WaitForSingleObject(server->queue_not_empty, 100);
            EnterCriticalSection(&server->queue_lock);
        }
        if (server->running != 1) {
            LeaveCriticalSection(&server->queue_lock);
            break;
        }
        fd = (SOCKET)server->queue[server->queue_head];
        server->queue_head = (server->queue_head + 1) % server->queue_max;
        server->queue_count--;
        SetEvent(server->queue_not_full);
        LeaveCriticalSection(&server->queue_lock);

        nl_buffer_t* req = nl_buffer_create(BUFFER_SIZE);
        if (!req) {
            closesocket(fd);
            continue;
        }

        char buf[BUFFER_SIZE];
        int idle_ms = 30000;
        while (server->running == 1) {
            int have_data = 0;
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(fd, &read_set);
            struct timeval tv;
            tv.tv_sec = idle_ms / 1000;
            tv.tv_usec = (idle_ms % 1000) * 1000;
            int pr = select(0, &read_set, NULL, NULL, &tv);
            if (pr == -1) {
                if (WSAGetLastError() == WSAEINTR) continue;
                break;
            }
            if (pr == 0) {
                have_data = (nl_buffer_size(req) > 0);
            } else {
                have_data = 1;
            }

            if (have_data) {
                int n = (int)recv(fd, buf, BUFFER_SIZE, 0);
                if (n > 0) {
                    nl_buffer_write(req, buf, (size_t)n);
                } else if (n == 0) {
                    break;
                } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
                    break;
                }
            }

            if (nl_buffer_size(req) > 0 && server->handler) {
                char path[1024] = {0};
                nl_http_method_t method = NL_METHOD_GET;
                const char* body = NULL;
                size_t body_size = 0;

                parse_http_request(req->data, nl_buffer_size(req),
                                   path, sizeof(path), &method, &body, &body_size);

                char* response = NULL;
                size_t response_len = 0;
                server->handler(path, method, body, body_size, &response, &response_len, server->user_data);

                // BUG-002: 契约要求 *response 必须堆分配（malloc/calloc/realloc）。
                // 服务端在此处 free(response)：若 handler 传字符串字面量/静态数组
                // 会触发 free(): invalid pointer。无响应时 handler 须置 *response = NULL。
                if (response && response_len > 0) {
                    size_t total = 0;
                    while (total < response_len) {
                        int w = (int)send(fd, response + total, (int)(response_len - total), 0);
                        if (w <= 0) break;
                        total += (size_t)w;
                    }
                    free(response);
                }
                nl_buffer_clear(req);
            }

            if (!have_data || pr == 0) break;
        }

        closesocket(fd);
        nl_buffer_destroy(req);
    }

    return 0;
}

static DWORD WINAPI listener_thread(LPVOID lpParam) {
    nl_server_t* server = (nl_server_t*)lpParam;

    if (server->protocol == NL_PROTO_UDP) {
        // ---- UDP 专属 IOCP 读循环（替代旧版裸轮询 + Sleep(10)） ----
        // 前置：nl_server_create 已 CreateIoCompletionPort 把 UDP fd 挂进
        // server->iocp_handle（completion key = (ULONG_PTR)server->fd）。
        //
        // 策略：有 handler → 预 post WSARecvFrom OVERLAPPED，GetQueuedCompletionStatus
        //        驱动；无 handler → 不 post，GetQueuedCompletionStatus 等 timeout
        //        自然睡眠，零 CPU。
        //
        // BUG-ARCH-003 仍然保留：GetQueuedCompletionStatus 收到一次完成就 dispatch，
        // 然后立刻重新 post 下一个 WSARecvFrom。由于 OVERLAPPED 完成是"一次一完成"
        // 语义（WSARecvFrom 收一个 datagram 就完成），不存在裸 recvfrom 的"非阻塞
        // 积压多 datagram 排空"问题。
        WSABUF wb;
        wb.buf = server->udp_buf;
        wb.len = BUFFER_SIZE;
        DWORD flags = 0;
        int addr_len = sizeof(server->udp_addrbuf);

        // 首次：有 handler 才预 post WSARecvFrom
        if (server->udp_handler || server->udp_handler_v2) {
            memset(&server->udp_ov, 0, sizeof(server->udp_ov));
            DWORD wsa_flags = 0;
            DWORD dummy_bytes = 0;
            int rc = WSARecvFrom(server->fd, &wb, 1, &dummy_bytes, &wsa_flags,
                                 (struct sockaddr*)&server->udp_addrbuf, &addr_len,
                                 &server->udp_ov, NULL);
            // WSA_IO_PENDING = 正常异步投递；0 = 立即完成（少见）
            if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                windows_log(NL_LOG_ERROR, "Windows UDP IOCP: 首次 WSARecvFrom 失败 gle=%d", WSAGetLastError());
            } else {
                server->udp_iocp_active = 1;
            }
        }

        while (server->running == 1) {
            DWORD bytes_xfer = 0;
            ULONG_PTR completion_key = 0;
            OVERLAPPED* pov = NULL;

            BOOL ok = GetQueuedCompletionStatus(server->iocp_handle, &bytes_xfer,
                                                &completion_key, &pov, 200);
            DWORD gle = ok ? 0 : GetLastError();

            if (!ok) {
                if (gle == WAIT_TIMEOUT) continue;     // 正常 idle 睡眠
                if (gle == ERROR_OPERATION_ABORTED) continue; // server 关闭期间取消
                windows_log(NL_LOG_ERROR, "Windows UDP IOCP: GetQueuedCompletionStatus 失败 gle=%lu", gle);
                break;
            }

            // completion key 应该就是 server->fd（nl_server_create 里挂进 iocp 时设的）
            if ((SOCKET)completion_key != server->fd) continue;

            // WSARecvFrom OVERLAPPED 完成
            if (pov == &server->udp_ov && server->udp_iocp_active) {
                if (bytes_xfer > 0) {
                    // BUG-201：peer 地址从 WSARecvFrom 的 addrbuf 解出来
                    char peer_addr[INET6_ADDRSTRLEN] = "0.0.0.0";
                    int  peer_port = 0;
                    struct sockaddr_in* sin = (struct sockaddr_in*)&server->udp_addrbuf;
                    if (sin->sin_family == AF_INET) {
                        inet_ntop(AF_INET, &sin->sin_addr, peer_addr, sizeof(peer_addr));
                        peer_port = (int)ntohs(sin->sin_port);
                    }
                    if (server->udp_handler_v2) {
                        server->udp_handler_v2(server->udp_buf, (size_t)bytes_xfer,
                                               peer_addr, peer_port, server->user_data);
                    } else {
                        server->udp_handler(server->udp_buf, (size_t)bytes_xfer, server->user_data);
                    }
                } else {
                    // bytes_xfer == 0 + gle==ERROR_OPERATION_ABORTED 常见于 server 关闭
                    // 非 abort 的 0 长度 datagram 理论上合法但罕见
                }

                // 重新 post 下一个 WSARecvFrom（只要还在 running）
                if (server->running == 1 && (server->udp_handler || server->udp_handler_v2)) {
                    memset(&server->udp_ov, 0, sizeof(server->udp_ov));
                    wb.buf = server->udp_buf;
                    wb.len = BUFFER_SIZE;
                    DWORD wsa_flags2 = 0;
                    DWORD dummy_bytes2 = 0;
                    addr_len = sizeof(server->udp_addrbuf);
                    int rc = WSARecvFrom(server->fd, &wb, 1, &dummy_bytes2, &wsa_flags2,
                                         (struct sockaddr*)&server->udp_addrbuf, &addr_len,
                                         &server->udp_ov, NULL);
                    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                        windows_log(NL_LOG_ERROR, "Windows UDP IOCP: 重 post WSARecvFrom 失败 gle=%d", WSAGetLastError());
                        server->udp_iocp_active = 0;
                    }
                } else if (!server->udp_handler && !server->udp_handler_v2) {
                    // handler 被动态清空 → 停止 post，iocp Wait 自然睡眠
                    server->udp_iocp_active = 0;
                }
            }
        }
        return 0;
    }

    // ---- TCP / HTTP / WEBSOCKET 路径（原有 worker pool + legacy 单线程不变） ----
    while (server->running == 1) {
        if (server->pool_used) {
            while (server->running == 1) {
                SOCKET client_fd = accept(server->fd, NULL, NULL);
                if (client_fd == INVALID_SOCKET) {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK || err == WSAECONNABORTED || err == WSAEINTR) {
                        break;
                    }
                    if (err != WSAEINTR) {
                        windows_log(NL_LOG_ERROR, "Windows: accept() failed: %d", err);
                    }
                    Sleep(1);
                    continue;
                }
                set_blocking(client_fd);
                EnterCriticalSection(&server->queue_lock);
                while (server->queue_count == server->queue_max && server->running == 1) {
                    LeaveCriticalSection(&server->queue_lock);
                    WaitForSingleObject(server->queue_not_full, 100);
                    EnterCriticalSection(&server->queue_lock);
                }
                if (server->running != 1) {
                    LeaveCriticalSection(&server->queue_lock);
                    closesocket(client_fd);
                    break;
                }
                server->queue[server->queue_tail] = (int)client_fd;
                server->queue_tail = (server->queue_tail + 1) % server->queue_max;
                server->queue_count++;
                SetEvent(server->queue_not_empty);
                LeaveCriticalSection(&server->queue_lock);
            }
        } else {
            // Legacy single-thread path (accept and handle in place).
            SOCKET client_fd = accept(server->fd, NULL, NULL);
            if (client_fd != INVALID_SOCKET) {
                nl_buffer_t* req = nl_buffer_create(BUFFER_SIZE);
                char buf[BUFFER_SIZE];
                int n = (int)recv(client_fd, buf, BUFFER_SIZE, 0);
                if (n > 0) {
                    nl_buffer_write(req, buf, (size_t)n);
                    if (nl_buffer_size(req) > 0 && server->handler) {
                        char path[1024] = {0};
                        nl_http_method_t method = NL_METHOD_GET;
                        const char* body = NULL;
                        size_t body_size = 0;
                        parse_http_request(req->data, nl_buffer_size(req),
                                          path, sizeof(path), &method, &body, &body_size);
                        char* response = NULL;
                        size_t response_len = 0;
                        server->handler(path, method, body, body_size, &response, &response_len, server->user_data);
                        // BUG-002: *response 必须堆分配；服务端 free(response) 前不做字面量/静态缓冲校验。
                        if (response && response_len > 0) {
                            send(client_fd, response, (int)response_len, 0);
                            free(response);
                        }
                    }
                }
                closesocket(client_fd);
                nl_buffer_destroy(req);
            } else {
                Sleep(1);
            }
        }
    }

    return 0;
}

static void stop_worker_pool(nl_server_t* server) {
    if (!server->pool_used) return;
    EnterCriticalSection(&server->queue_lock);
    InterlockedExchange(&server->running, 0);
    for (int i = 0; i < server->queue_count; i++) {
        int fd = server->queue[server->queue_head];
        server->queue_head = (server->queue_head + 1) % server->queue_max;
        if (fd > 0) closesocket((SOCKET)fd);
    }
    server->queue_count = 0;
    LeaveCriticalSection(&server->queue_lock);
    SetEvent(server->queue_not_empty);
    SetEvent(server->queue_not_full);
    for (int i = 0; i < server->num_workers; i++) {
        if (server->workers[i]) {
            WaitForSingleObject(server->workers[i], INFINITE);
            CloseHandle(server->workers[i]);
        }
    }
    DeleteCriticalSection(&server->queue_lock);
    CloseHandle(server->queue_not_empty);
    CloseHandle(server->queue_not_full);
    free(server->queue);
    free(server->workers);
    server->queue = NULL;
    server->workers = NULL;
    server->queue_not_empty = NULL;
    server->queue_not_full = NULL;
    server->num_workers = 0;
    server->pool_used = 0;
}

static void cleanup_start_failure(nl_server_t* server) {
    stop_worker_pool(server);
}

nl_server_t* nl_server_create(nl_protocol_t protocol, int port) {
    if (init_winsock() != 0) return NULL;
    
    nl_server_t* server = calloc(1, sizeof(nl_server_t));
    if (!server) return NULL;
    
    server->protocol = protocol;
    server->port = port;
    server->concurrency = 0;
    server->num_workers = 0;
    server->pool_used = 0;
    
    if (protocol == NL_PROTO_TCP || protocol == NL_PROTO_HTTP || protocol == NL_PROTO_WEBSOCKET) {
        server->fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    } else {
        server->fd = socket(AF_INET, SOCK_DGRAM, 0);
        // P0: make UDP socket non-blocking so the listener thread's recvfrom
        // cannot block forever (matches Linux/macOS set_nonblocking).
        u_long mode = 1;
        ioctlsocket(server->fd, FIONBIO, &mode);
    }
    
    if (server->fd == INVALID_SOCKET) {
        free(server);
        return NULL;
    }
    
    server->iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!server->iocp_handle) {
        closesocket(server->fd);
        free(server);
        return NULL;
    }
    
    CreateIoCompletionPort((HANDLE)server->fd, server->iocp_handle, (ULONG_PTR)server->fd, 0);
    
    set_reuseaddr(server->fd);
    
    if (protocol == NL_PROTO_TCP || protocol == NL_PROTO_HTTP || protocol == NL_PROTO_WEBSOCKET) {
        set_tcp_nodelay(server->fd, 1);
        set_tcp_keepalive(server->fd, 1, 7200, 75, 9);
    }
    
    set_buffer_sizes(server->fd, 262144, 262144);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    
    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        CloseHandle(server->iocp_handle);
        closesocket(server->fd);
        free(server);
        return NULL;
    }
    
    windows_log(NL_LOG_INFO, "Windows: Server created on port %d (IOCP)", port);
    return server;
}

void nl_server_destroy(nl_server_t* server) {
    if (!server) return;
    if (server->pool_used) stop_worker_pool(server);
    if (server->thread_handle) {
        // Join listener thread BEFORE closing the listening socket:
        // the listener blocks on accept()/recvfrom() on server->fd, so
        // closing fd first makes the wait never terminate -> deadlock.
        WaitForSingleObject(server->thread_handle, INFINITE);
        CloseHandle(server->thread_handle);
        server->thread_handle = NULL;
    }
    if (server->fd != INVALID_SOCKET) closesocket(server->fd);
    if (server->iocp_handle) CloseHandle(server->iocp_handle);
    free(server);
}

int nl_server_start(nl_server_t* server) {
    if (!server) return NL_EINVAL;
    
    int pool_created = 0;
    
    if (server->protocol == NL_PROTO_TCP || server->protocol == NL_PROTO_HTTP || 
        server->protocol == NL_PROTO_WEBSOCKET) {
        if (listen(server->fd, SOMAXCONN) == SOCKET_ERROR) {
            windows_log(NL_LOG_ERROR, "Windows: listen() failed: %d", WSAGetLastError());
            return NL_ERROR;
        }
        
        int use_pool = (server->protocol != NL_PROTO_UDP) ? 1 : 0;
        int target = (server->concurrency >= 1 && server->concurrency <= MAX_CONCURRENCY)
                     ? server->concurrency : DEFAULT_CONCURRENCY;
        if (use_pool && !server->pool_used) {
            server->num_workers = target;
            server->queue_max = MAX_WORKER_QUEUE;
            server->queue_head = 0;
            server->queue_tail = 0;
            server->queue_count = 0;
            server->queue = calloc((size_t)server->queue_max, sizeof(int));
            server->workers = calloc((size_t)server->num_workers, sizeof(HANDLE));
            server->queue_not_empty = CreateEvent(NULL, FALSE, FALSE, NULL);
            server->queue_not_full = CreateEvent(NULL, FALSE, FALSE, NULL);
            InitializeCriticalSection(&server->queue_lock);
            if (!server->queue || !server->workers || !server->queue_not_empty || !server->queue_not_full) {
                DeleteCriticalSection(&server->queue_lock);
                if (server->queue_not_empty) CloseHandle(server->queue_not_empty);
                if (server->queue_not_full) CloseHandle(server->queue_not_full);
                free(server->queue);
                free(server->workers);
                server->queue = NULL;
                server->workers = NULL;
                server->queue_not_empty = NULL;
                server->queue_not_full = NULL;
                server->num_workers = 0;
                windows_log(NL_LOG_ERROR, "Windows: failed to allocate worker pool");
                return NL_ERROR;
            }
            server->pool_used = 1;
            pool_created = 1;
            InterlockedExchange(&server->running, 1);
            for (int i = 0; i < server->num_workers; i++) {
                server->workers[i] = CreateThread(NULL, 0, worker_dispatch, server, 0, NULL);
                if (!server->workers[i]) {
                    cleanup_start_failure(server);
                    windows_log(NL_LOG_ERROR, "Windows: failed to create worker thread %d", i);
                    return NL_ERROR;
                }
            }
        }
    }
    
    if (pool_created) {
        InterlockedExchange(&server->running, 1);
    }
    server->thread_handle = CreateThread(NULL, 0, listener_thread, server, 0, NULL);
    if (!server->thread_handle) {
        InterlockedExchange(&server->running, 0);
        if (pool_created) stop_worker_pool(server);
        windows_log(NL_LOG_ERROR, "Windows: failed to create listener thread");
        return NL_ERROR;
    }
    
    windows_log(NL_LOG_INFO, "Windows: Server started (IOCP), socket=%lu, workers=%d",
                (ULONG)server->fd, server->pool_used ? server->num_workers : 0);
    return NL_OK;
}

void nl_server_stop(nl_server_t* server) {
    if (!server) return;
    stop_worker_pool(server);
    
    if (server->thread_handle) {
        WaitForSingleObject(server->thread_handle, INFINITE);
        CloseHandle(server->thread_handle);
        server->thread_handle = NULL;
    }
    if (server->fd != INVALID_SOCKET) {
        closesocket(server->fd);
        server->fd = INVALID_SOCKET;
    }
    windows_log(NL_LOG_INFO, "Windows: Server stopped");
}

int nl_server_set_concurrency(nl_server_t* server, int threads) {
    if (!server) return NL_EINVAL;
    if (threads <= 0 || threads > MAX_CONCURRENCY) return NL_EINVAL;
    server->concurrency = threads;
    return NL_OK;
}

void nl_server_set_handler(nl_server_t* server, nl_request_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}

void nl_server_set_udp_handler(nl_server_t* server, nl_udp_message_handler handler, void* user_data) {
    if (!server) return;
    server->udp_handler = handler;
    server->user_data = user_data;
}

void nl_server_set_udp_handler_v2(nl_server_t* server, nl_udp_message_handler_v2 handler, void* user_data) {
    if (!server) return;
    server->udp_handler_v2 = handler;
    server->user_data = user_data;
}

int nl_server_set_option(nl_server_t* server, nl_socket_option_t option, int value) {
    if (!server || server->fd == INVALID_SOCKET) return NL_EINVAL;
    
    int result = 0;
    
    switch (option) {
        case NL_OPT_TCP_NODELAY:
            result = set_tcp_nodelay(server->fd, value);
            break;
        case NL_OPT_TCP_KEEPALIVE:
            result = set_tcp_keepalive(server->fd, value, 0, 0, 0);
            break;
        case NL_OPT_SO_SNDBUF:
            result = setsockopt(server->fd, SOL_SOCKET, SO_SNDBUF, (const char*)&value, sizeof(value));
            break;
        case NL_OPT_SO_RCVBUF:
            result = setsockopt(server->fd, SOL_SOCKET, SO_RCVBUF, (const char*)&value, sizeof(value));
            break;
        case NL_OPT_SO_REUSEADDR:
            result = set_reuseaddr(server->fd);
            break;
        case NL_OPT_SO_REUSEPORT:
            result = set_reuseport(server->fd);
            break;
        case NL_OPT_SO_BROADCAST:
            result = set_broadcast(server->fd, value);
            break;
        case NL_OPT_CONCURRENCY:
            result = (nl_server_set_concurrency(server, value) == NL_OK) ? 0 : -1;
            break;
        default:
            return NL_ENOTSUPPORTED;
    }
    
    return (result == 0) ? NL_OK : NL_ERROR;
}

int nl_server_get_option(nl_server_t* server, nl_socket_option_t option, int* value) {
    if (!server || server->fd == INVALID_SOCKET || !value) return NL_EINVAL;
    
    int opt = 0;
    int optlen = sizeof(opt);
    
    switch (option) {
        case NL_OPT_TCP_NODELAY:
            if (getsockopt(server->fd, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, &optlen) == SOCKET_ERROR) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_SO_SNDBUF:
            if (getsockopt(server->fd, SOL_SOCKET, SO_SNDBUF, (char*)&opt, &optlen) == SOCKET_ERROR) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_SO_RCVBUF:
            if (getsockopt(server->fd, SOL_SOCKET, SO_RCVBUF, (char*)&opt, &optlen) == SOCKET_ERROR) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_CONCURRENCY:
            opt = (server->concurrency >= 1 && server->concurrency <= MAX_CONCURRENCY)
                 ? server->concurrency : DEFAULT_CONCURRENCY;
            break;
        default:
            return NL_ENOTSUPPORTED;
    }
    
    *value = opt;
    return NL_OK;
}

int nl_server_get_fd(nl_server_t* server) {
    return (server && server->fd != INVALID_SOCKET) ? (int)server->fd : -1;
}

nl_client_t* nl_client_create(nl_protocol_t protocol) {
    if (init_winsock() != 0) return NULL;
    
    nl_client_t* client = calloc(1, sizeof(nl_client_t));
    if (!client) return NULL;
    
    client->protocol = protocol;
    
    if (protocol == NL_PROTO_TCP || protocol == NL_PROTO_HTTP || protocol == NL_PROTO_WEBSOCKET) {
        client->fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    } else {
        client->fd = socket(AF_INET, SOCK_DGRAM, 0);
    }
    
    if (client->fd == INVALID_SOCKET) {
        free(client);
        return NULL;
    }
    
    if (protocol == NL_PROTO_TCP || protocol == NL_PROTO_HTTP || protocol == NL_PROTO_WEBSOCKET) {
        set_tcp_nodelay(client->fd, 1);
    }
    
    set_buffer_sizes(client->fd, 262144, 262144);
    windows_log(NL_LOG_DEBUG, "Windows: Client created (socket=%lu)", (ULONG)client->fd);
    return client;
}

void nl_client_destroy(nl_client_t* client) {
    if (!client) return;
    if (client->fd != INVALID_SOCKET) closesocket(client->fd);
    free(client);
}

int nl_client_connect(nl_client_t* client, const char* host, int port) {
    if (!client || !host) return NL_EINVAL;
    
    memset(&client->addr, 0, sizeof(client->addr));
    client->addr.sin_family = AF_INET;
    client->addr.sin_port = htons((u_short)port);
    
    if (InetPton(AF_INET, host, &client->addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host);
        if (!he) return NL_ECONNECT;
        memcpy(&client->addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    if (connect(client->fd, (struct sockaddr*)&client->addr, sizeof(client->addr)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            windows_log(NL_LOG_ERROR, "Windows: connect() failed: %d", err);
            return NL_ECONNECT;
        }
    }
    
    client->connected = 1;
    windows_log(NL_LOG_INFO, "Windows: Client connected to %s:%d", host, port);
    return NL_OK;
}

void nl_client_disconnect(nl_client_t* client) {
    if (!client) return;
    if (client->fd != INVALID_SOCKET) {
        closesocket(client->fd);
        client->fd = INVALID_SOCKET;
    }
    client->connected = 0;
}

int nl_client_send(nl_client_t* client, const void* data, size_t len) {
    if (!client || !data) return NL_EINVAL;
    
    int sent = send(client->fd, (const char*)data, (int)len, 0);
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return NL_EAGAIN;
        return NL_ERROR;
    }
    
    return sent;
}

int nl_client_recv(nl_client_t* client, void* buf, size_t len) {
    if (!client || !buf) return NL_EINVAL;
    
    int received = recv(client->fd, (char*)buf, (int)len, 0);
    if (received == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return NL_EAGAIN;
        return NL_ERROR;
    }
    if (received == 0) return NL_ECLOSED;
    
    return received;
}

int nl_client_send_to(nl_client_t* client, const char* host, int port, const void* data, size_t len) {
    if (!client || !host || !data) return NL_EINVAL;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    
    if (InetPton(AF_INET, host, &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host);
        if (!he) return NL_ECONNECT;
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    int sent = sendto(client->fd, (const char*)data, (int)len, 0, 
                     (struct sockaddr*)&addr, sizeof(addr));
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return NL_EAGAIN;
        return NL_ERROR;
    }
    
    return sent;
}

int nl_client_recv_from(nl_client_t* client, void* buf, size_t len, char* from_addr, size_t addr_len, int* from_port) {
    if (!client || !buf) return NL_EINVAL;
    
    struct sockaddr_in addr;
    int addr_struct_len = sizeof(addr);
    
    int received = recvfrom(client->fd, (char*)buf, (int)len, 0, 
                           (struct sockaddr*)&addr, &addr_struct_len);
    if (received == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return NL_EAGAIN;
        return NL_ERROR;
    }
    
    if (from_addr) {
        InetNtop(AF_INET, &addr.sin_addr, from_addr, (int)addr_len);
    }
    if (from_port) {
        *from_port = ntohs(addr.sin_port);
    }
    
    return received;
}

int nl_client_set_option(nl_client_t* client, nl_socket_option_t option, int value) {
    if (!client || client->fd == INVALID_SOCKET) return NL_EINVAL;
    
    int result = 0;
    
    switch (option) {
        case NL_OPT_TCP_NODELAY:
            result = set_tcp_nodelay(client->fd, value);
            break;
        case NL_OPT_SO_SNDBUF:
            result = setsockopt(client->fd, SOL_SOCKET, SO_SNDBUF, (const char*)&value, sizeof(value));
            break;
        case NL_OPT_SO_RCVBUF:
            result = setsockopt(client->fd, SOL_SOCKET, SO_RCVBUF, (const char*)&value, sizeof(value));
            break;
        case NL_OPT_SO_REUSEADDR:
            result = set_reuseaddr(client->fd);
            break;
        case NL_OPT_SO_REUSEPORT:
            result = set_reuseport(client->fd);
            break;
        case NL_OPT_SO_BROADCAST:
            result = set_broadcast(client->fd, value);
            break;
        default:
            return NL_ENOTSUPPORTED;
    }
    
    return (result == 0) ? NL_OK : NL_ERROR;
}

int nl_client_get_option(nl_client_t* client, nl_socket_option_t option, int* value) {
    if (!client || client->fd == INVALID_SOCKET || !value) return NL_EINVAL;
    
    int opt = 0;
    int optlen = sizeof(opt);
    
    switch (option) {
        case NL_OPT_TCP_NODELAY:
            if (getsockopt(client->fd, IPPROTO_TCP, TCP_NODELAY, (char*)&opt, &optlen) == SOCKET_ERROR) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_SO_SNDBUF:
            if (getsockopt(client->fd, SOL_SOCKET, SO_SNDBUF, (char*)&opt, &optlen) == SOCKET_ERROR) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_SO_RCVBUF:
            if (getsockopt(client->fd, SOL_SOCKET, SO_RCVBUF, (char*)&opt, &optlen) == SOCKET_ERROR) {
                return NL_ERROR;
            }
            break;
        default:
            return NL_ENOTSUPPORTED;
    }
    
    *value = opt;
    return NL_OK;
}

int nl_client_get_fd(nl_client_t* client) {
    return (client && client->fd != INVALID_SOCKET) ? (int)client->fd : -1;
}

nl_config_t* nl_config_create(void) {
    nl_config_t* config = calloc(1, sizeof(nl_config_t));
    if (!config) return NULL;
    InitializeCriticalSection(&config->mutex);
    return config;
}

void nl_config_destroy(nl_config_t* config) {
    if (!config) return;
    DeleteCriticalSection(&config->mutex);
    free(config);
}

int nl_config_load(nl_config_t* config, const char* path) {
    if (!config || !path) return NL_EINVAL;
    
    FILE* fp = fopen(path, "r");
    if (!fp) return NL_ERROR;
    
    EnterCriticalSection(&config->mutex);
    config->count = 0;
    
    char line[256];
    while (fgets(line, sizeof(line), fp) && config->count < 100) {
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            // BUG-304: 源 key/value 截断到 63 字符（槽位 64 含终止符），
            // snprintf 的 N 精确等于目标容量，GCC 推导不会报截断
            char key[64];
            char val[64];
            int kn = snprintf(key, sizeof(key), "%s", line);
            int vn = snprintf(val, sizeof(val), "%s", eq + 1);
            (void)kn; (void)vn;  // 截断是预期行为（key/val 槽位固定 64）
            memcpy(config->data + config->count * 128, key, sizeof(key));
            memcpy(config->data + config->count * 128 + 64, val, sizeof(val));
            config->count++;
        }
    }
    
    LeaveCriticalSection(&config->mutex);
    fclose(fp);
    
    windows_log(NL_LOG_INFO, "Windows: Config loaded from %s", path);
    return NL_OK;
}

int nl_config_save(nl_config_t* config, const char* path) {
    if (!config || !path) return NL_EINVAL;
    
    FILE* fp = fopen(path, "w");
    if (!fp) return NL_ERROR;
    
    EnterCriticalSection(&config->mutex);
    
    for (int i = 0; i < config->count; i++) {
        fprintf(fp, "%s=%s\n", 
                config->data + i * 128,
                config->data + i * 128 + 64);
    }
    
    LeaveCriticalSection(&config->mutex);
    fclose(fp);
    
    return NL_OK;
}

const char* nl_config_get(nl_config_t* config, const char* key) {
    if (!config || !key) return NULL;
    
    EnterCriticalSection(&config->mutex);
    
    for (int i = 0; i < config->count; i++) {
        if (strcmp(config->data + i * 128, key) == 0) {
            LeaveCriticalSection(&config->mutex);
            return config->data + i * 128 + 64;
        }
    }
    
    LeaveCriticalSection(&config->mutex);
    return NULL;
}

void nl_config_set(nl_config_t* config, const char* key, const char* value) {
    if (!config || !key || !value) return;
    
    EnterCriticalSection(&config->mutex);
    
    for (int i = 0; i < config->count; i++) {
        if (strcmp(config->data + i * 128, key) == 0) {
            // 用 snprintf 保证 NUL 终止并清空槽位残留（旧 token/密码不会被带出）
            snprintf(config->data + i * 128 + 64, 64, "%s", value);
            LeaveCriticalSection(&config->mutex);
            return;
        }
    }
    
    if (config->count < 100) {
        snprintf(config->data + config->count * 128, 64, "%s", key);
        snprintf(config->data + config->count * 128 + 64, 64, "%s", value);
        config->count++;
    }
    
    LeaveCriticalSection(&config->mutex);
}

nl_buffer_t* nl_buffer_create(size_t capacity) {
    nl_buffer_t* buffer = calloc(1, sizeof(nl_buffer_t));
    if (!buffer) return NULL;
    
    buffer->data = malloc(capacity);
    if (!buffer->data) {
        free(buffer);
        return NULL;
    }
    
    buffer->capacity = capacity;
    InitializeCriticalSection(&buffer->mutex);
    return buffer;
}

void nl_buffer_destroy(nl_buffer_t* buffer) {
    if (!buffer) return;
    if (buffer->data) free(buffer->data);
    DeleteCriticalSection(&buffer->mutex);
    free(buffer);
}

void nl_buffer_clear(nl_buffer_t* buffer) {
    if (!buffer) return;
    EnterCriticalSection(&buffer->mutex);
    buffer->length = 0;
    LeaveCriticalSection(&buffer->mutex);
}

size_t nl_buffer_write(nl_buffer_t* buffer, const void* data, size_t len) {
    if (!buffer || !data) return 0;
    
    EnterCriticalSection(&buffer->mutex);
    
    if (buffer->length + len > buffer->capacity) {
        len = buffer->capacity - buffer->length;
    }
    
    if (len > 0) {
        memcpy(buffer->data + buffer->length, data, len);
        buffer->length += len;
    }
    
    LeaveCriticalSection(&buffer->mutex);
    return len;
}

size_t nl_buffer_read(nl_buffer_t* buffer, void* data, size_t len) {
    if (!buffer || !data) return 0;
    
    EnterCriticalSection(&buffer->mutex);
    
    if (len > buffer->length) {
        len = buffer->length;
    }
    
    if (len > 0) {
        memcpy(data, buffer->data, len);
        memmove(buffer->data, buffer->data + len, buffer->length - len);
        buffer->length -= len;
    }
    
    LeaveCriticalSection(&buffer->mutex);
    return len;
}

size_t nl_buffer_size(nl_buffer_t* buffer) {
    if (!buffer) return 0;
    EnterCriticalSection(&buffer->mutex);
    size_t size = buffer->length;
    LeaveCriticalSection(&buffer->mutex);
    return size;
}

void nl_log_set_level(nl_log_level_t level) {
    current_log_level = level;
}

void nl_log_set_callback(nl_log_callback callback, void* user_data) {
    log_callback = callback;
    log_user_data = user_data;
}

const char* nl_version_string(void) {
    return NETLEAF_VERSION;
}

int nl_version_major(void) {
    return NETLEAF_VERSION_MAJOR;
}

int nl_version_minor(void) {
    return NETLEAF_VERSION_MINOR;
}

int nl_version_patch(void) {
    return NETLEAF_VERSION_PATCH;
}

// =========================================
// Advanced Server API - File Server
// =========================================

struct nl_file_server {
    char directory[1024];
    char index_file[128];
    int port;
    SOCKET sock;
    HANDLE thread;
    volatile long running;
    int enable_easter_egg;
};

struct nl_route {
    char path[256];
    nl_http_method_t method;
    nl_http_handler_t handler;
    void* user_data;
    struct nl_route* next;
};

struct nl_router {
    struct nl_route* routes;
    char static_dir[1024];
    CRITICAL_SECTION mutex;
};

static void get_file_extension(const char* filename, char* ext, size_t ext_len) {
    const char* dot = strrchr(filename, '.');
    if (dot) {
        strncpy(ext, dot + 1, ext_len - 1);
        ext[ext_len - 1] = '\0';
        for (size_t i = 0; i < strlen(ext); i++) {
            ext[i] = tolower(ext[i]);
        }
    } else {
        ext[0] = '\0';
    }
}

static const char* get_mime_type(const char* ext) {
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) return "text/html";
    if (strcmp(ext, "css") == 0) return "text/css";
    if (strcmp(ext, "js") == 0) return "application/javascript";
    if (strcmp(ext, "json") == 0) return "application/json";
    if (strcmp(ext, "png") == 0) return "image/png";
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "gif") == 0) return "image/gif";
    if (strcmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcmp(ext, "ico") == 0) return "image/x-icon";
    if (strcmp(ext, "xml") == 0) return "application/xml";
    if (strcmp(ext, "txt") == 0) return "text/plain";
    if (strcmp(ext, "pdf") == 0) return "application/pdf";
    if (strcmp(ext, "zip") == 0) return "application/zip";
    return "application/octet-stream";
}

static char* read_file(const char* filepath, size_t* out_size) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (size < 0) {
        fclose(fp);
        if (out_size) *out_size = 0;
        return NULL;
    }
    
    char* content = (char*)malloc((size_t)size + 1);
    if (!content) {
        fclose(fp);
        if (out_size) *out_size = 0;
        return NULL;
    }
    
    size_t read = fread(content, 1, (size_t)size, fp);
    fclose(fp);
    content[read] = '\0';
    
    if (out_size) *out_size = read;
    return content;
}

static char* normalize_path(const char* base_dir, const char* req_path, char* result, size_t result_len) {
    char full_path[4096];
    
    if (req_path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", base_dir, req_path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, req_path);
    }
    
    // Clean up path separators
    for (size_t i = 0; i < strlen(full_path); i++) {
        if (full_path[i] == '\\') full_path[i] = '/';
    }
    
    strncpy(result, full_path, result_len);
    result[result_len - 1] = '\0';
    
    return result;
}

static int is_path_safe(const char* base_dir, const char* filepath) {
    char normalized_base[4096];
    char normalized_file[4096];
    
    _fullpath(normalized_base, base_dir, sizeof(normalized_base));
    _fullpath(normalized_file, filepath, sizeof(normalized_file));
    
    // 目录边界校验：前缀相同还需下一字符为路径结束或分隔符，避免 C:\base 误匹配 C:\baseXXX
    size_t base_len = strlen(normalized_base);
    if (strncmp(normalized_file, normalized_base, base_len) != 0) return 0;
    if (base_len > 0 && (normalized_base[base_len - 1] == '\\' || normalized_base[base_len - 1] == '/')) return 1;
    return normalized_file[base_len] == '\0' || normalized_file[base_len] == '\\' || normalized_file[base_len] == '/';
}

static int send_http_response(SOCKET client, const char* content_type, const char* content, size_t content_len) {
    char header[4096];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, content_len
    );
    
    send(client, header, header_len, 0);
    if (content && content_len > 0) {
        send(client, content, (int)content_len, 0);
    }
    
    return NL_OK;
}

static int send_http_error(SOCKET client, int status_code, const char* message) {
    char response[4096];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body><h1>%d %s</h1></body></html>",
        status_code, message,
        strlen(message) + 32,
        status_code, message
    );
    
    send(client, response, len, 0);
    return NL_OK;
}

static int send_418_response(SOCKET client) {
    const char* teapot_html = 
        "<html>\n"
        "<head>\n"
        "<title>418 I'm a teapot</title>\n"
        "<style>\n"
        "body { font-family: Arial, sans-serif; text-align: center; padding: 50px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); }\n"
        ".teapot { font-size: 100px; margin-bottom: 20px; }\n"
        ".container { background: white; border-radius: 16px; padding: 40px; box-shadow: 0 10px 40px rgba(0,0,0,0.2); }\n"
        "h1 { color: #8B4513; }\n"
        "p { color: #666; }\n"
        "</style>\n"
        "</head>\n"
        "<body>\n"
        "<div class=\"container\">\n"
        "<div class=\"teapot\">🫖</div>\n"
        "<h1>418 I'm a teapot</h1>\n"
        "<p>This server is a teapot, not a coffee machine!</p>\n"
        "<p>Happy April Fools' Day! ☕</p>\n"
        "</div>\n"
        "</body>\n"
        "</html>";
    
    char response[4096];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 418 I'm a teapot\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "X-Teapot: Yes, this is a teapot!\r\n"
        "\r\n"
        "%s",
        strlen(teapot_html),
        teapot_html);
    
    send(client, response, len, 0);
    return NL_OK;
}

static int is_april_fools_day(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return (st.wMonth == 4 && st.wDay == 1);
}

static DWORD WINAPI file_server_thread(LPVOID arg) {
    nl_file_server_t* server = (nl_file_server_t*)arg;
    
    while (server->running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        
        SOCKET client = accept(server->sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client == INVALID_SOCKET) {
            Sleep(10);
            continue;
        }
        
        char buffer[8192];
        int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        
        if (received <= 0) {
            closesocket(client);
            continue;
        }
        
        buffer[received] = '\0';
        
        char method[16], path[1024], protocol[32];
        if (sscanf(buffer, "%15s %1023s %31s", method, path, protocol) != 3) {
            send_http_error(client, 400, "Bad Request");
            closesocket(client);
            continue;
        }
        
        if (server->enable_easter_egg && is_april_fools_day()) {
            const char* tea_path = "/tea";
            if (strncmp(path, tea_path, strlen(tea_path)) == 0) {
                send_418_response(client);
                closesocket(client);
                continue;
            }
        }
        
        char filepath[4096];
        normalize_path(server->directory, path, filepath, sizeof(filepath));
        
        if (!is_path_safe(server->directory, filepath)) {
            send_http_error(client, 403, "Forbidden");
            closesocket(client);
            continue;
        }
        
        DWORD attrs = GetFileAttributesA(filepath);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // BUG-304: index_path 与 filepath 同容量（4096），拼接与回拷容量一致，
            // GCC 推导不会越界，消除 -Wformat-truncation
            char index_path[4096];
            snprintf(index_path, sizeof(index_path), "%s/%s",
                     filepath, server->index_file);
            if (GetFileAttributesA(index_path) != INVALID_FILE_ATTRIBUTES) {
                snprintf(filepath, sizeof(filepath), "%s", index_path);
            } else {
                send_http_error(client, 404, "Not Found");
                closesocket(client);
                continue;
            }
        }
        
        size_t file_size;
        char* file_content = read_file(filepath, &file_size);
        
        if (!file_content) {
            send_http_error(client, 404, "Not Found");
            closesocket(client);
            continue;
        }
        
        char ext[32];
        get_file_extension(filepath, ext, sizeof(ext));
        const char* mime_type = get_mime_type(ext);
        
        send_http_response(client, mime_type, file_content, file_size);
        
        free(file_content);
        closesocket(client);
    }
    
    return 0;
}

nl_file_server_t* nl_file_server_create(const char* directory, int port) {
    if (!directory) return NULL;
    
    nl_file_server_t* server = (nl_file_server_t*)calloc(1, sizeof(nl_file_server_t));
    if (!server) return NULL;
    
    char full_dir[1024];
    if (_fullpath(full_dir, directory, sizeof(full_dir))) {
        strncpy(server->directory, full_dir, sizeof(server->directory));
    } else {
        strncpy(server->directory, directory, sizeof(server->directory));
    }
    server->directory[sizeof(server->directory) - 1] = '\0';
    
    strncpy(server->index_file, "index.html", sizeof(server->index_file));
    server->port = port;
    server->running = 0;
    
    return server;
}

void nl_file_server_destroy(nl_file_server_t* server) {
    if (!server) return;
    
    if (server->running) {
        nl_file_server_stop(server);
    }
    
    free(server);
}

int nl_file_server_start(nl_file_server_t* server) {
    if (!server) return NL_EINVAL;
    if (server->running) return NL_OK;
    
    if (init_winsock() != 0) return NL_ERROR;
    
    server->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->sock == INVALID_SOCKET) {
        return NL_ERROR;
    }
    
    int opt = 1;
    setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)server->port);
    
    if (bind(server->sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(server->sock);
        return NL_ERROR;
    }
    
    if (listen(server->sock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server->sock);
        return NL_ERROR;
    }
    
    InterlockedExchange(&server->running, 1);
    server->thread = CreateThread(NULL, 0, file_server_thread, server, 0, NULL);
    if (!server->thread) {
        InterlockedExchange(&server->running, 0);
        closesocket(server->sock);
        return NL_ERROR;
    }
    
    windows_log(NL_LOG_INFO, "File server started on port %d, serving %s", server->port, server->directory);
    return NL_OK;
}

void nl_file_server_stop(nl_file_server_t* server) {
    if (!server || !server->running) return;
    
    InterlockedExchange(&server->running, 0);
    
    if (server->thread) {
        WaitForSingleObject(server->thread, INFINITE);
        CloseHandle(server->thread);
        server->thread = NULL;
    }
    
    if (server->sock != INVALID_SOCKET) {
        closesocket(server->sock);
        server->sock = INVALID_SOCKET;
    }
    
    windows_log(NL_LOG_INFO, "File server stopped");
}

void nl_file_server_set_index(nl_file_server_t* server, const char* index_file) {
    if (!server || !index_file) return;
    strncpy(server->index_file, index_file, sizeof(server->index_file) - 1);
    server->index_file[sizeof(server->index_file) - 1] = '\0';
}

void nl_file_server_set_easter_egg(nl_file_server_t* server, int enable) {
    if (!server) return;
    server->enable_easter_egg = enable;
}

static nl_file_server_t* g_simple_server = NULL;

int nl_serve_files(const char* directory, int port) {
    if (g_simple_server) {
        nl_file_server_stop(g_simple_server);
        nl_file_server_destroy(g_simple_server);
    }
    
    g_simple_server = nl_file_server_create(directory, port);
    if (!g_simple_server) return NL_ERROR;
    
    return nl_file_server_start(g_simple_server);
}

// =========================================
// Advanced Server API - Router
// =========================================

nl_router_t* nl_router_create(void) {
    nl_router_t* router = (nl_router_t*)calloc(1, sizeof(nl_router_t));
    if (!router) return NULL;
    
    InitializeCriticalSection(&router->mutex);
    return router;
}

void nl_router_destroy(nl_router_t* router) {
    if (!router) return;
    
    EnterCriticalSection(&router->mutex);
    struct nl_route* route = router->routes;
    while (route) {
        struct nl_route* next = route->next;
        free(route);
        route = next;
    }
    router->routes = NULL;
    LeaveCriticalSection(&router->mutex);
    
    DeleteCriticalSection(&router->mutex);
    free(router);
}

void nl_router_add_route(nl_router_t* router, const char* path, nl_http_method_t method, nl_http_handler_t handler, void* user_data) {
    if (!router || !path || !handler) return;
    
    struct nl_route* route = (struct nl_route*)calloc(1, sizeof(struct nl_route));
    if (!route) return;
    
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->path[sizeof(route->path) - 1] = '\0';
    route->method = method;
    route->handler = handler;
    route->user_data = user_data;
    
    EnterCriticalSection(&router->mutex);
    route->next = router->routes;
    router->routes = route;
    LeaveCriticalSection(&router->mutex);
}

void nl_router_set_static_dir(nl_router_t* router, const char* directory) {
    if (!router) return;
    
    EnterCriticalSection(&router->mutex);
    if (directory) {
        char full_dir[1024];
        if (_fullpath(full_dir, directory, sizeof(full_dir))) {
            strncpy(router->static_dir, full_dir, sizeof(router->static_dir));
        } else {
            strncpy(router->static_dir, directory, sizeof(router->static_dir));
        }
        router->static_dir[sizeof(router->static_dir) - 1] = '\0';
    } else {
        router->static_dir[0] = '\0';
    }
    LeaveCriticalSection(&router->mutex);
}

static struct nl_route* router_find_route(nl_router_t* router, const char* path, nl_http_method_t method) {
    EnterCriticalSection(&router->mutex);
    struct nl_route* route = router->routes;
    while (route) {
        if (route->method == method && strcmp(route->path, path) == 0) {
            LeaveCriticalSection(&router->mutex);
            return route;
        }
        route = route->next;
    }
    LeaveCriticalSection(&router->mutex);
    return NULL;
}

typedef struct {
    nl_router_t* router;
    int port;
    SOCKET sock;
    HANDLE thread;
    volatile long running;
} router_server_t;

static DWORD WINAPI router_server_thread(LPVOID arg) {
    router_server_t* rs = (router_server_t*)arg;
    
    while (rs->running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        
        SOCKET client = accept(rs->sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client == INVALID_SOCKET) {
            Sleep(10);
            continue;
        }
        
        char buffer[8192];
        int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        
        if (received <= 0) {
            closesocket(client);
            continue;
        }
        
        buffer[received] = '\0';
        
        char method_str[16], path[1024], protocol[32];
        if (sscanf(buffer, "%15s %1023s %31s", method_str, path, protocol) != 3) {
            send_http_error(client, 400, "Bad Request");
            closesocket(client);
            continue;
        }
        
        nl_http_method_t method = NL_METHOD_GET;
        if (strcmp(method_str, "POST") == 0) method = NL_METHOD_POST;
        else if (strcmp(method_str, "PUT") == 0) method = NL_METHOD_PUT;
        else if (strcmp(method_str, "DELETE") == 0) method = NL_METHOD_DELETE;
        else if (strcmp(method_str, "PATCH") == 0) method = NL_METHOD_PATCH;
        else if (strcmp(method_str, "HEAD") == 0) method = NL_METHOD_HEAD;
        else if (strcmp(method_str, "OPTIONS") == 0) method = NL_METHOD_OPTIONS;
        
        char* body_start = strstr(buffer, "\r\n\r\n");
        const char* body = NULL;
        size_t body_size = 0;
        
        if (body_start) {
            body = body_start + 4;
            body_size = (size_t)(received - (body - buffer));
        }
        
        struct nl_route* route = router_find_route(rs->router, path, method);
        if (route) {
            char* response = NULL;
            size_t response_size = 0;
            route->handler(path, method, body, body_size, &response, &response_size, route->user_data);
            
            // BUG-002: *response 必须堆分配（malloc/calloc/realloc）；
            // 字面量/静态缓冲传到这里 free() 会崩。无响应须置 *response = NULL。
            if (response) {
                send_http_response(client, "application/json", response, response_size);
                free(response);
            } else {
                send_http_error(client, 500, "Internal Server Error");
            }
        } else if (rs->router->static_dir[0] != '\0') {
            char filepath[4096];
            normalize_path(rs->router->static_dir, path, filepath, sizeof(filepath));
            
            if (is_path_safe(rs->router->static_dir, filepath)) {
                DWORD attrs = GetFileAttributesA(filepath);
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    // BUG-304: index_path 与 filepath 同容量（4096），拼接与回拷容量一致，
                    // GCC 推导不会越界，消除 -Wformat-truncation
                    char index_path[4096];
                    snprintf(index_path, sizeof(index_path), "%s/index.html", filepath);
                    if (GetFileAttributesA(index_path) != INVALID_FILE_ATTRIBUTES) {
                        snprintf(filepath, sizeof(filepath), "%s", index_path);
                    }
                }
                
                size_t file_size;
                char* file_content = read_file(filepath, &file_size);
                
                if (file_content) {
                    char ext[32];
                    get_file_extension(filepath, ext, sizeof(ext));
                    const char* mime_type = get_mime_type(ext);
                    send_http_response(client, mime_type, file_content, file_size);
                    free(file_content);
                    closesocket(client);
                    continue;
                }
            }
            send_http_error(client, 404, "Not Found");
        } else {
            send_http_error(client, 404, "Not Found");
        }
        
        closesocket(client);
    }
    
    return 0;
}

static router_server_t* g_router_server = NULL;

int nl_router_serve(nl_router_t* router, int port) {
    if (!router) return NL_EINVAL;
    
    if (g_router_server) {
        g_router_server->running = 0;
        if (g_router_server->thread) {
            WaitForSingleObject(g_router_server->thread, INFINITE);
            CloseHandle(g_router_server->thread);
        }
        if (g_router_server->sock != INVALID_SOCKET) {
            closesocket(g_router_server->sock);
        }
        free(g_router_server);
    }
    
    if (init_winsock() != 0) return NL_ERROR;
    
    router_server_t* rs = (router_server_t*)calloc(1, sizeof(router_server_t));
    if (!rs) return NL_ERROR;
    
    rs->router = router;
    rs->port = port;
    rs->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    
    if (rs->sock == INVALID_SOCKET) {
        free(rs);
        return NL_ERROR;
    }
    
    int opt = 1;
    setsockopt(rs->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);
    
    if (bind(rs->sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(rs->sock);
        free(rs);
        return NL_ERROR;
    }
    
    if (listen(rs->sock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(rs->sock);
        free(rs);
        return NL_ERROR;
    }
    
    rs->running = 1;
    rs->thread = CreateThread(NULL, 0, router_server_thread, rs, 0, NULL);
    if (!rs->thread) {
        rs->running = 0;
        closesocket(rs->sock);
        free(rs);
        return NL_ERROR;
    }
    
    g_router_server = rs;
    windows_log(NL_LOG_INFO, "Router server started on port %d", port);
    return NL_OK;
}

static nl_http_handler_t g_default_handler = NULL;
static void* g_default_handler_data = NULL;

static void default_server_handler(const char* path, nl_http_method_t method,
                                    const char* body, size_t body_size,
                                    char** response, size_t* response_size,
                                    void* user_data) {
    (void)user_data;
    if (g_default_handler) {
        g_default_handler(path, method, body, body_size, response, response_size, g_default_handler_data);
    } else {
        const char* default_resp = "{\"message\":\"NetLeaf v2.4.0\"}";
        *response_size = strlen(default_resp);
        *response = (char*)malloc(*response_size + 1);
        if (*response) {
            // BUG-304: 用 snprintf 取代 strncpy，根除 -Wstringop-truncation
            snprintf(*response, *response_size + 1, "%s", default_resp);
        }
    }
}

int nl_serve(int port, nl_http_handler_t default_handler, void* user_data) {
    g_default_handler = default_handler;
    g_default_handler_data = user_data;
    
    nl_router_t* router = nl_router_create();
    if (!router) return NL_ERROR;
    
    nl_router_add_route(router, "/", NL_METHOD_GET, default_server_handler, NULL);
    
    int result = nl_router_serve(router, port);
    return result;
}

// =========================================
// Inline HTML/Vue Modern Web Server - Windows
// =========================================

// Route type enumeration
typedef enum {
    NL_ROUTE_TYPE_CONTENT = 0,    // Static content (html/vue/json)
    NL_ROUTE_TYPE_FILE = 1,       // File-based (hot reload)
    NL_ROUTE_TYPE_REDIRECT = 2,   // HTTP 302 redirect
    NL_ROUTE_TYPE_PROXY = 3       // Reverse proxy (upstream http/https/tcp)
} nl_route_type_t;

// 上游协议类型（反代使用）
typedef enum {
    NL_PROXY_HTTP = 0,           // 明文 HTTP 上游
    NL_PROXY_HTTPS = 1,          // TLS 上游
    NL_PROXY_TCP = 2             // 原始 TCP 字节透传
} nl_proxy_protocol_t;

typedef struct nl_web_route {
    char path[256];
    char* content;
    size_t content_size;
    char content_type[64];
    nl_route_type_t type;         // Route type
    char file_path[512];          // File path for hot reload
    char redirect_url[512];       // Redirect URL for 302
    char upstream[512];           // 反代上游地址 host:port（不带 scheme）
    nl_proxy_protocol_t protocol; // 反代上游协议
    struct nl_web_route* next;
} nl_web_route_t;

// ============ 上游 keep-alive 连接池（单线程引擎持有，无需加锁）============
// 空闲上游连接按 "host:port|proto" 分桶，取用时探测存活，空闲超时回收。
// 说明：Windows 下 socket 为句柄，fd 字段用 SOCKET 承载（避免 int 截断）。
typedef struct nl_up_pool_entry {
    SOCKET fd;                 // 空闲上游连接 socket
    int64_t last_used_ms;      // 上次归还时间（用于空闲回收）
    struct nl_up_pool_entry* next;
} nl_up_pool_entry_t;
typedef struct {
    char  key[576];            // "host:port|proto"
    nl_up_pool_entry_t* head;
    int   count;
} nl_up_pool_bucket_t;
typedef struct {
    nl_up_pool_bucket_t* buckets;
    int bucket_cap;
    int total_idle;
    int max_total;             // 全局空闲上限，默认 256
    int max_per_key;           // 每 key 空闲上限，默认 8
    int idle_timeout_ms;       // 空闲超时，默认 30000
} nl_web_upstream_pool_t;

// 当前毫秒（GetTickCount64 为系统毫秒计数，用于空闲回收）
static int64_t pr_pool_now_ms(void) {
    return (int64_t)GetTickCount64();
}
// 关闭池中 socket（平台差异集中于此）
static void pr_pool_close_fd(SOCKET fd) { if (fd != INVALID_SOCKET) closesocket(fd); }
// 探测空闲上游连接是否仍存活：MSG_PEEK 非阻塞偷看，WSAEWOULDBLOCK=健康
static int pr_pool_probe_alive(SOCKET fd) {
    if (fd == INVALID_SOCKET) return 0;
    char c;
    int r = recv(fd, &c, 1, MSG_PEEK);
    if (r == 0) return 0;                 // 对端已关闭
    if (r > 0)  return 0;                 // 有残留数据，不复用
    int err = WSAGetLastError();
    return (err == WSAEWOULDBLOCK) ? 1 : 0;
}
// key 构造："host:port|proto"
static void pr_pool_make_key(char* out, size_t cap, const char* host, int port, int proto) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s:%d|%d", host ? host : "", port, proto);
}
// FNV-1a 哈希
static unsigned pr_pool_hash(const char* key) {
    unsigned h = 2166136261u;
    for (; *key; key++) { h ^= (unsigned char)*key; h *= 16777619u; }
    return h;
}
// 定位桶（create=1 时创建新键桶；线性探测解决碰撞）
static nl_up_pool_bucket_t* pr_pool_bucket(nl_web_upstream_pool_t* p, const char* key, int create) {
    if (!p || !p->buckets || p->bucket_cap <= 0) return NULL;
    int start = (int)(pr_pool_hash(key) % (unsigned)p->bucket_cap);
    for (int i = 0; i < p->bucket_cap; i++) {
        nl_up_pool_bucket_t* b = &p->buckets[(start + i) % p->bucket_cap];
        if (b->key[0] == '\0') {
            if (!create) return NULL;
            snprintf(b->key, sizeof(b->key), "%s", key);
            return b;
        }
        if (strcmp(b->key, key) == 0) return b;
    }
    return NULL;
}
// 初始化池
static void pool_init(nl_web_upstream_pool_t* p, int max_total, int max_per_key, int idle_timeout_ms) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->bucket_cap = 64;
    p->buckets = (nl_up_pool_bucket_t*)calloc((size_t)p->bucket_cap, sizeof(nl_up_pool_bucket_t));
    if (!p->buckets) { p->bucket_cap = 0; return; }
    p->max_total = max_total > 0 ? max_total : 256;
    p->max_per_key = max_per_key > 0 ? max_per_key : 8;
    p->idle_timeout_ms = idle_timeout_ms > 0 ? idle_timeout_ms : 30000;
}
// 取一条可用上游连接（探测存活，死连接丢弃）；无可用返回 INVALID_SOCKET
static SOCKET pool_get(nl_web_upstream_pool_t* p, const char* host, int port, int proto) {
    char key[576];
    pr_pool_make_key(key, sizeof(key), host, port, proto);
    nl_up_pool_bucket_t* b = pr_pool_bucket(p, key, 0);
    if (!b) return INVALID_SOCKET;
    nl_up_pool_entry_t** pp = &b->head;
    while (*pp) {
        nl_up_pool_entry_t* e = *pp;
        if (pr_pool_probe_alive(e->fd)) {
            *pp = e->next;
            SOCKET fd = e->fd;
            free(e);
            b->count--; p->total_idle--;
            return fd;
        }
        pr_pool_close_fd(e->fd);       // 死连接：丢弃
        *pp = e->next;
        free(e);
        b->count--; p->total_idle--;
    }
    return INVALID_SOCKET;
}
// 归还一条上游连接（超全局/单 key 上限则直接关闭）
static void pool_put(nl_web_upstream_pool_t* p, const char* key, SOCKET fd) {
    if (fd == INVALID_SOCKET) return;
    if (!p || !p->buckets || !key || key[0] == '\0' ||
        p->total_idle >= p->max_total) { pr_pool_close_fd(fd); return; }
    nl_up_pool_bucket_t* b = pr_pool_bucket(p, key, 1);
    if (!b || b->count >= p->max_per_key) { pr_pool_close_fd(fd); return; }
    nl_up_pool_entry_t* e = (nl_up_pool_entry_t*)malloc(sizeof(*e));
    if (!e) { pr_pool_close_fd(fd); return; }
    e->fd = fd;
    e->last_used_ms = pr_pool_now_ms();
    e->next = b->head;
    b->head = e;
    b->count++; p->total_idle++;
}
// 回收空闲超时连接
static void pool_reap(nl_web_upstream_pool_t* p, int64_t now_ms) {
    if (!p || !p->buckets) return;
    for (int i = 0; i < p->bucket_cap; i++) {
        nl_up_pool_bucket_t* b = &p->buckets[i];
        nl_up_pool_entry_t** pp = &b->head;
        while (*pp) {
            nl_up_pool_entry_t* e = *pp;
            if (now_ms - e->last_used_ms >= p->idle_timeout_ms) {
                pr_pool_close_fd(e->fd);
                *pp = e->next;
                free(e);
                b->count--; p->total_idle--;
            } else {
                pp = &e->next;
            }
        }
    }
}
// 销毁池（关闭全部空闲连接）
static void pool_destroy(nl_web_upstream_pool_t* p) {
    if (!p) return;
    if (p->buckets) {
        for (int i = 0; i < p->bucket_cap; i++) {
            nl_up_pool_entry_t* e = p->buckets[i].head;
            while (e) { nl_up_pool_entry_t* n = e->next; pr_pool_close_fd(e->fd); free(e); e = n; }
            p->buckets[i].head = NULL;
            p->buckets[i].key[0] = '\0';
            p->buckets[i].count = 0;
        }
        free(p->buckets);
        p->buckets = NULL;
    }
    p->bucket_cap = 0;
    p->total_idle = 0;
}

struct nl_web_server {
    int port;
    SOCKET sock;
    HANDLE thread;
    volatile long running;
    nl_web_route_t* routes;
    CRITICAL_SECTION mutex;
    char encoding[32];
    int auto_encoding_enabled;
    char fallback_encoding[32];
    struct nl_web_server* next;
    int error_suggestions_enabled;
    char error_page_templates[8][256];
    nl_redirect_type_t redirect_type;  // Default redirect type (301 or 302)
    // 工作池：accept 线程 + 动态 worker 线程池（堆分配，容量可运行时调整）
    HANDLE* workers;          // 动态分配，容量由 worker_capacity 决定
    int worker_capacity;      // workers 数组当前分配容量
    int worker_count;         // 实际已启动/在跑的 worker 数
    int worker_active;
    // 路由 hash 表（加速 O(n) -> O(1) 查找）
    nl_web_route_t** route_hash;
    int route_hash_size;
    int route_hash_count;
    // 数据驱动反向代理引擎（select 多路复用 + ring 状态表）
    SOCKET *pr_fd_client;      // [idx] -> 客户端 SOCKET
    SOCKET *pr_fd_upstream;    // [idx] -> 上游 SOCKET（0=未连接）
    int    *pr_route_idx;     // [idx] -> route 在 routes 链表下标
    uint8_t *pr_state;         // [idx] -> 无状态状态位（bit0 方向 / bit1 EOF / bit7 活跃）
    char  **pr_hbuf;          // [idx] -> 每连接 8KB 首包缓冲（malloc/free）
    char  **pr_hbuf_out;      // [idx] -> 每连接 8KB 上游请求构造缓冲
    int    *pr_hdr_len;       // [idx] -> 已收请求头累计字节数
    int    *pr_remain;        // [idx] -> 该方向已收待发剩余字节数
    int  pr_cap;              // ring 容量
    int  pr_count;           // 活跃连接数
    int  pr_free_top;        // 回收栈顶（pr_free[0..pr_free_top] 为空闲槽）
    int  *pr_free;           // 回收栈（存空闲 idx）
    // ---- 上游 keep-alive 连接池（单线程引擎持有，无需锁）----
    nl_web_upstream_pool_t* up_pool;  // 空闲上游连接池（NULL=未启用）
    // 响应 framing 解析（每上游连接一份状态，旁路判定响应是否完整可复用）
    uint8_t *pr_up_parse;    // [idx] -> 解析状态（见 PR_UP_*）
    int64_t *pr_up_cl;       // [idx] -> 剩余 Content-Length
    int64_t *pr_chunk_rem;   // [idx] -> chunk 当前段剩余字节
    uint8_t *pr_chunk_state; // [idx] -> chunk 子状态（见 PR_CHUNK_*）
    int     *pr_up_hlen;     // [idx] -> 上游响应头/临时行缓冲已用长度
    uint8_t *pr_req_is_head; // [idx] -> 客户端请求是否 HEAD（无 body）
    char   **pr_up_key;      // [idx] -> 池 key（归还时使用，slot 归还即释放）
    // 上游→客户端方向的延迟写：客户端写慢（send WSAEWOULDBLOCK）时按 slot 缓冲待补发字节
    char   **pr_up_pend;     // [idx] -> 待补发缓冲（懒分配，PR_PROXY_BUF_SZ）
    int     *pr_up_pend_len; // [idx] -> 待补发剩余字节数
    HANDLE iocp;             // IOCP 完成端口（NULL=select 回退，非 NULL=IOCP 主路径）
    SOCKET pr_accept_client; // AcceptEx 预创建的备用 client socket
    void*  pr_iocp_client_ctx;  // [cap] 客户端侧 OVERLAPPED 上下文（pr_iocp_ctx_t*）
    void*  pr_iocp_up_ctx;      // [cap] 上游侧 OVERLAPPED 上下文（pr_iocp_ctx_t*）
    void*  pr_iocp_cw_ctx;      // [cap] 客户端写侧 OVERLAPPED 上下文（补发上游→客户端余量）
};
static struct nl_web_server* g_web_servers = NULL;
static CRITICAL_SECTION g_web_servers_mutex;
static int g_auto_cleanup_enabled = 0;

static int g_mutex_initialized = 0;

// 路由 hash 表辅助函数（FNV-1a + 线性探测，桶存 route* 指针，不修改 route->next）
static const int NL_ROUTE_HASH_SIZE = 128;

static unsigned nl_route_hash(const char* path) {
    unsigned h = 2166136261u;
    for (; *path; path++) {
        h ^= (unsigned char)*path;
        h *= 16777619u;
    }
    return h % NL_ROUTE_HASH_SIZE;
}

static void nl_route_hash_init(nl_web_server_t* server) {
    if (server->route_hash) return;
    server->route_hash_size = NL_ROUTE_HASH_SIZE;
    server->route_hash_count = 0;
    server->route_hash = (nl_web_route_t**)calloc(NL_ROUTE_HASH_SIZE, sizeof(nl_web_route_t*));
}

static void nl_route_hash_insert_simple(nl_web_server_t* server, nl_web_route_t* route) {
    if (!server->route_hash) return;
    int start = (int)nl_route_hash(route->path);
    for (int i = 0; i < server->route_hash_size; i++) {
        int idx = (start + i) % server->route_hash_size;
        if (server->route_hash[idx] == NULL) {
            server->route_hash[idx] = route;
            server->route_hash_count++;
            return;
        }
        if (strcmp(server->route_hash[idx]->path, route->path) == 0) {
            server->route_hash[idx] = route;
            return;
        }
    }
}

static nl_web_route_t* nl_route_hash_lookup_simple(nl_web_server_t* server, const char* path) {
    if (!server->route_hash) return NULL;
    int start = (int)nl_route_hash(path);
    for (int i = 0; i < server->route_hash_size; i++) {
        int idx = (start + i) % server->route_hash_size;
        nl_web_route_t* r = server->route_hash[idx];
        if (r == NULL) return NULL;
        if (strcmp(r->path, path) == 0) return r;
    }
    return NULL;
}

static void nl_route_hash_rebuild(nl_web_server_t* server) {
    if (!server->route_hash) nl_route_hash_init(server);
    for (int i = 0; i < server->route_hash_size; i++) server->route_hash[i] = NULL;
    server->route_hash_count = 0;
    for (nl_web_route_t* r = server->routes; r; r = r->next) nl_route_hash_insert_simple(server, r);
}

static void init_web_mutex(void) {
    if (!g_mutex_initialized) {
        InitializeCriticalSection(&g_web_servers_mutex);
        g_mutex_initialized = 1;
    }
}

// Modern responsive CSS base
static const char* nl_responsive_css = 
    "<style>\n"
    "  * { margin:0; padding:0; box-sizing:border-box; }\n"
    "  body { font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif; background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); min-height:100vh; padding:20px; }\n"
    "  .container { max-width:800px; margin:0 auto; background:#fff; border-radius:16px; box-shadow:0 20px 60px rgba(0,0,0,0.3); padding:40px; animation:fadeIn 0.5s ease-out; }\n"
    "  @keyframes fadeIn { from{opacity:0; transform:translateY(-20px);} to{opacity:1; transform:translateY(0);} }\n"
    "  h1 { color:#2d3748; font-size:2.5rem; margin-bottom:24px; }\n"
    "  .btn { background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); color:white; border:none; padding:14px 28px; font-size:1rem; border-radius:8px; cursor:pointer; transition:transform 0.2s, box-shadow 0.2s; margin:8px; }\n"
    "  .btn:hover { transform:translateY(-2px); box-shadow:0 8px 20px rgba(102,126,234,0.4); }\n"
    "  .btn:active { transform:translateY(0); }\n"
    "  .counter { font-size:4rem; font-weight:800; color:#667eea; text-align:center; margin:24px 0; }\n"
    "  input, textarea { width:100%; padding:12px; border:2px solid #e2e8f0; border-radius:8px; font-size:1rem; transition:border-color 0.2s; margin:8px 0; }\n"
    "  input:focus, textarea:focus { outline:none; border-color:#667eea; }\n"
    "  .card { background:#f7fafc; border-radius:12px; padding:24px; margin:16px 0; border-left:4px solid #667eea; }\n"
    "  .grid { display:grid; grid-template-columns:repeat(auto-fit, minmax(200px,1fr)); gap:16px; }\n"
    "  .stat { background:white; padding:24px; border-radius:12px; text-align:center; box-shadow:0 4px 12px rgba(0,0,0,0.1); }\n"
    "  .stat-value { font-size:2.5rem; font-weight:800; color:#667eea; }\n"
    "  .stat-label { color:#718096; font-size:0.9rem; margin-top:8px; }\n"
    "</style>\n";

static const char* nl_vue_cdn = 
    "<script src=\"https://unpkg.com/vue@3/dist/vue.global.js\"></script>\n";

// 连接上游服务器，成功返回套接字，失败返回 INVALID_SOCKET。
// 依据 route 中已解析好的 upstream(host:port) 与 protocol 建立 TCP/TLS 连接。
// select 数据驱动引擎会前置调用，故需前置声明。
static SOCKET nl_proxy_connect_upstream(const nl_web_route_t* route);

// ---------------------------------------------------------------------------
// 数据驱动反向代理引擎（Windows select 版）
// ---------------------------------------------------------------------------
#define PR_ST_DIR_W   0x01   // 方向：等上游可读（写下游）
#define PR_ST_UP_EOF  0x02   // 上游已 EOF，写尽后关闭
#define PR_ST_ACTIVE  0x80   // 该槽活跃
#define PR_PROXY_BUF_SZ 65536

// ---- 上游响应 framing 解析状态（旁路判定响应是否完整可复用）----
#define PR_UP_HDR     0  // 响应头未收全
#define PR_UP_CL      1  // 按 Content-Length 计 body
#define PR_UP_CHUNK   2  // Transfer-Encoding: chunked
#define PR_UP_DONE    3  // 完成，可复用
#define PR_UP_EOF     4  // 不可复用（body 以 EOF 结束 / Connection: close）
// ---- chunked 子状态 ----
#define PR_CHUNK_SIZE    0  // 读 chunk 大小行
#define PR_CHUNK_DATA    1  // 读 chunk 数据
#define PR_CHUNK_CRLF    2  // 读 chunk 数据后的 CRLF
#define PR_CHUNK_TRAILER 3  // 读 trailer / 终止空行

// ---- IOCP 事件类型常量 ----
#define IOCP_EV_ACCEPT        1  // AcceptEx 完成
#define IOCP_EV_CLIENT_READ   2  // WSARecv 客户端读完成
#define IOCP_EV_UPSTREAM_READ 3  // WSARecv 上游读完成
#define IOCP_EV_CLIENT_WRITE  4  // WSASend 客户端写完成（补发上游→客户端余量）
#define IOCP_LISTEN_KEY 0xFFFFFFFFUL  // listen socket completion key 哨兵

// IOCP OVERLAPPED 上下文（嵌 OVERLAPPED，每次 WSARecv/AcceptEx 唯一实例）
typedef struct {
    OVERLAPPED ov;
    int  idx;        // ring slot 索引
    int  ev;         // IOCP_EV_* 事件类型
    SOCKET sock;     // 关联的 socket（AcceptEx 完成时填新 client）
    char *buf;       // 读写缓冲（非 OWN，指向 pr_hbuf[idx] / 临时缓冲）
    DWORD buflen;    // 缓冲长度
} pr_iocp_ctx_t;
// IOCP 不受 FD_SETSIZE 限制，与 Linux/macOS 对齐：按逻辑核数 × 64 动态定容
// 下限 64，上限 4096（工程保护值，IOCP 本身无硬上限）。
static int nl_web_proxy_ring_capacity(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cap = (int)si.dwNumberOfProcessors * 64;
    if (cap < 1) cap = 1;
    if (cap > 4096) cap = 4096;
    if (cap < 64)   cap = 64;
    return cap;
}

static int pr_alloc_slot(nl_web_server_t* s) {
    if (s->pr_count >= s->pr_cap || s->pr_free_top < 0) return -1;
    int idx = s->pr_free[s->pr_free_top--];
    s->pr_count++;
    return idx;
}

static void pr_free_slot(nl_web_server_t* s, int idx) {
    if (idx < 0 || idx >= s->pr_cap) return;
    if (s->pr_hbuf[idx])     { free(s->pr_hbuf[idx]);     s->pr_hbuf[idx] = NULL; }
    if (s->pr_hbuf_out[idx]) { free(s->pr_hbuf_out[idx]); s->pr_hbuf_out[idx] = NULL; }
    if (s->pr_up_key && s->pr_up_key[idx]) { free(s->pr_up_key[idx]); s->pr_up_key[idx] = NULL; }
    if (s->pr_up_pend && s->pr_up_pend[idx]) { free(s->pr_up_pend[idx]); s->pr_up_pend[idx] = NULL; }
    if (s->pr_up_pend_len) s->pr_up_pend_len[idx] = 0;
    s->pr_state[idx] = 0;
    s->pr_fd_client[idx]   = INVALID_SOCKET;
    s->pr_fd_upstream[idx] = 0;
    s->pr_route_idx[idx]   = -1;
    if (s->pr_hdr_len) s->pr_hdr_len[idx] = 0;
    if (s->pr_remain)  s->pr_remain[idx]  = 0;
    if (s->pr_up_parse) s->pr_up_parse[idx] = PR_UP_HDR;
    if (s->pr_up_cl) s->pr_up_cl[idx] = 0;
    if (s->pr_chunk_rem) s->pr_chunk_rem[idx] = 0;
    if (s->pr_chunk_state) s->pr_chunk_state[idx] = 0;
    if (s->pr_up_hlen) s->pr_up_hlen[idx] = 0;
    if (s->pr_req_is_head) s->pr_req_is_head[idx] = 0;
    s->pr_free[++(s->pr_free_top)] = idx;
    if (s->pr_count > 0) s->pr_count--;
}

// 忽略大小写判断 [s, s+n) 是否以 prefix 开头（prefix 为小写字面量）
static int pr_ci_starts(const char* s, size_t n, const char* prefix) {
    size_t i = 0;
    for (; prefix[i]; i++) {
        if (i >= n) return 0;
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != prefix[i]) return 0;
    }
    return 1;
}
// 忽略大小写判断 [s, s+n) 是否包含 needle（needle 为小写字面量）
static int pr_ci_has(const char* s, size_t n, const char* needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    if (n < nl) return 0;
    for (size_t i = 0; i + nl <= n; i++) {
        size_t j = 0;
        for (; j < nl; j++) {
            char c = s[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != needle[j]) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}
// 在 [s, s+n) 中查找 needle（可移植，替代非标准 memmem）
static const char* pr_memfind(const char* s, size_t n, const char* needle, size_t nl) {
    if (nl == 0) return s;
    if (n < nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++)
        if (s[i] == needle[0] && memcmp(s + i, needle, nl) == 0) return s + i;
    return NULL;
}

// 解析响应头块，确定 body framing 与是否可复用（结果写入 pr_up_parse 等字段）
static void pr_up_parse_headers(nl_web_server_t* s, int idx, const char* hdr, int hlen) {
    const char* end = hdr + hlen;
    // 状态行：第一个空格后 3 位为状态码
    int status = 0;
    const char* sp = (const char*)memchr(hdr, ' ', (size_t)hlen);
    if (sp && sp + 4 <= end) {
        status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    }
    int no_body = (s->pr_req_is_head && s->pr_req_is_head[idx]) ||
                  (status >= 100 && status < 200) || status == 204 || status == 304;
    int conn_close = 0;
    long long cl = -1;
    int chunked = 0;
    const char* line = pr_memfind(hdr, (size_t)hlen, "\r\n", 2);
    if (line) line += 2; else line = end;
    while (line < end) {
        const char* le = pr_memfind(line, (size_t)(end - line), "\r\n", 2);
        if (!le) break;
        size_t ll = (size_t)(le - line);
        if (ll == 0) break;
        if (ll >= 11 && pr_ci_starts(line, ll, "connection:")) {
            if (pr_ci_has(line + 11, ll - 11, "close")) conn_close = 1;
        } else if (ll >= 15 && pr_ci_starts(line, ll, "content-length:")) {
            cl = atoll(line + 15);
        } else if (ll >= 18 && pr_ci_starts(line, ll, "transfer-encoding:")) {
            if (pr_ci_has(line + 18, ll - 18, "chunked")) chunked = 1;
        }
        line = le + 2;
    }
    if (conn_close) { s->pr_up_parse[idx] = PR_UP_EOF; return; }  // 对端将关闭 → 不可复用
    if (no_body)    { s->pr_up_parse[idx] = PR_UP_DONE; return; } // 无 body → 完成
    if (cl == 0)    { s->pr_up_parse[idx] = PR_UP_DONE; return; }
    if (cl > 0) {
        s->pr_up_cl[idx] = (int64_t)cl;
        s->pr_up_parse[idx] = PR_UP_CL;
        return;
    }
    if (chunked) {
        s->pr_chunk_state[idx] = PR_CHUNK_SIZE;
        s->pr_chunk_rem[idx] = 0;
        s->pr_up_hlen[idx] = 0;
        s->pr_up_parse[idx] = PR_UP_CHUNK;
        return;
    }
    // 无 Content-Length 且非 chunked：body 以 EOF 结束 → 不可复用
    s->pr_up_parse[idx] = PR_UP_EOF;
}

// 消费 chunked body 字节，推进至终止帧（0\r\n\r\n）后置 PR_UP_DONE
static void pr_up_parse_chunked(nl_web_server_t* s, int idx, const char* data, size_t* ppos, size_t len) {
    size_t pos = *ppos;
    while (pos < len) {
        uint8_t cs = s->pr_chunk_state[idx];
        if (cs == PR_CHUNK_SIZE || cs == PR_CHUNK_TRAILER) {
            // 累积一行到 pr_hbuf_out（临时行缓冲），pr_up_hlen 记长度
            char* lb = s->pr_hbuf_out[idx];
            int* ll = &s->pr_up_hlen[idx];
            while (pos < len) {
                char c = data[pos++];
                if (*ll < 8191) lb[(*ll)++] = c;
                else { s->pr_up_parse[idx] = PR_UP_EOF; *ppos = pos; return; }
                if (c == '\n') break;
            }
            if (*ll > 0 && lb[*ll - 1] == '\n') {
                int line_len = *ll;
                while (line_len > 0 && (lb[line_len - 1] == '\n' || lb[line_len - 1] == '\r')) line_len--;
                if (cs == PR_CHUNK_SIZE) {
                    long long sz = 0; int ok = 0;
                    for (int i = 0; i < line_len; i++) {
                        char c = lb[i];
                        int d;
                        if (c == ';') break;                       // chunk 扩展，忽略其后
                        else if (c >= '0' && c <= '9') d = c - '0';
                        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                        else if (c == ' ' || c == '\t') { if (!ok) continue; else break; }
                        else { s->pr_up_parse[idx] = PR_UP_EOF; *ll = 0; *ppos = pos; return; }
                        sz = sz * 16 + d; ok = 1;
                    }
                    *ll = 0;
                    if (sz == 0) { s->pr_chunk_state[idx] = PR_CHUNK_TRAILER; }
                    else { s->pr_chunk_rem[idx] = sz; s->pr_chunk_state[idx] = PR_CHUNK_DATA; }
                } else {
                    *ll = 0;
                    if (line_len == 0) { s->pr_up_parse[idx] = PR_UP_DONE; *ppos = pos; return; }
                }
            }
            continue;
        }
        if (cs == PR_CHUNK_DATA) {
            long long take = (long long)(len - pos);
            if (take > s->pr_chunk_rem[idx]) take = s->pr_chunk_rem[idx];
            pos += (size_t)take;
            s->pr_chunk_rem[idx] -= take;
            if (s->pr_chunk_rem[idx] == 0) { s->pr_chunk_state[idx] = PR_CHUNK_CRLF; s->pr_chunk_rem[idx] = 2; }
            continue;
        }
        if (cs == PR_CHUNK_CRLF) {
            long long take = (long long)(len - pos);
            if (take > s->pr_chunk_rem[idx]) take = s->pr_chunk_rem[idx];
            pos += (size_t)take;
            s->pr_chunk_rem[idx] -= take;
            if (s->pr_chunk_rem[idx] == 0) s->pr_chunk_state[idx] = PR_CHUNK_SIZE;
            continue;
        }
        break;
    }
    *ppos = pos;
}

// 旁路解析被转发的上游字节（方向：上游→客户端），判定响应是否完整可复用。
// 绝不修改 data（转发必须字节级原样）。
static void pr_parse_upstream_bytes(nl_web_server_t* s, int idx, const char* data, size_t len) {
    if (!s || !s->pr_up_parse || !s->pr_hbuf_out || !data || len == 0) return;
    size_t pos = 0;
    while (pos < len) {
        uint8_t st = s->pr_up_parse[idx];
        if (st == PR_UP_DONE || st == PR_UP_EOF) return;  // 终态：停止解析
        if (st == PR_UP_HDR) {
            char* hb = s->pr_hbuf_out[idx];
            int* hl = &s->pr_up_hlen[idx];
            int found = 0;
            while (pos < len) {
                if (*hl >= 8190) { s->pr_up_parse[idx] = PR_UP_EOF; return; }  // 头过大：判不可复用
                char c = data[pos++];
                hb[(*hl)++] = c;
                if (*hl >= 4 && hb[*hl - 4] == '\r' && hb[*hl - 3] == '\n' &&
                    hb[*hl - 2] == '\r' && hb[*hl - 1] == '\n') { found = 1; break; }
            }
            if (found) { pr_up_parse_headers(s, idx, hb, *hl); *hl = 0; }
            continue;
        }
        if (st == PR_UP_CL) {
            long long rem = s->pr_up_cl[idx];
            long long take = (long long)(len - pos);
            if (take >= rem) { pos += (size_t)rem; s->pr_up_cl[idx] = 0; s->pr_up_parse[idx] = PR_UP_DONE; }
            else { s->pr_up_cl[idx] = rem - take; pos = len; }
            continue;
        }
        if (st == PR_UP_CHUNK) { pr_up_parse_chunked(s, idx, data, &pos, len); continue; }
        return;
    }
}

static int pr_lookup_route_idx(nl_web_server_t* s, const char* path) {
    int idx = 0;
    EnterCriticalSection(&s->mutex);
    for (nl_web_route_t* r = s->routes; r; r = r->next, idx++) {
        if (strcmp(r->path, path) == 0) {
            LeaveCriticalSection(&s->mutex);
            return idx;
        }
    }
    LeaveCriticalSection(&s->mutex);
    return -1;
}

// 非阻塞泵：先补发上次未发完的余量（pr_remain），再读新数据转发。
// 返回值：1=还有数据（EAGAIN/读满），0=对端 EOF，-1=错误。
static int pr_pump_once(nl_web_server_t* s, int idx, SOCKET from, SOCKET to, char* buf, size_t cap) {
    int* remain = s->pr_remain;
    while (remain && remain[idx] > 0) {
        int w = send(to, buf, remain[idx], 0);
        if (w < 0) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) return 1;
            return -1;
        }
        if (w == 0) return 0; // 对端关闭连接（recv 0）
        remain[idx] -= w;
    }
    int n = recv(from, buf, (int)cap, 0);
    if (n < 0) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return 1;
        return -1;
    }
    if (n == 0) {
        if (remain && remain[idx] > 0) return 1;
        return 0;
    }
    remain[idx] = n;
    while (remain[idx] > 0) {
        int w = send(to, buf, remain[idx], 0);
        if (w < 0) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) return 1;
            return -1;
        }
        if (w == 0) break;
        remain[idx] -= w;
    }
    return 1;
}

// 构造上游请求，输出到 out，返回字节数（-1=失败）
static int pr_build_upstream_http(nl_web_server_t* s, int idx,
                                  const char* method, const char* path,
                                  const char* proto,
                                  const char* raw_hdrs, size_t raw_hdrs_len,
                                  char* out, size_t out_cap) {
    nl_web_route_t* r;
    EnterCriticalSection(&s->mutex);
    int rd = s->pr_route_idx[idx];
    r = NULL;
    int cur = 0;
    for (nl_web_route_t* it = s->routes; it; it = it->next, cur++) {
        if (cur == rd) { r = it; break; }
    }
    if (rd < 0 || !r) { LeaveCriticalSection(&s->mutex); return -1; }
    int total = 0;
    if (r->protocol == NL_PROXY_TCP) {
        total = (int)raw_hdrs_len;
        if (total > 0 && total < (int)out_cap) memcpy(out, raw_hdrs, total);
        else total = 0;
    } else {
        total = snprintf(out, out_cap,
                         "%s %s %s\r\nHost: %s\r\nX-Forwarded-For: 127.0.0.1\r\nConnection: keep-alive\r\n",
                         method, path, proto, r->upstream);
        if ((size_t)total >= out_cap) total = (int)out_cap - 1;
        if (raw_hdrs_len > 0 && (size_t)total + raw_hdrs_len < out_cap) {
            memcpy(out + total, raw_hdrs, raw_hdrs_len);
            total += (int)raw_hdrs_len;
        }
    }
    LeaveCriticalSection(&s->mutex);
    return total;
}

// select 数据驱动主循环：单线程 select 驱动 N 条 proxy 连接
// 空闲 200ms 退避（select 超时），非阻塞 socket（WSAEMSGSIZE/EWOULDBLOCK）。
void nl_web_proxy_engine(nl_web_server_t* s, SOCKET listen_sock) {
    int cap = nl_web_proxy_ring_capacity();
    s->pr_cap = cap;
    s->pr_fd_client   = (SOCKET*)calloc((size_t)cap, sizeof(SOCKET));
    s->pr_fd_upstream = (SOCKET*)calloc((size_t)cap, sizeof(SOCKET));
    s->pr_route_idx   = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_state       = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_hbuf        = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_hbuf_out    = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_hdr_len     = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_remain      = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_free        = (int*)malloc((size_t)cap * sizeof(int));
    s->pr_up_parse    = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_up_cl       = (int64_t*)calloc((size_t)cap, sizeof(int64_t));
    s->pr_chunk_rem   = (int64_t*)calloc((size_t)cap, sizeof(int64_t));
    s->pr_chunk_state = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_up_hlen     = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_req_is_head = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_up_key      = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_up_pend     = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_up_pend_len = (int*)calloc((size_t)cap, sizeof(int));
    for (int i = 0; i < cap; i++) {
        s->pr_fd_client[i]   = INVALID_SOCKET;
        s->pr_fd_upstream[i] = 0;
        s->pr_route_idx[i]   = -1;
        s->pr_up_parse[i]    = PR_UP_HDR;
        s->pr_free[i] = cap - 1 - i;
    }
    s->pr_free_top = cap - 1;
    s->pr_count = 0;
    if (!s->pr_fd_client || !s->pr_fd_upstream || !s->pr_route_idx ||
        !s->pr_state || !s->pr_hbuf || !s->pr_hbuf_out ||
        !s->pr_hdr_len || !s->pr_remain || !s->pr_free ||
        !s->pr_up_parse || !s->pr_up_cl || !s->pr_chunk_rem ||
        !s->pr_chunk_state || !s->pr_up_hlen || !s->pr_req_is_head || !s->pr_up_key ||
        !s->pr_up_pend || !s->pr_up_pend_len) {
        goto cleanup_fail;
    }
    // 初始化上游 keep-alive 连接池（全局空闲 256，单 key 8，空闲 30s 回收）
    s->up_pool = (nl_web_upstream_pool_t*)malloc(sizeof(nl_web_upstream_pool_t));
    if (!s->up_pool) goto cleanup_fail;
    pool_init(s->up_pool, 256, 8, 30000);

    // ---- IOCP 完成端口初始化 ----
    s->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!s->iocp) {
        windows_log(NL_LOG_ERROR, "IOCP: CreateIoCompletionPort 失败 gle=%lu", GetLastError());
        goto cleanup_fail;
    }
    // listen socket 关联到 IOCP，completion key 用哨兵
    CreateIoCompletionPort((HANDLE)listen_sock, s->iocp, (ULONG_PTR)IOCP_LISTEN_KEY, 0);

    // 预创建 per-slot OVERLAPPED 上下文数组（客户端侧 + 上游侧各 cap 个）
    s->pr_iocp_client_ctx = calloc((size_t)cap, sizeof(pr_iocp_ctx_t));
    s->pr_iocp_up_ctx     = calloc((size_t)cap, sizeof(pr_iocp_ctx_t));
    s->pr_iocp_cw_ctx     = calloc((size_t)cap, sizeof(pr_iocp_ctx_t));
    if (!s->pr_iocp_client_ctx || !s->pr_iocp_up_ctx || !s->pr_iocp_cw_ctx) {
        windows_log(NL_LOG_ERROR, "IOCP: OVERLAPPED ctx calloc 失败");
        goto cleanup_fail;
    }

    // 模块级静态 AcceptEx 上下文（AcceptEx 的 OVERLAPPED 必须持续到完成）
    static pr_iocp_ctx_t accept_ctx;
    memset(&accept_ctx, 0, sizeof(accept_ctx));
    accept_ctx.ev = IOCP_EV_ACCEPT;

    // AcceptEx 预 post：先 WSASocket 一个 OVERLAPPED client 备用 socket
    s->pr_accept_client = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (s->pr_accept_client == INVALID_SOCKET) {
        windows_log(NL_LOG_ERROR, "IOCP: WSASocket (accept) 失败");
        goto cleanup_fail;
    }
    // post 第一个 AcceptEx（addr 缓冲简化为 NULL + 0，让 kernel 处理）
    DWORD bytes_recv_init = 0;
    AcceptEx(listen_sock, s->pr_accept_client, NULL, 0, 0, 0, &bytes_recv_init, (LPOVERLAPPED)&accept_ctx);

    // ---- IOCP 主循环 ----
    while (s->running) {
        DWORD bytes_trans;
        ULONG_PTR compkey;
        LPOVERLAPPED p_ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(s->iocp, &bytes_trans, &compkey, &p_ov, 200);
        DWORD gle = GetLastError();
        // 每轮回收空闲超时的池连接
        pool_reap(s->up_pool, pr_pool_now_ms());
        if (!ok && gle == WAIT_TIMEOUT) continue;   // 空闲退避
        if (!ok || !p_ov) continue;                  // 错误 / 空 OVERLAPPED：跳过
        pr_iocp_ctx_t* ctx = (pr_iocp_ctx_t*)p_ov;

        // ---- AcceptEx 完成 ----
        if (compkey == (ULONG_PTR)IOCP_LISTEN_KEY) {
            // accept_ctx 是静态 OVERLAPPED，AcceptEx 刚把新 client 填入 s->pr_accept_client
            SOCKET new_client = s->pr_accept_client;
            if (new_client != INVALID_SOCKET) {
                int idx = pr_alloc_slot(s);
                if (idx < 0) {
                    closesocket(new_client);
                } else {
                    // 新 client 关联到 IOCP（completion key = idx）
                    CreateIoCompletionPort((HANDLE)new_client, s->iocp, (ULONG_PTR)idx, 0);
                    s->pr_fd_client[idx] = new_client;
                    s->pr_state[idx] = PR_ST_ACTIVE;
                    s->pr_hbuf[idx]     = (char*)malloc(8192);
                    s->pr_hbuf_out[idx] = (char*)malloc(8192);
                    s->pr_up_key[idx]   = (char*)malloc(576);
                    s->pr_hdr_len[idx]  = 0;
                    s->pr_remain[idx]   = 0;
                    s->pr_up_parse[idx] = PR_UP_HDR;
                    s->pr_up_cl[idx]    = 0;
                    s->pr_chunk_rem[idx] = 0;
                    s->pr_chunk_state[idx] = 0;
                    s->pr_up_hlen[idx]  = 0;
                    s->pr_req_is_head[idx] = 0;
                    if (!s->pr_hbuf[idx] || !s->pr_hbuf_out[idx] || !s->pr_up_key[idx]) {
                        closesocket(new_client);
                        pr_free_slot(s, idx);
                    } else {
                        // post 客户端 WSARecv（读首包）
                        pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                        memset(cctx, 0, sizeof(*cctx));
                        cctx->idx = idx;
                        cctx->ev  = IOCP_EV_CLIENT_READ;
                        cctx->sock = new_client;
                        cctx->buf = s->pr_hbuf[idx];
                        cctx->buflen = 8192;
                        WSABUF wsabuf = {8192, cctx->buf};
                        DWORD flags = 0;
                        DWORD dummy = 0;
                        WSARecv(new_client, &wsabuf, 1, &dummy, &flags, (LPOVERLAPPED)cctx, NULL);
                    }
                }
            }
            // 预先创建下一个备用 socket，post 下一次 AcceptEx
            s->pr_accept_client = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            if (s->pr_accept_client != INVALID_SOCKET) {
                DWORD bytes_recv_next = 0;
                AcceptEx(listen_sock, s->pr_accept_client, NULL, 0, 0, 0,
                         &bytes_recv_next, (LPOVERLAPPED)&accept_ctx);
            }
            continue;
        }

        // ---- client / upstream socket 完成（compkey == idx）----
        int idx = (int)compkey;
        if (idx < 0 || idx >= cap) continue;
        if (!(s->pr_state[idx] & PR_ST_ACTIVE)) continue;

        SOCKET cfd = s->pr_fd_client[idx];
        if (cfd == INVALID_SOCKET) continue;

        if (ctx->ev == IOCP_EV_CLIENT_READ) {
            // ---- WSARecv 客户端读完成 ----
            int nread = (int)bytes_trans;
            if (nread <= 0) {
                // 客户端 EOF / 错误
                if (s->pr_fd_upstream[idx] > 0) { closesocket(s->pr_fd_upstream[idx]); s->pr_fd_upstream[idx] = 0; }
                closesocket(cfd); s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                continue;
            }

            char* hbuf = s->pr_hbuf[idx];
            int* hlen = &s->pr_hdr_len[idx];
            *hlen += nread;
            hbuf[*hlen] = '\0';

            if (s->pr_fd_upstream[idx] == 0) {
                // 首次收到首行：解析 method/path/proto，路由，连上游
                char* line_end = memchr(hbuf, '\n', (size_t)*hlen);
                if (!line_end) {
                    // 等更多数据：再 post 一次 WSARecv
                    pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                    WSABUF wsabuf = {8192 - (ULONG)*hlen, hbuf + *hlen};
                    DWORD flags = 0, dummy = 0;
                    WSARecv(cfd, &wsabuf, 1, &dummy, &flags, (LPOVERLAPPED)cctx, NULL);
                    continue;
                }
                *line_end = '\0';
                char method[16] = {0}, path[4096] = {0}, proto[32] = {0};
                char* sp1 = strchr(hbuf, ' ');
                if (!sp1) { send_http_error(cfd, 400, "Bad Request"); closesocket(cfd); pr_free_slot(s, idx); continue; }
                char* sp2 = strchr(sp1 + 1, ' ');
                if (!sp2) { send_http_error(cfd, 400, "Bad Request"); closesocket(cfd); pr_free_slot(s, idx); continue; }
                size_t mlen = (size_t)(sp1 - hbuf);
                size_t plen = (size_t)(sp2 - (sp1 + 1));
                size_t vlen = (size_t)(line_end - (sp2 + 1));
                if (mlen == 0 || mlen >= sizeof(method) ||
                    plen == 0 || plen >= sizeof(path) ||
                    vlen == 0 || vlen >= sizeof(proto)) {
                    send_http_error(cfd, 400, "Bad Request");
                    closesocket(cfd);
                    pr_free_slot(s, idx);
                    continue;
                }
                memcpy(method, hbuf, mlen);
                memcpy(path, sp1 + 1, plen);
                memcpy(proto, sp2 + 1, vlen);
                // HEAD 请求无响应 body，用于响应 framing 判定
                s->pr_req_is_head[idx] = (strcmp(method, "HEAD") == 0) ? 1 : 0;
                *hlen = 0;

                int ridx = pr_lookup_route_idx(s, path);
                nl_web_route_t* r = NULL;
                EnterCriticalSection(&s->mutex);
                int cur = 0;
                for (nl_web_route_t* it = s->routes; it; it = it->next, cur++)
                    if (cur == ridx) { r = it; break; }
                LeaveCriticalSection(&s->mutex);
                if (!r) {
                    send_http_error(cfd, 404, "Not Found");
                    closesocket(cfd);
                    pr_free_slot(s, idx);
                    continue;
                }

                // ---------- 完整混合路由分发 ----------
                if (r->type != NL_ROUTE_TYPE_PROXY) {
                    // 非 proxy：临时切阻塞发响应，处理完立即 close
                    u_long nb_off = 0;
                    ioctlsocket(cfd, FIONBIO, &nb_off);

                    if (r->type == NL_ROUTE_TYPE_REDIRECT) {
                        char resp[1024];
                        snprintf(resp, sizeof(resp),
                            "HTTP/1.1 302 Found\r\n"
                            "Location: %s\r\n"
                            "Content-Length: 0\r\n"
                            "Connection: close\r\n\r\n",
                            r->redirect_url);
                        send(cfd, resp, (int)strlen(resp), 0);
                    }
                    else if (r->type == NL_ROUTE_TYPE_FILE) {
                        FILE* fp = fopen(r->file_path, "rb");
                        if (!fp) {
                            send_http_error(cfd, 404, "File Not Found");
                        } else {
                            fseek(fp, 0, SEEK_END);
                            long fsz = ftell(fp);
                            fseek(fp, 0, SEEK_SET);
                            if (fsz > 0 && fsz <= 10*1024*1024) {
                                char* fc = (char*)malloc(fsz + 1);
                                if (fc) {
                                    size_t rs = fread(fc, 1, fsz, fp);
                                    fc[rs] = '\0';
                                    send_http_response(cfd, r->content_type, fc, rs);
                                    free(fc);
                                } else {
                                    send_http_error(cfd, 500, "Memory Error");
                                }
                            } else {
                                send_http_error(cfd, 500, "File Too Large");
                            }
                            fclose(fp);
                        }
                    }
                    else { // NL_ROUTE_TYPE_CONTENT
                        send_http_response(cfd, r->content_type,
                                          r->content, strlen(r->content));
                    }
                    closesocket(cfd);   // 取消所有 pending I/O
                    pr_free_slot(s, idx);
                    continue;
                }

                // ---------- proxy 路由：优先复用池中 keep-alive 连接 ----------
                // 解析上游 host:port（供池 key 使用；tcp 不参与池）
                char up_host[512];
                int  up_port = 0;
                {
                    char up_copy[512];
                    snprintf(up_copy, sizeof(up_copy), "%s", r->upstream);
                    char* colon = strrchr(up_copy, ':');
                    if (colon) { *colon = '\0'; up_port = atoi(colon + 1); snprintf(up_host, sizeof(up_host), "%s", up_copy); }
                    else { up_port = (r->protocol == NL_PROXY_HTTPS) ? 443 : 80; snprintf(up_host, sizeof(up_host), "%s", up_copy); }
                    if (up_port <= 0) up_port = (r->protocol == NL_PROXY_HTTPS) ? 443 : 80;
                }
                SOCKET ufd_new = INVALID_SOCKET;
                if (r->protocol != NL_PROXY_TCP) {
                    ufd_new = pool_get(s->up_pool, up_host, up_port, (int)r->protocol);
                    if (ufd_new != INVALID_SOCKET) {
                        // 命中池：重关联到当前 slot（completion key = idx）
                        CreateIoCompletionPort((HANDLE)ufd_new, s->iocp, (ULONG_PTR)idx, 0);
                    }
                }
                if (ufd_new == INVALID_SOCKET) {
                    ufd_new = nl_proxy_connect_upstream(r);
                }
                if (ufd_new == INVALID_SOCKET) {
                    send_http_error(cfd, 502, "Bad Gateway");
                    closesocket(cfd);
                    pr_free_slot(s, idx);
                    continue;
                }
                // 记录池 key（归还时使用）；tcp 置空表示不参与池
                if (r->protocol != NL_PROXY_TCP) {
                    pr_pool_make_key(s->pr_up_key[idx], 576, up_host, up_port, (int)r->protocol);
                } else {
                    s->pr_up_key[idx][0] = '\0';
                }
                // 上游 socket 切非阻塞 + 关联 IOCP
                u_long nb3 = 1;
                ioctlsocket(ufd_new, FIONBIO, &nb3);
                CreateIoCompletionPort((HANDLE)ufd_new, s->iocp, (ULONG_PTR)idx, 0);
                s->pr_fd_upstream[idx] = ufd_new;
                s->pr_route_idx[idx]   = ridx;

                // 构造上游请求并**同步阻塞发出去**（刚建连，还没 post OVERLAPPED，send 即可）
                char* ubuf = s->pr_hbuf_out[idx];
                int build_n = pr_build_upstream_http(s, idx, method, path, proto, NULL, 0, ubuf, 8192);
                if (build_n > 0) {
                    // 临时切阻塞，保证初始请求完整发出去
                    u_long nb_off2 = 0;
                    ioctlsocket(ufd_new, FIONBIO, &nb_off2);
                    int off = 0;
                    while (off < build_n) {
                        int w = send(ufd_new, ubuf + off, build_n - off, 0);
                        if (w <= 0) break;
                        off += w;
                    }
                    u_long nb_on = 1;
                    ioctlsocket(ufd_new, FIONBIO, &nb_on);
                }

                // post 上游 WSARecv（响应回来 → pump 回客户端）
                // 懒分配 per-slot 上游响应缓冲（pr_up_pend[idx]），替换共享 pump_buf 避免并发竞争
                if (!s->pr_up_pend[idx]) {
                    s->pr_up_pend[idx] = (char*)malloc(PR_PROXY_BUF_SZ);
                    if (!s->pr_up_pend[idx]) {
                        if (ufd_new > 0) { closesocket(ufd_new); s->pr_fd_upstream[idx] = 0; }
                        closesocket(cfd); s->pr_fd_client[idx] = INVALID_SOCKET;
                        pr_free_slot(s, idx);
                        continue;
                    }
                }
                pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
                memset(uctx, 0, sizeof(*uctx));
                uctx->idx = idx;
                uctx->ev  = IOCP_EV_UPSTREAM_READ;
                uctx->sock = ufd_new;
                uctx->buf  = s->pr_up_pend[idx];
                uctx->buflen = PR_PROXY_BUF_SZ;
                WSABUF wsb_up = {PR_PROXY_BUF_SZ, s->pr_up_pend[idx]};
                DWORD flg_up = 0, dummy_up = 0;
                WSARecv(ufd_new, &wsb_up, 1, &dummy_up, &flg_up, (LPOVERLAPPED)uctx, NULL);

                // 再 post 一次客户端 WSARecv（可能还有 body 未读）
                pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                memset(cctx, 0, sizeof(*cctx));
                cctx->idx = idx;
                cctx->ev  = IOCP_EV_CLIENT_READ;
                cctx->sock = cfd;
                cctx->buf  = hbuf;
                cctx->buflen = 8192;
                WSABUF wsabuf2 = {8192, hbuf};
                DWORD flags2 = 0, dummy2 = 0;
                WSARecv(cfd, &wsabuf2, 1, &dummy2, &flags2, (LPOVERLAPPED)cctx, NULL);

                s->pr_state[idx] = PR_ST_DIR_W | PR_ST_ACTIVE;
                continue;
            }

            // ---- 后续客户端数据（body 等）→ 阻塞转发上游 ----
            if (s->pr_fd_upstream[idx] > 0) {
                int w = (int)bytes_trans;
                u_long nb_off3 = 0;
                ioctlsocket(s->pr_fd_upstream[idx], FIONBIO, &nb_off3);
                int off2 = 0;
                while (off2 < w) {
                    int wc = send(s->pr_fd_upstream[idx], hbuf + off2, w - off2, 0);
                    if (wc <= 0) break;
                    off2 += wc;
                }
                u_long nb_on2 = 1;
                ioctlsocket(s->pr_fd_upstream[idx], FIONBIO, &nb_on2);
                // 继续 post 客户端 WSARecv
                pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                memset(cctx, 0, sizeof(*cctx));
                cctx->idx = idx;
                cctx->ev  = IOCP_EV_CLIENT_READ;
                cctx->sock = cfd;
                cctx->buf  = hbuf;
                cctx->buflen = 8192;
                WSABUF wsabuf3 = {8192, hbuf};
                DWORD flags3 = 0, dummy3 = 0;
                WSARecv(cfd, &wsabuf3, 1, &dummy3, &flags3, (LPOVERLAPPED)cctx, NULL);
            }
            continue;
        }

        else if (ctx->ev == IOCP_EV_UPSTREAM_READ) {
            // ---- 上游 WSARecv 完成 → pump 回客户端 ----
            int nread_up = (int)bytes_trans;
            SOCKET ufd = s->pr_fd_upstream[idx];

            if (nread_up <= 0) {
                // 上游 EOF
                s->pr_state[idx] |= PR_ST_UP_EOF;
                if (ufd > 0) { closesocket(ufd); s->pr_fd_upstream[idx] = 0; }
                closesocket(cfd); s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                continue;
            }

            // 旁路解析本批上游字节（判定响应 framing，不修改转发字节）
            // 数据已在 per-slot pr_up_pend[idx] 中（WSARecv 直接 recv 到此缓冲）
            pr_parse_upstream_bytes(s, idx, s->pr_up_pend[idx], (size_t)nread_up);

            // 非阻塞发送回客户端（不回退阻塞，避免客户端写慢卡住引擎线程）
            int sent = 0, werr = 0;
            while (sent < nread_up) {
                int wc2 = send(cfd, s->pr_up_pend[idx] + sent, nread_up - sent, 0);
                if (wc2 > 0) { sent += wc2; continue; }
                if (wc2 < 0 && WSAGetLastError() == WSAEWOULDBLOCK) break;  // 写满，走延迟补发
                werr = 1; break;
            }
            if (werr) {
                // 客户端写错误：连接不可用，关闭两端
                if (ufd > 0) { closesocket(ufd); s->pr_fd_upstream[idx] = 0; }
                closesocket(cfd); s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                continue;
            }

            if (sent < nread_up) {
                // 客户端写慢：剩余数据已在 pr_up_pend[idx][sent..nread_up]，前移后投递 WSASend 继续补发
                int rem = nread_up - sent;
                if (rem > PR_PROXY_BUF_SZ) rem = PR_PROXY_BUF_SZ;
                if (sent > 0)
                    memmove(s->pr_up_pend[idx], s->pr_up_pend[idx] + sent, (size_t)rem);
                s->pr_up_pend_len[idx] = rem;
                pr_iocp_ctx_t* wctx = &((pr_iocp_ctx_t*)s->pr_iocp_cw_ctx)[idx];
                memset(wctx, 0, sizeof(*wctx));
                wctx->idx = idx;
                wctx->ev  = IOCP_EV_CLIENT_WRITE;
                wctx->sock = cfd;
                wctx->buf  = s->pr_up_pend[idx];
                wctx->buflen = (DWORD)rem;
                WSABUF wsb_w = {(ULONG)rem, s->pr_up_pend[idx]};
                DWORD wsent = 0, wflg = 0;
                WSASend(cfd, &wsb_w, 1, &wsent, wflg, (LPOVERLAPPED)wctx, NULL);
                // 补发期间不 post 上游 WSARecv，避免响应字节乱序
                continue;
            }

            // 响应完整且已全部转发给客户端 → 归还上游连接复用，关闭客户端
            if (s->pr_up_parse[idx] == PR_UP_DONE) {
                if (s->pr_up_key[idx] && s->pr_up_key[idx][0] != '\0') {
                    // 归还池（不关闭 socket；IOCP 无显式摘除 API，重关联时更新 key）
                    pool_put(s->up_pool, s->pr_up_key[idx], ufd);
                } else {
                    closesocket(ufd);   // 不参与池（如 tcp）直接关闭
                }
                s->pr_fd_upstream[idx] = 0;
                closesocket(cfd);
                s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                continue;
            }

            // 继续 post 上游 WSARecv
            if (ufd > 0 && (s->pr_state[idx] & PR_ST_ACTIVE)) {
                pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
                memset(uctx, 0, sizeof(*uctx));
                uctx->idx = idx;
                uctx->ev  = IOCP_EV_UPSTREAM_READ;
                uctx->sock = ufd;
                uctx->buf  = s->pr_up_pend[idx];
                uctx->buflen = PR_PROXY_BUF_SZ;
                WSABUF wsb_up2 = {PR_PROXY_BUF_SZ, s->pr_up_pend[idx]};
                DWORD flg_up2 = 0, dummy_up2 = 0;
                WSARecv(ufd, &wsb_up2, 1, &dummy_up2, &flg_up2, (LPOVERLAPPED)uctx, NULL);
            }
            continue;
        }

        else if (ctx->ev == IOCP_EV_CLIENT_WRITE) {
            // ---- 客户端 WSASend 完成 → 继续补发上游→客户端余量 ----
            SOCKET ufd = s->pr_fd_upstream[idx];
            int rem = s->pr_up_pend_len[idx];
            int sent = (int)bytes_trans;

            if (sent <= 0 || !s->pr_up_pend[idx]) {
                // 客户端写失败/关闭：清理两端
                s->pr_up_pend_len[idx] = 0;
                if (ufd > 0) { closesocket(ufd); s->pr_fd_upstream[idx] = 0; }
                closesocket(cfd); s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                continue;
            }
            if (sent < rem) {
                // 仍有剩余：前移后重新投递 WSASend
                memmove(s->pr_up_pend[idx], s->pr_up_pend[idx] + sent, (size_t)(rem - sent));
                s->pr_up_pend_len[idx] = rem - sent;
                pr_iocp_ctx_t* wctx2 = &((pr_iocp_ctx_t*)s->pr_iocp_cw_ctx)[idx];
                memset(wctx2, 0, sizeof(*wctx2));
                wctx2->idx = idx;
                wctx2->ev  = IOCP_EV_CLIENT_WRITE;
                wctx2->sock = cfd;
                wctx2->buf  = s->pr_up_pend[idx];
                wctx2->buflen = (DWORD)(rem - sent);
                WSABUF wsb_w2 = {(ULONG)(rem - sent), s->pr_up_pend[idx]};
                DWORD wsent2 = 0, wflg2 = 0;
                WSASend(cfd, &wsb_w2, 1, &wsent2, wflg2, (LPOVERLAPPED)wctx2, NULL);
                continue;
            }

            // 余量全部补发完成
            s->pr_up_pend_len[idx] = 0;
            // 响应完整 → 归还上游连接复用，关闭客户端（与上游读完成分支一致）
            if (s->pr_up_parse[idx] == PR_UP_DONE) {
                if (s->pr_up_key[idx] && s->pr_up_key[idx][0] != '\0') {
                    pool_put(s->up_pool, s->pr_up_key[idx], ufd);
                } else {
                    closesocket(ufd);
                }
                s->pr_fd_upstream[idx] = 0;
                closesocket(cfd);
                s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                continue;
            }
            // 响应未完整：继续 post 上游 WSARecv
            if (ufd > 0 && (s->pr_state[idx] & PR_ST_ACTIVE)) {
                pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
                memset(uctx, 0, sizeof(*uctx));
                uctx->idx = idx;
                uctx->ev  = IOCP_EV_UPSTREAM_READ;
                uctx->sock = ufd;
                uctx->buf  = s->pr_up_pend[idx];
                uctx->buflen = PR_PROXY_BUF_SZ;
                WSABUF wsb_up3 = {PR_PROXY_BUF_SZ, s->pr_up_pend[idx]};
                DWORD flg_up3 = 0, dummy_up3 = 0;
                WSARecv(ufd, &wsb_up3, 1, &dummy_up3, &flg_up3, (LPOVERLAPPED)uctx, NULL);
            }
            continue;
        }
        // 其他 ev 类型：忽略
    }

    // 清理所有活动连接
    for (int i = 0; i < cap; i++) {
        if (s->pr_state[i] & PR_ST_ACTIVE) {
            if (s->pr_fd_client[i] != INVALID_SOCKET) closesocket(s->pr_fd_client[i]);
            if (s->pr_fd_upstream[i] > 0) closesocket(s->pr_fd_upstream[i]);
            s->pr_state[i] = 0;
        }
        if (s->pr_hbuf[i])     free(s->pr_hbuf[i]);
        if (s->pr_hbuf_out[i]) free(s->pr_hbuf_out[i]);
        if (s->pr_up_key && s->pr_up_key[i]) free(s->pr_up_key[i]);
        if (s->pr_up_pend && s->pr_up_pend[i]) free(s->pr_up_pend[i]);
    }

cleanup_fail:
    // 销毁上游 keep-alive 连接池（关闭所有空闲连接）
    if (s->up_pool) { pool_destroy(s->up_pool); free(s->up_pool); s->up_pool = NULL; }
    // IOCP 清理：CloseHandle 会自动取消所有 pending I/O
    if (s->iocp) { CloseHandle(s->iocp); s->iocp = NULL; }
    if (s->pr_accept_client != INVALID_SOCKET) {
        closesocket(s->pr_accept_client);
        s->pr_accept_client = INVALID_SOCKET;
    }
    free(s->pr_iocp_client_ctx); s->pr_iocp_client_ctx = NULL;
    free(s->pr_iocp_up_ctx);     s->pr_iocp_up_ctx     = NULL;
    free(s->pr_iocp_cw_ctx);     s->pr_iocp_cw_ctx     = NULL;
    free(s->pr_fd_client);   s->pr_fd_client   = NULL;
    free(s->pr_fd_upstream); s->pr_fd_upstream = NULL;
    free(s->pr_route_idx);   s->pr_route_idx   = NULL;
    free(s->pr_state);       s->pr_state       = NULL;
    free(s->pr_hbuf);        s->pr_hbuf        = NULL;
    free(s->pr_hbuf_out);    s->pr_hbuf_out    = NULL;
    free(s->pr_hdr_len);     s->pr_hdr_len     = NULL;
    free(s->pr_remain);      s->pr_remain      = NULL;
    free(s->pr_free);        s->pr_free        = NULL;
    free(s->pr_up_parse);    s->pr_up_parse    = NULL;
    free(s->pr_up_cl);       s->pr_up_cl       = NULL;
    free(s->pr_chunk_rem);   s->pr_chunk_rem   = NULL;
    free(s->pr_chunk_state); s->pr_chunk_state = NULL;
    free(s->pr_up_hlen);     s->pr_up_hlen     = NULL;
    free(s->pr_req_is_head); s->pr_req_is_head = NULL;
    free(s->pr_up_key);      s->pr_up_key      = NULL;
    free(s->pr_up_pend);     s->pr_up_pend     = NULL;
    free(s->pr_up_pend_len); s->pr_up_pend_len = NULL;
    s->pr_cap = s->pr_count = 0;
}

// 引擎线程入口
static DWORD WINAPI nl_web_proxy_engine_thread(LPVOID arg) {
    nl_web_server_t* s = (nl_web_server_t*)arg;
    nl_web_proxy_engine(s, s->sock);
    return 0;
}

// 连接上游服务器（原实现），成功返回套接字，失败返回 INVALID_SOCKET。
static SOCKET nl_proxy_connect_upstream(const nl_web_route_t* route) {
    char host[512];
    int port = 0;
    nl_proxy_protocol_t proto = route->protocol;

    char upstream_copy[512];
    snprintf(upstream_copy, sizeof(upstream_copy), "%s", route->upstream);
    char* colon = strrchr(upstream_copy, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
        snprintf(host, sizeof(host), "%s", upstream_copy);
    } else {
        port = (proto == NL_PROXY_HTTPS) ? 443 : 80;
        snprintf(host, sizeof(host), "%s", upstream_copy);
    }
    if (port <= 0) port = (proto == NL_PROXY_HTTPS) ? 443 : 80;

    struct sockaddr_in up_addr;
    memset(&up_addr, 0, sizeof(up_addr));
    up_addr.sin_family = AF_INET;
    up_addr.sin_port = htons((u_short)port);

    struct hostent* he = gethostbyname(host);
    if (he && he->h_addr_list[0]) {
        memcpy(&up_addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    } else {
        // 回退：inet_pton 解析 IP 字面量；再失败则设为 127.0.0.1（本地回环场景）
        if (inet_pton(AF_INET, host, &up_addr.sin_addr) != 1) {
            up_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        }
    }

    SOCKET up_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (up_sock == INVALID_SOCKET) return INVALID_SOCKET;

    DWORD tv_ms = 5000;
    setsockopt(up_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
    setsockopt(up_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_ms, sizeof(tv_ms));

    if (connect(up_sock, (struct sockaddr*)&up_addr, sizeof(up_addr)) == SOCKET_ERROR) {
        closesocket(up_sock);
        return INVALID_SOCKET;
    }

    // 完成反代：HTTPS 上游需要 TLS 握手。优先通过扩展系统动态发现 TLS 握手钩子，
    // 取代原先依赖 #ifdef NL_HTTPS_ENABLE 的编译期死代码（核心库默认不定义该宏）。
    if (proto == NL_PROXY_HTTPS) {
        int tls_ok = 0;
#ifdef NL_HTTPS_ENABLE
        extern int nl_tls_handshake(int sock, const char* host);
        tls_ok = nl_tls_handshake((int)up_sock, host);
#else
        extern void* nl_extension_get_func_by_id(const char* ext_id, const char* func_name);
        /* 尝试取 HTTPS 扩展的 TLS 握手钩子（如扩展导出 nl_tls_handshake） */
        void* handshake_fn = nl_extension_get_func_by_id("https", "nl_tls_handshake");
        if (handshake_fn) {
            typedef int (*nl_tls_handshake_fn)(int, const char*);
            tls_ok = ((nl_tls_handshake_fn)handshake_fn)((int)up_sock, host);
        }
#endif
        if (!tls_ok) {
            windows_log(NL_LOG_WARN, "proxy: TLS not available for upstream %s, falling back to plaintext", host);
        }
    }

    return up_sock;
}

static void nl_handle_client(nl_web_server_t* server, SOCKET client) {
    char buffer[8192];
    int received = recv(client, buffer, sizeof(buffer) - 1, 0);
    
    if (received <= 0) {
        closesocket(client);
        return;
    }
    
    buffer[received] = '\0';

    /* 取长补短：核心库旧路径原先用 sscanf 粗糙解析（无法提取请求头），
     * 此处改用与优化层一致的"首行三段 + 请求头"结构化解析，得到
     * method/path/version 及完整请求头，同时保留核心库特有的
     * 路由表/反代/静态文件优势。解析失败则返回 400。 */
    char method[16], path[4096], protocol[32];
    method[0] = path[0] = protocol[0] = '\0';
    {
        char* line_end = memchr(buffer, '\n', (size_t)received);
        if (!line_end || line_end == buffer) {
            send_http_error(client, 400, "Bad Request");
            closesocket(client);
            return;
        }
        *line_end = '\0';
        char* sp1 = strchr(buffer, ' ');
        if (!sp1 || sp1 == buffer) {
            send_http_error(client, 400, "Bad Request");
            closesocket(client);
            return;
        }
        *sp1 = '\0';
        char* sp2 = strchr(sp1 + 1, ' ');
        if (!sp2) {
            send_http_error(client, 400, "Bad Request");
            closesocket(client);
            return;
        }
        size_t mlen = sp1 - buffer;
        size_t plen = sp2 - (sp1 + 1);
        size_t vlen = (size_t)(line_end - (sp2 + 1));
        if (mlen == 0 || mlen >= sizeof(method) || plen == 0 || plen >= sizeof(path) ||
            vlen == 0 || vlen >= sizeof(protocol)) {
            send_http_error(client, 400, "Bad Request");
            closesocket(client);
            return;
        }
        memcpy(method, buffer, mlen);
        method[mlen] = '\0';
        memcpy(path, sp1 + 1, plen);
        path[plen] = '\0';
        memcpy(protocol, sp2 + 1, vlen);
        protocol[vlen] = '\0';
    }

    EnterCriticalSection(&server->mutex);
    // 使用 hash 表 O(1) 查找路由；若 hash 未初始化则回退到线性链表
    nl_web_route_t* route = nl_route_hash_lookup_simple(server, path);
    if (!route) {
        for (nl_web_route_t* r = server->routes; r; r = r->next) {
            if (strcmp(r->path, path) == 0) { route = r; break; }
        }
    }
    if (route) {
        if (route->type == NL_ROUTE_TYPE_REDIRECT) {
                // Send redirect (301 or 302 based on server setting)
                int redirect_code = server->redirect_type;
                const char* redirect_text = (redirect_code == 301) ? "Moved Permanently" : "Found";
                LeaveCriticalSection(&server->mutex);
                char redirect_response[1024];
                snprintf(redirect_response, sizeof(redirect_response),
                    "HTTP/1.1 %d %s\r\n"
                    "Location: %s\r\n"
                    "Content-Length: 0\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    redirect_code, redirect_text, route->redirect_url);
                (void)send(client, redirect_response, (int)strlen(redirect_response), 0);
                closesocket(client);
                return;
            }
            else if (route->type == NL_ROUTE_TYPE_FILE) {
                // Hot reload: read file content on each request
                LeaveCriticalSection(&server->mutex);
                
                FILE* fp = fopen(route->file_path, "rb");
                if (!fp) {
                    send_http_error(client, 404, "File Not Found");
                    closesocket(client);
                    return;
                }
                
                fseek(fp, 0, SEEK_END);
                long file_size = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                
                if (file_size <= 0 || file_size > 10 * 1024 * 1024) {
                    fclose(fp);
                    send_http_error(client, 500, "File Too Large");
                    closesocket(client);
                    return;
                }
                
                char* file_content = (char*)malloc(file_size + 1);
                if (!file_content) {
                    fclose(fp);
                    send_http_error(client, 500, "Memory Error");
                    closesocket(client);
                    return;
                }
                
                size_t read_size = fread(file_content, 1, file_size, fp);
                fclose(fp);
                file_content[read_size] = '\0';
                
                send_http_response(client, route->content_type, file_content, read_size);
                free(file_content);
                closesocket(client);
                return;
            }
            else if (route->type == NL_ROUTE_TYPE_PROXY) {
                // 反向代理：连接上游并双向透传
                LeaveCriticalSection(&server->mutex);

                SOCKET up_sock = nl_proxy_connect_upstream(route);
                if (up_sock == INVALID_SOCKET) {
                    send_http_error(client, 502, "Bad Gateway");
                    closesocket(client);
                    return;
                }

                // 高压优化：代理双向透传缓冲堆分配一次复用，避免 16KB 栈占用。
                char* proxy_buf = (char*)malloc(65536);
                if (!proxy_buf) {
                    closesocket(up_sock);
                    send_http_error(client, 500, "Memory Error");
                    closesocket(client);
                    return;
                }

                // 高压优化：TCP 上游加 5s 超时，避免上游无响应时 worker 线程被无限占用。
                DWORD up_tv_ms = 5000;
                setsockopt(up_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&up_tv_ms, sizeof(up_tv_ms));
                setsockopt(up_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&up_tv_ms, sizeof(up_tv_ms));

                if (route->protocol == NL_PROXY_TCP) {
                    /* 完成反代：纯字节透传（TCP 语义），不构造 HTTP 请求行。 */
                    int sent = send(up_sock, buffer, (int)received, 0);
                    if (sent <= 0) {
                        free(proxy_buf);
                        closesocket(up_sock);
                        send_http_error(client, 502, "Bad Gateway");
                        closesocket(client);
                        return;
                    }
                    int n;
                    while ((n = recv(up_sock, proxy_buf, sizeof(proxy_buf), 0)) > 0) {
                        if (send(client, proxy_buf, n, 0) <= 0) break;
                    }
                    free(proxy_buf);
                    closesocket(up_sock);
                    closesocket(client);
                    return;
                }

                // HTTP/HTTPS：重组 request line + 重写 Host/Connection + 透传
                // X-Forwarded-For，并原样保留客户端原始请求头与请求体。
                char upstream_req_full[12288];
                int total = snprintf(upstream_req_full, sizeof(upstream_req_full),
                    "%s %s %s\r\nHost: %s\r\nX-Forwarded-For: 127.0.0.1\r\nConnection: close\r\n",
                    method, path, protocol, route->upstream);
                /* buffer 已被 memchr/strchr 改写（首行以 \0 收尾），
                 * 必须用 memchr 重新定位首行换行符，取剩余部分即原始
                 * 请求头与 body，避免 strstr 命中被破坏的 \r\n。 */
                const char* line_term = memchr(buffer, '\n', (size_t)received);
                if (line_term) {
                    const char* rest = line_term + 1;
                    size_t rest_len = 0;
                    while (rest[rest_len] &&
                           rest_len + total < (size_t)sizeof(upstream_req_full))
                        rest_len++;
                    if (rest_len > 0) {
                        memcpy(upstream_req_full + total, rest, rest_len);
                        total += (int)rest_len;
                    }
                }
                int sent2 = send(up_sock, upstream_req_full, total, 0);
                if (sent2 <= 0) {
                    free(proxy_buf);
                    closesocket(up_sock);
                    send_http_error(client, 502, "Bad Gateway");
                    closesocket(client);
                    return;
                }

                int n;
                while ((n = recv(up_sock, proxy_buf, sizeof(proxy_buf), 0)) > 0) {
                    if (send(client, proxy_buf, n, 0) <= 0) break;
                }
                free(proxy_buf);
                closesocket(up_sock);
                closesocket(client);
                return;
            }
            else {
                // Static content
                LeaveCriticalSection(&server->mutex);
                send_http_response(client, route->content_type, route->content, route->content_size);
                closesocket(client);
                return;
            }
    }
    LeaveCriticalSection(&server->mutex);
    
    send_http_error(client, 404, "Not Found");
    closesocket(client);
}

static DWORD WINAPI nl_worker_thread(LPVOID arg) {
    nl_web_server_t* server = (nl_web_server_t*)arg;
    while (server->running) {
        SOCKET client = accept(server->sock, NULL, NULL);
        if (client == INVALID_SOCKET) {
            Sleep(10);
            continue;
        }
        nl_handle_client(server, client);
    }
    return 0;
}

static DWORD WINAPI web_server_thread(LPVOID arg) {
    nl_web_server_t* server = (nl_web_server_t*)arg;

    // 启动 worker 线程池
    for (int i = 0; i < server->worker_count; i++) {
        server->workers[i] = CreateThread(NULL, 0, nl_worker_thread, server, 0, NULL);
        if (!server->workers[i]) {
            server->worker_count = i;
            break;
        }
    }
    server->worker_active = 1;

    // 若 worker 全部启动成功，则 accept 线程退出，让 worker 承担所有连接
    if (server->worker_count > 0) {
        for (int i = 0; i < server->worker_count; i++) {
            if (server->workers && server->workers[i]) {
                WaitForSingleObject(server->workers[i], INFINITE);
                CloseHandle(server->workers[i]);
                server->workers[i] = NULL;
            }
        }
        server->worker_active = 0;
        return 0;
    }

    // 回退：worker 未启动（启动失败或 worker_count=0），accept 线程自己承担主 accept 循环
    while (server->running) {
        SOCKET client = accept(server->sock, NULL, NULL);
        if (client == INVALID_SOCKET) {
            Sleep(10);
            continue;
        }
        nl_handle_client(server, client);
    }

    return 0;
}

nl_web_server_t* nl_web_create(int port) {
    init_web_mutex();
    
    EnterCriticalSection(&g_web_servers_mutex);
    struct nl_web_server* existing = g_web_servers;
    while (existing) {
        if (existing->port == port) {
            LeaveCriticalSection(&g_web_servers_mutex);
            return existing;
        }
        existing = existing->next;
    }
    
    nl_web_server_t* server = (nl_web_server_t*)calloc(1, sizeof(nl_web_server_t));
    if (!server) {
        LeaveCriticalSection(&g_web_servers_mutex);
        return NULL;
    }
    
    server->port = port;
    server->routes = NULL;
    InitializeCriticalSection(&server->mutex);
    nl_route_hash_init(server);
    strncpy(server->encoding, "UTF-8", sizeof(server->encoding) - 1);
    server->redirect_type = NL_REDIRECT_TEMPORARY;  // Default: 302
    server->next = g_web_servers;
    g_web_servers = server;
    LeaveCriticalSection(&g_web_servers_mutex);
    
    // 懒启动：create 仅注册并初始化，不绑定 socket、不起线程；
    // 由调用方显式调用 nl_web_start 启动，降低服务启动阻塞时间。
    return server;
}

void nl_web_destroy(nl_web_server_t* server) {
    if (!server) return;
    
    init_web_mutex();
    EnterCriticalSection(&g_web_servers_mutex);
    struct nl_web_server** pp = &g_web_servers;
    while (*pp) {
        if (*pp == server) {
            *pp = server->next;
            break;
        }
        pp = &(*pp)->next;
    }
    LeaveCriticalSection(&g_web_servers_mutex);
    
    if (server->running) {
        nl_web_stop(server);
    }
    
    EnterCriticalSection(&server->mutex);
    nl_web_route_t* route = server->routes;
    while (route) {
        nl_web_route_t* next = route->next;
        if (route->content) free(route->content);
        free(route);
        route = next;
    }
    LeaveCriticalSection(&server->mutex);
    
    DeleteCriticalSection(&server->mutex);
    free(server->route_hash);
    free(server->workers);
    // 代理引擎 pr ring（引擎未启动时全为 NULL，free(NULL) 安全）
    // IOCP 完成端口句柄（如果引擎线程还在跑，CloseHandle 后 pending I/O 会被取消）
    if (server->iocp) { CloseHandle(server->iocp); server->iocp = NULL; }
    if (server->pr_accept_client != INVALID_SOCKET) {
        closesocket(server->pr_accept_client);
        server->pr_accept_client = INVALID_SOCKET;
    }
    free(server->pr_iocp_client_ctx); server->pr_iocp_client_ctx = NULL;
    free(server->pr_iocp_up_ctx);     server->pr_iocp_up_ctx     = NULL;
    free(server->pr_iocp_cw_ctx);     server->pr_iocp_cw_ctx     = NULL;
    free(server->pr_fd_client);   server->pr_fd_client   = NULL;
    free(server->pr_fd_upstream); server->pr_fd_upstream = NULL;
    free(server->pr_route_idx);   server->pr_route_idx   = NULL;
    free(server->pr_state);       server->pr_state       = NULL;
    free(server->pr_hbuf);        server->pr_hbuf        = NULL;
    free(server->pr_hbuf_out);    server->pr_hbuf_out    = NULL;
    free(server->pr_hdr_len);     server->pr_hdr_len     = NULL;
    free(server->pr_remain);      server->pr_remain      = NULL;
    free(server->pr_free);        server->pr_free        = NULL;
    free(server->pr_up_parse);    server->pr_up_parse    = NULL;
    free(server->pr_up_cl);       server->pr_up_cl       = NULL;
    free(server->pr_chunk_rem);   server->pr_chunk_rem   = NULL;
    free(server->pr_chunk_state); server->pr_chunk_state = NULL;
    free(server->pr_up_hlen);     server->pr_up_hlen     = NULL;
    free(server->pr_req_is_head); server->pr_req_is_head = NULL;
    free(server->pr_up_key);      server->pr_up_key      = NULL;
    free(server->pr_up_pend);     server->pr_up_pend     = NULL;
    free(server->pr_up_pend_len); server->pr_up_pend_len = NULL;
    if (server->up_pool) { pool_destroy(server->up_pool); free(server->up_pool); server->up_pool = NULL; }
    free(server);
}

int nl_web_start(nl_web_server_t* server) {
    if (!server || server->running) return NL_EINVAL;
    
    if (init_winsock() != 0) return NL_ERROR;
    
    server->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->sock == INVALID_SOCKET) return NL_ERROR;
    
    int opt = 1;
    setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)server->port);
    
    if (bind(server->sock, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(server->sock);
        return NL_ERROR;
    }
    
    if (listen(server->sock, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server->sock);
        return NL_ERROR;
    }
    
    server->running = 1;
    // 数据驱动检测：是否存在反向代理路由，有则起 select 多路复用引擎（单线程泵 N 连接），
    // 否则回退 worker 池。
    int has_proxy = 0;
    EnterCriticalSection(&server->mutex);
    for (nl_web_route_t* r = server->routes; r; r = r->next)
        if (r->type == NL_ROUTE_TYPE_PROXY) { has_proxy = 1; break; }
    LeaveCriticalSection(&server->mutex);
    if (has_proxy) {
        server->thread = CreateThread(NULL, 0, nl_web_proxy_engine_thread, server, 0, NULL);
        if (!server->thread) {
            server->running = 0;
            closesocket(server->sock);
            return NL_ERROR;
        }
        windows_log(NL_LOG_INFO, "Web server (proxy engine) started on port %d", server->port);
        return NL_OK;
    }
    // 全动态化：worker 池容量按需分配。未显式设置时用 WEB_WORKER_DEFAULT。
    // 保留 64 上限（线程栈资源上限）防止误配置；worker_count 为 0 时由
    // web_server_thread 回退到 accept 线程自处理连接。
    int target = server->worker_count > 0 ? server->worker_count : WEB_WORKER_DEFAULT;
    if (target > 64) target = 64;
    if (server->worker_capacity < target) {
        HANDLE* new_workers = (HANDLE*)realloc(server->workers,
                                               (size_t)target * sizeof(HANDLE));
        if (!new_workers) {
            server->running = 0;
            closesocket(server->sock);
            return NL_ERROR;
        }
        server->workers = new_workers;
        server->worker_capacity = target;
    }
    server->worker_count = target;
    server->thread = CreateThread(NULL, 0, web_server_thread, server, 0, NULL);
    if (!server->thread) {
        server->running = 0;
        closesocket(server->sock);
        return NL_ERROR;
    }
    
    windows_log(NL_LOG_INFO, "Web server started on port %d", server->port);
    return NL_OK;
}

// 全动态化：运行时调整 worker 池目标容量（0 = 关闭池，回退 accept 线程自处理）。
// 在 nl_web_start 前调用生效；已 start 的服务器下次重启后生效。
int nl_web_set_worker_count(nl_web_server_t* server, int target) {
    if (!server) return NL_ERROR;
    if (target < 0) target = 0;
    if (target > 64) target = 64;
    server->worker_count = target;
    return NL_OK;
}

void nl_web_stop_by_port(int port) {
    init_web_mutex();
    EnterCriticalSection(&g_web_servers_mutex);
    struct nl_web_server* server = g_web_servers;
    while (server) {
        if (server->port == port) {
            LeaveCriticalSection(&g_web_servers_mutex);
            nl_web_destroy(server);
            return;
        }
        server = server->next;
    }
    LeaveCriticalSection(&g_web_servers_mutex);
}

static void cleanup_all_web_servers(void) {
    init_web_mutex();
    EnterCriticalSection(&g_web_servers_mutex);
    while (g_web_servers) {
        struct nl_web_server* server = g_web_servers;
        g_web_servers = server->next;
        
        LeaveCriticalSection(&g_web_servers_mutex);
        
        if (server->running) {
            nl_web_stop(server);
        }
        
        EnterCriticalSection(&server->mutex);
        nl_web_route_t* route = server->routes;
        while (route) {
            nl_web_route_t* next = route->next;
            if (route->content) free(route->content);
            free(route);
            route = next;
        }
        LeaveCriticalSection(&server->mutex);
        
        DeleteCriticalSection(&server->mutex);
        free(server->route_hash);
        free(server->workers);
        free(server);
        
        EnterCriticalSection(&g_web_servers_mutex);
    }
    LeaveCriticalSection(&g_web_servers_mutex);
    
    if (g_mutex_initialized) {
        DeleteCriticalSection(&g_web_servers_mutex);
        g_mutex_initialized = 0;
    }
}

void nl_web_set_auto_cleanup(int enable) {
    if (enable && !g_auto_cleanup_enabled) {
        g_auto_cleanup_enabled = 1;
        atexit(cleanup_all_web_servers);
    }
    g_auto_cleanup_enabled = enable;
}

void nl_web_stop(nl_web_server_t* server) {
    if (!server || !server->running) return;
    
    InterlockedExchange(&server->running, 0);
    
    // Close the socket first to unblock accept() in the thread
    if (server->sock != INVALID_SOCKET) {
        closesocket(server->sock);
        server->sock = INVALID_SOCKET;
    }
    
    if (server->thread) {
        WaitForSingleObject(server->thread, 5000);
        if (server->thread) {
            // Thread didn't exit in time, force terminate
            TerminateThread(server->thread, 0);
            CloseHandle(server->thread);
            server->thread = NULL;
        }
    }
    
    // 兜底：代理引擎 pr ring（正常退出时引擎已自清理；TerminateThread 路径
    // 可能未清理，此处 close 残留连接 + 释放数组，避免泄漏）
    if (server->pr_fd_client || server->pr_fd_upstream || server->pr_state) {
        int p_cap = server->pr_cap;
        for (int i = 0; i < p_cap; i++) {
            if (server->pr_fd_client) {
                if (server->pr_fd_client[i] != INVALID_SOCKET) closesocket(server->pr_fd_client[i]);
            }
            if (server->pr_fd_upstream) {
                if (server->pr_fd_upstream[i] > 0) closesocket(server->pr_fd_upstream[i]);
            }
            if (server->pr_hbuf) { free(server->pr_hbuf[i]); server->pr_hbuf[i] = NULL; }
            if (server->pr_hbuf_out) { free(server->pr_hbuf_out[i]); server->pr_hbuf_out[i] = NULL; }
            if (server->pr_up_key && server->pr_up_key[i]) { free(server->pr_up_key[i]); server->pr_up_key[i] = NULL; }
            if (server->pr_up_pend && server->pr_up_pend[i]) { free(server->pr_up_pend[i]); server->pr_up_pend[i] = NULL; }
        }
        free(server->pr_fd_client);   server->pr_fd_client   = NULL;
        free(server->pr_fd_upstream); server->pr_fd_upstream = NULL;
        free(server->pr_route_idx);   server->pr_route_idx   = NULL;
        free(server->pr_state);       server->pr_state       = NULL;
        free(server->pr_hbuf);        server->pr_hbuf        = NULL;
        free(server->pr_hbuf_out);    server->pr_hbuf_out    = NULL;
        free(server->pr_hdr_len);     server->pr_hdr_len     = NULL;
        free(server->pr_remain);      server->pr_remain      = NULL;
        free(server->pr_free);        server->pr_free        = NULL;
        free(server->pr_up_parse);    server->pr_up_parse    = NULL;
        free(server->pr_up_cl);       server->pr_up_cl       = NULL;
        free(server->pr_chunk_rem);   server->pr_chunk_rem   = NULL;
        free(server->pr_chunk_state); server->pr_chunk_state = NULL;
        free(server->pr_up_hlen);     server->pr_up_hlen     = NULL;
        free(server->pr_req_is_head); server->pr_req_is_head = NULL;
        free(server->pr_up_key);      server->pr_up_key      = NULL;
        free(server->pr_up_pend);     server->pr_up_pend     = NULL;
        free(server->pr_up_pend_len); server->pr_up_pend_len = NULL;
        free(server->pr_iocp_cw_ctx); server->pr_iocp_cw_ctx = NULL;
        server->pr_cap = server->pr_count = 0;
    }
    // 销毁上游 keep-alive 连接池（兜底：正常退出时引擎已自清理）
    if (server->up_pool) { pool_destroy(server->up_pool); free(server->up_pool); server->up_pool = NULL; }
    
    // 清理 worker 线程（兜底：正常情况下 web_server_thread 内已 join）
    for (int i = 0; i < server->worker_count; i++) {
        if (server->workers && server->workers[i]) {
            CloseHandle(server->workers[i]);
            server->workers[i] = NULL;
        }
    }
    
    windows_log(NL_LOG_INFO, "Web server stopped");
}

static void add_web_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type) {
    if (!server || !path || !content) return;
    // content_type 为空时给出默认类型，避免后续 strncpy 解引用空指针
    const char* ctype = content_type ? content_type : "application/octet-stream";
    
    EnterCriticalSection(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (route) {
        strncpy(route->path, path, sizeof(route->path) - 1);
        route->path[sizeof(route->path) - 1] = '\0';
        route->content_size = strlen(content);
        route->content = (char*)malloc(route->content_size + 1);
        if (!route->content) {
            free(route);
            LeaveCriticalSection(&server->mutex);
            return;
        }
        // BUG-304: 用 snprintf 取代 strncpy，根除 -Wstringop-truncation
        snprintf(route->content, route->content_size + 1, "%s", content);
        snprintf(route->content_type, sizeof(route->content_type), "%s", ctype);
        route->type = NL_ROUTE_TYPE_CONTENT;  // Default: static content
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
    }
    LeaveCriticalSection(&server->mutex);
}

static int is_url(const char* str) {
    if (!str) return 0;
    size_t len = strlen(str);
    if (len < 7) return 0;
    if ((strncmp(str, "http://", 7) == 0) || (strncmp(str, "https://", 8) == 0)) {
        return 1;
    }
    return 0;
}

static int is_file_path(const char* str, char* abs_path, size_t abs_path_size) {
    if (!str || !abs_path) return 0;
    
    char full_path[512];
    const char* src;
    if (_fullpath(full_path, str, sizeof(full_path))) {
        src = full_path;
    } else {
        src = str;
    }
    // BUG-304: 用 memcpy + strlen 取代 strncpy，避免 -Wstringop-truncation
    // （strncpy 在源串 >= N 时填满缓冲但不补终止符，GCC 推断截断）
    size_t slen = strlen(src);
    if (slen >= abs_path_size) {
        slen = abs_path_size - 1;
    }
    memcpy(abs_path, src, slen);
    abs_path[slen] = '\0';
    
    FILE* fp = fopen(abs_path, "rb");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
}

static void add_web_route_smart(nl_web_server_t* server, const char* path, const char* content, const char* content_type) {
    if (!server || !path || !content) return;
    
    // Priority 1: Check for URL (302/301 redirect)
    if (is_url(content)) {
        EnterCriticalSection(&server->mutex);
        nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
        if (route) {
            strncpy(route->path, path, sizeof(route->path) - 1);
            route->path[sizeof(route->path) - 1] = '\0';
            strncpy(route->redirect_url, content, sizeof(route->redirect_url) - 1);
            route->redirect_url[sizeof(route->redirect_url) - 1] = '\0';
            strncpy(route->content_type, "text/html", sizeof(route->content_type) - 1);
            route->type = NL_ROUTE_TYPE_REDIRECT;
            route->next = server->routes;
            server->routes = route;
            nl_route_hash_insert_simple(server, route);
        }
        LeaveCriticalSection(&server->mutex);
        windows_log(NL_LOG_INFO, "Added redirect: %s -> %s", path, content);
        return;
    }
    
    // Priority 2: Check for file path (hot reload)
    char abs_path[512];
    if (is_file_path(content, abs_path, sizeof(abs_path))) {
        EnterCriticalSection(&server->mutex);
        nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
        if (route) {
            strncpy(route->path, path, sizeof(route->path) - 1);
            route->path[sizeof(route->path) - 1] = '\0';
            strncpy(route->file_path, abs_path, sizeof(route->file_path) - 1);
            route->file_path[sizeof(route->file_path) - 1] = '\0';
            strncpy(route->content_type, content_type, sizeof(route->content_type) - 1);
            route->type = NL_ROUTE_TYPE_FILE;
            route->next = server->routes;
            server->routes = route;
            nl_route_hash_insert_simple(server, route);
        }
        LeaveCriticalSection(&server->mutex);
        windows_log(NL_LOG_INFO, "Added file route: %s -> %s (hot reload)", path, abs_path);
        return;
    }
    
    // Priority 3: Static content
    add_web_route(server, path, content, content_type);
}

// 解析 upstream 字符串为 host + port + protocol（支持 scheme 前缀或默认端口）
// 返回 0 成功，-1 失败。host/port 由调用方提供足够缓冲。
static int nl_parse_upstream(const char* upstream, char* host, size_t host_size,
                             int* port_out, nl_proxy_protocol_t* proto_out) {
    if (!upstream || upstream[0] == '\0') return -1;

    const char* p = upstream;
    nl_proxy_protocol_t proto = NL_PROXY_HTTP;

    if (strncmp(p, "https://", 8) == 0) {
        proto = NL_PROXY_HTTPS;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        proto = NL_PROXY_HTTP;
        p += 7;
    } else if (strncmp(p, "tcp://", 6) == 0) {
        proto = NL_PROXY_TCP;
        p += 6;
    }

    // 分离 host 与 port
    char host_port[512];
    size_t len = strlen(p);
    if (len >= sizeof(host_port)) len = sizeof(host_port) - 1;
    memcpy(host_port, p, len);
    host_port[len] = '\0';

    int port = 0;
    char* colon = strrchr(host_port, ':');
    char* host_end = host_port;
    if (colon && colon != host_port) {
        *colon = '\0';
        host_end = host_port;
        port = atoi(colon + 1);
        if (port <= 0) port = 0;
    }

    if (port == 0) {
        // 未显式指定端口：按协议取默认端口
        if (proto == NL_PROXY_HTTPS) port = 443;
        else port = 80;
    }

    snprintf(host, host_size, "%s", host_end);
    *port_out = port;
    *proto_out = proto;
    return 0;
}

// 数据驱动反向代理：将 path 路径的请求转发至上游（http/https/tcp）
int nl_web_add_proxy(nl_web_server_t* server, const char* path, const char* upstream) {
    if (!server || !path || !upstream) return NL_EINVAL;

    char host[512];
    int port = 0;
    nl_proxy_protocol_t proto = NL_PROXY_HTTP;
    if (nl_parse_upstream(upstream, host, sizeof(host), &port, &proto) != 0) {
        return NL_EINVAL;
    }

    EnterCriticalSection(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (route) {
        strncpy(route->path, path, sizeof(route->path) - 1);
        route->path[sizeof(route->path) - 1] = '\0';
        // 上游地址统一存 host:port（不带 scheme），protocol 单独存放
        snprintf(route->upstream, sizeof(route->upstream), "%s:%d", host, port);
        route->protocol = proto;
        route->type = NL_ROUTE_TYPE_PROXY;
        route->content = NULL;
        route->content_size = 0;
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
        LeaveCriticalSection(&server->mutex);
        windows_log(NL_LOG_INFO, "Added proxy route: %s -> %s (%d)", path, route->upstream, port);
        return NL_OK;
    }
    LeaveCriticalSection(&server->mutex);
    return NL_ENOMEM;
}

// Runtime route management APIs (v2.2.2)

int nl_web_add_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type) {
    if (!server || !path || !content) return NL_EINVAL;
    
    EnterCriticalSection(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (route) {
        strncpy(route->path, path, sizeof(route->path) - 1);
        route->path[sizeof(route->path) - 1] = '\0';
        route->content_size = strlen(content);
        route->content = (char*)malloc(route->content_size + 1);
        if (!route->content) {
            free(route);
            LeaveCriticalSection(&server->mutex);
            return NL_ENOMEM;
        }
        // BUG-304: 用 snprintf 取代 strncpy，根除 -Wstringop-truncation
        snprintf(route->content, route->content_size + 1, "%s", content);
        snprintf(route->content_type, sizeof(route->content_type), "%s", content_type);
        route->type = NL_ROUTE_TYPE_CONTENT;
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
        windows_log(NL_LOG_INFO, "Added route: %s", path);
        LeaveCriticalSection(&server->mutex);
        return NL_OK;
    }
    LeaveCriticalSection(&server->mutex);
    return NL_ENOMEM;
}

int nl_web_remove_route(nl_web_server_t* server, const char* path) {
    if (!server || !path) return NL_EINVAL;
    
    EnterCriticalSection(&server->mutex);
    nl_web_route_t** pp = &server->routes;
    while (*pp) {
        if (strcmp((*pp)->path, path) == 0) {
            nl_web_route_t* target = *pp;
            *pp = target->next;
            if (target->content) free(target->content);
            free(target);
            // 路由已删除，重建 hash 表以剔除指向已释放 route 的指针
            nl_route_hash_rebuild(server);
            LeaveCriticalSection(&server->mutex);
            windows_log(NL_LOG_INFO, "Removed route: %s", path);
            return NL_OK;
        }
        pp = &(*pp)->next;
    }
    LeaveCriticalSection(&server->mutex);
    return NL_ENOENT;
}

int nl_web_get_route_count(nl_web_server_t* server) {
    if (!server) return 0;
    
    EnterCriticalSection(&server->mutex);
    int count = 0;
    nl_web_route_t* route = server->routes;
    while (route) {
        count++;
        route = route->next;
    }
    LeaveCriticalSection(&server->mutex);
    return count;
}

int nl_web_list_routes(nl_web_server_t* server, char** paths, int max_paths) {
    if (!server || !paths) return NL_EINVAL;
    
    EnterCriticalSection(&server->mutex);
    int count = 0;
    nl_web_route_t* route = server->routes;
    while (route) {
        if (count < max_paths && paths[count]) {
            strncpy(paths[count], route->path, 256);
            paths[count][255] = '\0';
        }
        count++;
        route = route->next;
    }
    LeaveCriticalSection(&server->mutex);
    return count;
}

int nl_web_update_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type) {
    if (!server || !path || !content) return NL_EINVAL;
    
    EnterCriticalSection(&server->mutex);
    nl_web_route_t* route = server->routes;
    while (route) {
        if (strcmp(route->path, path) == 0) {
            if (route->type != NL_ROUTE_TYPE_REDIRECT && route->type != NL_ROUTE_TYPE_FILE) {
                if (route->content) free(route->content);
                route->content_size = strlen(content);
                route->content = (char*)malloc(route->content_size + 1);
                if (!route->content) {
                    LeaveCriticalSection(&server->mutex);
                    return NL_ERROR;
                }
                // BUG-304: 用 snprintf 取代 strncpy，根除 -Wstringop-truncation
                snprintf(route->content, route->content_size + 1, "%s", content);
                if (content_type) {
                    snprintf(route->content_type, sizeof(route->content_type), "%s", content_type);
                }
            }
            LeaveCriticalSection(&server->mutex);
            windows_log(NL_LOG_INFO, "Updated route: %s", path);
            return NL_OK;
        }
        route = route->next;
    }
    LeaveCriticalSection(&server->mutex);
    return NL_ENOENT;
}

static int charset_module_loaded = 0;

static void charset_module_init(void) {
    if (!charset_module_loaded) {
        charset_module_loaded = 1;
    }
}

static void charset_module_cleanup(void) {
    if (charset_module_loaded) {
        charset_module_loaded = 0;
    }
}

static int strcasecmp_n(const char* str1, const char* str2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c1 = str1[i];
        char c2 = str2[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        if (c1 == '\0') return 0;
    }
    return 0;
}

static const char* find_charset_case_insensitive(const char* html) {
    const char* p = html;
    while ((p = strstr(p, "charset")) != NULL) {
        if (p > html && *(p - 1) != '<') {
            p += 7;
            continue;
        }
        
        const char* eq = strchr(p, '=');
        if (eq) {
            return eq;
        }
        p += 7;
    }
    return NULL;
}

static const char* find_tag_case_insensitive(const char* html, const char* tag) {
    const char* p = html;
    size_t tag_len = strlen(tag);
    
    while ((p = strstr(p, "<")) != NULL) {
        p++;
        if (strcasecmp_n(p, tag, tag_len) == 0) {
            char next = p[tag_len];
            if (next == '>' || next == ' ' || next == '\t' || next == '\n' || next == '\r') {
                return p - 1;
            }
        }
        p++;
    }
    return NULL;
}

static char* add_charset_if_missing(nl_web_server_t* server, const char* html) {
    charset_module_init();
    
    if (!server || !html) {
        charset_module_cleanup();
        char* empty = (char*)malloc(1);
        if (empty) *empty = '\0';
        return empty;
    }
    
    const char* encoding = server->encoding;
    if (strlen(encoding) == 0) {
        charset_module_cleanup();
        char* copy = (char*)malloc(strlen(html) + 1);
        if (copy) strncpy(copy, html, strlen(html) + 1);
        return copy;
    }
    
    if (find_charset_case_insensitive(html)) {
        charset_module_cleanup();
        char* copy = (char*)malloc(strlen(html) + 1);
        if (copy) strncpy(copy, html, strlen(html) + 1);
        return copy;
    }
    
    const char* insert_pos = NULL;
    size_t insert_offset = 0;
    
    const char* head_start = find_tag_case_insensitive(html, "head");
    if (head_start) {
        insert_offset = head_start - html + strlen("<head");
        while (html[insert_offset] != '>' && html[insert_offset] != '\0') {
            insert_offset++;
        }
        if (html[insert_offset] == '>') {
            insert_offset++;
        }
        insert_pos = html + insert_offset;
    } else {
        const char* html_start = find_tag_case_insensitive(html, "html");
        if (html_start) {
            insert_offset = html_start - html + strlen("<html");
            while (html[insert_offset] != '>' && html[insert_offset] != '\0') {
                insert_offset++;
            }
            if (html[insert_offset] == '>') {
                insert_offset++;
            }
            insert_pos = html + insert_offset;
        }
    }
    
    if (!insert_pos) {
        insert_pos = html;
        insert_offset = 0;
    }
    
    size_t html_len = strlen(html);
    size_t charset_tag_len = strlen("<meta charset=\"\">") + strlen(encoding);
    size_t new_len = html_len + charset_tag_len + 1;
    
    if (new_len > 1048576) {
        charset_module_cleanup();
        char* copy = (char*)malloc(html_len + 1);
        if (copy) strncpy(copy, html, html_len + 1);
        return copy;
    }
    
    char* result = (char*)malloc(new_len);
    if (!result) {
        charset_module_cleanup();
        char* copy = (char*)malloc(html_len + 1);
        if (copy) strncpy(copy, html, html_len + 1);
        return copy;
    }
    
    memcpy(result, html, insert_offset);
    snprintf(result + insert_offset, new_len - insert_offset, 
             "<meta charset=\"%s\">%s", encoding, html + insert_offset);
    
    charset_module_cleanup();
    return result;
}

void nl_web_add_html(nl_web_server_t* server, const char* path, const char* html) {
    // Smart detection: URL redirect, file path hot reload, or static content
    if (is_url(html)) {
        add_web_route_smart(server, path, html, "text/html");
        return;
    }
    
    char abs_path[512];
    if (is_file_path(html, abs_path, sizeof(abs_path))) {
        add_web_route_smart(server, path, html, "text/html");
        return;
    }
    
    // Static content with charset processing
    char* processed = add_charset_if_missing(server, html);
    if (processed) {
        add_web_route_smart(server, path, processed, "text/html");
        free(processed);
    }
}

void nl_web_add_vue(nl_web_server_t* server, const char* path, const char* vue_code) {
    // Smart detection: URL redirect or file path hot reload
    if (is_url(vue_code)) {
        add_web_route_smart(server, path, vue_code, "text/html");
        return;
    }
    
    char abs_path[512];
    if (is_file_path(vue_code, abs_path, sizeof(abs_path))) {
        add_web_route_smart(server, path, vue_code, "text/html");
        return;
    }
    
    // Vue code wrapped in full HTML
    char full_html[32768];
    snprintf(full_html, sizeof(full_html),
        "<!DOCTYPE html>\n"
        "<html><head><title>NetLeaf Vue</title>\n"
        "%s"
        "%s"
        "</head><body>\n"
        "<div id=\"app\">\n"
        "%s\n"
        "</div>\n"
        "<script>\n"
        "const { createApp, ref, reactive } = Vue;\n"
        "createApp({\n"
        "  setup() {\n"
        "    return { }\n"
        "  }\n"
        "}).mount('#app');\n"
        "</script>\n"
        "</body></html>",
        nl_responsive_css, nl_vue_cdn, vue_code);
    add_web_route_smart(server, path, full_html, "text/html");
}

void nl_web_set_encoding(nl_web_server_t* server, const char* encoding) {
    if (!server || !encoding) return;
    strncpy(server->encoding, encoding, sizeof(server->encoding) - 1);
}

NL_API void nl_web_enable_auto_encoding(nl_web_server_t* server, int enable) {
    if (!server) return;
    server->auto_encoding_enabled = enable;
}

NL_API int nl_web_is_auto_encoding_enabled(nl_web_server_t* server) {
    if (!server) return 0;
    return server->auto_encoding_enabled;
}

NL_API void nl_web_set_fallback_encoding(nl_web_server_t* server, const char* encoding) {
    if (!server || !encoding) return;
    strncpy(server->fallback_encoding, encoding, sizeof(server->fallback_encoding) - 1);
}

NL_API const char* nl_web_get_negotiated_encoding(nl_web_server_t* server) {
    if (!server) return NULL;
    if (server->auto_encoding_enabled && strlen(server->fallback_encoding) > 0) {
        return server->fallback_encoding;
    }
    return server->encoding;
}

// =========================================
// Error Page API (v2.2.0)
// =========================================

NL_API int nl_web_server_set_error_page(nl_web_server_t* server, int status_code, const char* template_path) {
    if (!server || !template_path) return NL_ERROR_PAGE_NOT_FOUND;
    if (status_code < 100 || status_code > 999) return NL_ERROR_PAGE_NOT_FOUND;
    
    int idx = 0;
    if (status_code == 400) idx = 0;
    else if (status_code == 401) idx = 1;
    else if (status_code == 403) idx = 2;
    else if (status_code == 404) idx = 3;
    else if (status_code == 500) idx = 4;
    else if (status_code == 502) idx = 5;
    else if (status_code == 503) idx = 6;
    else idx = 7;
    
    strncpy(server->error_page_templates[idx], template_path, sizeof(server->error_page_templates[idx]) - 1);
    server->error_page_templates[idx][sizeof(server->error_page_templates[idx]) - 1] = '\0';
    
    return NL_ERROR_PAGE_OK;
}

NL_API int nl_web_server_enable_error_suggestions(nl_web_server_t* server, int enable) {
    if (!server) return 0;
    server->error_suggestions_enabled = enable ? 1 : 0;
    return 1;
}

NL_API int nl_web_server_is_error_suggestions_enabled(nl_web_server_t* server) {
    if (!server) return 0;
    return server->error_suggestions_enabled;
}

static const char* get_error_code_string(int status_code) {
    switch (status_code) {
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown Error";
    }
}

NL_API char* nl_render_error_page(const char* template_content, nl_error_page_vars_t* vars) {
    if (!vars) return NULL;
    
    /* Reserved interface for future template-based error pages */
    (void)template_content;
    
    char* result = malloc(4096);
    if (!result) return NULL;
    
    snprintf(result, 4096, 
        "<html><head><title>%d - %s</title></head>"
        "<body><h1>%d %s</h1>"
        "<p>Requested path: %s</p>"
        "%s"
        "<p>Server version: %s</p></body></html>",
        vars->status_code, vars->error_message ? vars->error_message : get_error_code_string(vars->status_code),
        vars->status_code, vars->error_message ? vars->error_message : get_error_code_string(vars->status_code),
        vars->requested_path ? vars->requested_path : "/",
        vars->suggestion ? vars->suggestion : "",
        vars->server_version ? vars->server_version : "NetLeaf v2.4.0");
    
    return result;
}

NL_API char* nl_make_error_response(int status_code, const char* error_message, const char* requested_path, const char* suggestion) {
    nl_error_page_vars_t vars;
    memset(&vars, 0, sizeof(vars));
    vars.status_code = status_code;
    vars.error_message = error_message ? error_message : get_error_code_string(status_code);
    vars.requested_path = requested_path;
    vars.suggestion = suggestion;
    vars.server_version = "NetLeaf v2.4.0";
    
    time_t now = time(NULL);
    struct tm tm_buf;
    char time_str[64];
    localtime_s(&tm_buf, &now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);
    vars.timestamp = time_str;
    
    char* body = nl_render_error_page(NULL, &vars);
    if (!body) return NULL;
    
    size_t body_len = strlen(body);
    char* response = malloc(body_len + 256);
    if (!response) { free(body); return NULL; }
    
    snprintf(response, body_len + 256,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n%s",
        status_code, get_error_code_string(status_code),
        body_len, body);
    
    free(body);
    return response;
}

static char* substitute_variables(const char* template, const char** vars, const char** values, int count) {
    if (!template) {
        // template 为空指针时直接返回空字符串，避免 strlen(NULL)
        char* result = (char*)malloc(1);
        if (result) result[0] = '\0';
        return result;
    }
    if (!vars || !values || count <= 0) {
        char* result = (char*)malloc(strlen(template) + 1);
        if (result) strncpy(result, template, strlen(template) + 1);
        return result;
    }
    
    // 第一遍：完整扫描模板，精确计算替换后所需总长度。
    // 旧实现仅保证“当前替换点”装得下，未给替换点之后的字面量预留空间，
    // 且逐字符拷贝分支无边界检查，故改为先扫描计算总长再一次性分配。
    size_t total_len = 0;
    const char* scan = template;
    while (*scan) {
        if (strncmp(scan, "{{<var>", 7) == 0) {
            const char* var_start = scan + 7;
            const char* var_end = strstr(var_start, "</var>}}");
            if (var_end) {
                size_t var_len = var_end - var_start;
                char var_name[256];
                if (var_len >= sizeof(var_name)) var_len = sizeof(var_name) - 1;
                strncpy(var_name, var_start, var_len);
                var_name[var_len] = '\0';
                const char* replacement = "";
                for (int i = 0; i < count; i++) {
                    if (strcmp(vars[i], var_name) == 0) {
                        replacement = values[i] ? values[i] : "";
                        break;
                    }
                }
                total_len += strlen(replacement);
                scan = var_end + 8;
                continue;
            }
        }
        total_len++;
        scan++;
    }
    
    // 一次性分配精确容量（+1 存放结尾 '\0'），从根本上杜绝堆越界
    char* result = (char*)malloc(total_len + 1);
    if (!result) return NULL;
    
    // 第二遍：按同样规则拷贝，逐字符分支同样受剩余容量约束
    char* ptr = result;
    const char* src = template;
    size_t remaining = total_len;
    while (*src) {
        if (strncmp(src, "{{<var>", 7) == 0) {
            const char* var_start = src + 7;
            const char* var_end = strstr(var_start, "</var>}}");
            if (var_end) {
                size_t var_len = var_end - var_start;
                char var_name[256];
                if (var_len >= sizeof(var_name)) var_len = sizeof(var_name) - 1;
                strncpy(var_name, var_start, var_len);
                var_name[var_len] = '\0';
                const char* replacement = "";
                for (int i = 0; i < count; i++) {
                    if (strcmp(vars[i], var_name) == 0) {
                        replacement = values[i] ? values[i] : "";
                        break;
                    }
                }
                size_t rep_len = strlen(replacement);
                if (rep_len > remaining) rep_len = remaining;
                if (rep_len > 0) {
                    memcpy(ptr, replacement, rep_len);
                    ptr += rep_len;
                    remaining -= rep_len;
                }
                src = var_end + 8;
                continue;
            }
        }
        if (remaining == 0) break;
        *ptr++ = *src++;
        remaining--;
    }
    *ptr = '\0';
    return result;
}

void nl_web_add_html_with_vars(nl_web_server_t* server, const char* path, const char* html, const char** vars, const char** values, int count) {
    if (!server || !path || !html) return;
    
    char* substituted = substitute_variables(html, vars, values, count);
    if (substituted) {
        add_web_route(server, path, substituted, "text/html");
        free(substituted);
    }
}

void nl_web_add_vue_with_vars(nl_web_server_t* server, const char* path, const char* vue_code, const char** vars, const char** values, int count) {
    if (!server || !path || !vue_code) return;
    
    char* substituted = substitute_variables(vue_code, vars, values, count);
    if (substituted) {
        char full_html[32768];
        snprintf(full_html, sizeof(full_html),
            "<!DOCTYPE html>\n"
            "<html><head><title>NetLeaf Vue</title>\n"
            "%s"
            "%s"
            "</head><body>\n"
            "<div id=\"app\">\n"
            "%s\n"
            "</div>\n"
            "<script>\n"
            "const { createApp, ref, reactive } = Vue;\n"
            "createApp({\n"
            "  setup() {\n"
            "    return { }\n"
            "  }\n"
            "}).mount('#app');\n"
            "</script>\n"
            "</body></html>",
            nl_responsive_css, nl_vue_cdn, substituted);
        add_web_route(server, path, full_html, "text/html");
        free(substituted);
    }
}

void nl_web_add_json(nl_web_server_t* server, const char* path, const char* json) {
    // Smart detection: URL redirect or file path hot reload
    if (is_url(json)) {
        add_web_route_smart(server, path, json, "text/html");
        return;
    }
    
    char abs_path[512];
    if (is_file_path(json, abs_path, sizeof(abs_path))) {
        add_web_route_smart(server, path, json, "application/json");
        return;
    }
    
    // Static JSON content
    add_web_route_smart(server, path, json, "application/json");
}

// Redirect Type API
void nl_web_set_redirect_type(nl_web_server_t* server, nl_redirect_type_t type) {
    if (!server) return;
    EnterCriticalSection(&server->mutex);
    server->redirect_type = type;
    LeaveCriticalSection(&server->mutex);
    windows_log(NL_LOG_INFO, "Redirect type set to %d", type);
}

nl_redirect_type_t nl_web_get_redirect_type(nl_web_server_t* server) {
    if (!server) return NL_REDIRECT_TEMPORARY;
    return server->redirect_type;
}



// =========================================
// JSON Parser Implementation
// =========================================

typedef struct nl_json_node {
    nl_json_type_t type;
    union {
        int bool_val;
        int64_t int_val;
        double double_val;
        char* string_val;
        struct nl_json_node** array_val;
        struct {
            char** keys;
            struct nl_json_node** values;
            size_t count;
        } object_val;
    } data;
    size_t array_size;
} nl_json_node;

typedef struct nl_json {
    nl_json_node* root;
    nl_status_t error_code;
    int error_line;
    int error_col;
} nl_json_t;

static void skip_ws(const char** s, int* line, int* col) {
    while (**s && (unsigned char)**s <= 32) {
        if (**s == '\n') { (*line)++; *col = 0; }
        else (*col)++;
        (*s)++;
    }
}

static nl_json_node* parse_value(const char** s, int* line, int* col, nl_status_t* err) {
    skip_ws(s, line, col);
    if (!**s || **s == '\0') { *err = NL_EPARSE; return NULL; }
    
    nl_json_node* node = (nl_json_node*)calloc(1, sizeof(nl_json_node));
    if (!node) { *err = NL_ENOMEM; return NULL; }
    
    switch (**s) {
        case 'n': 
            if (strncmp(*s, "null", 4) == 0) { node->type = NL_JSON_NULL; (*s) += 4; *col += 4; }
            else { free(node); *err = NL_ESYNTAX; return NULL; }
            break;
        case 't':
            if (strncmp(*s, "true", 4) == 0) { node->type = NL_JSON_BOOL; node->data.bool_val = 1; (*s) += 4; *col += 4; }
            else { free(node); *err = NL_ESYNTAX; return NULL; }
            break;
        case 'f':
            if (strncmp(*s, "false", 5) == 0) { node->type = NL_JSON_BOOL; node->data.bool_val = 0; (*s) += 5; *col += 5; }
            else { free(node); *err = NL_ESYNTAX; return NULL; }
            break;
        case '"': {
            node->type = NL_JSON_STRING;
            (*s)++; (*col)++;
            size_t cap = 32, len = 0;
            node->data.string_val = (char*)malloc(cap);
            if (!node->data.string_val) { free(node); *err = NL_ENOMEM; return NULL; }
            while (**s && **s != '"') {
                if (**s == '\\') {
                    (*s)++; (*col)++;
                    if (!**s) { free(node->data.string_val); free(node); *err = NL_ESYNTAX; return NULL; }
                    char esc = 0;
                    switch (**s) {
                        case '"': case '\\': case '/': esc = **s; break;
                        case 'b': esc = '\b'; break;
                        case 'f': esc = '\f'; break;
                        case 'n': esc = '\n'; break;
                        case 'r': esc = '\r'; break;
                        case 't': esc = '\t'; break;
                        default: free(node->data.string_val); free(node); *err = NL_ESYNTAX; return NULL;
                    }
                    if (len + 1 >= cap) { cap *= 2; node->data.string_val = (char*)realloc(node->data.string_val, cap); }
                    node->data.string_val[len++] = esc;
                } else {
                    if (len + 1 >= cap) { cap *= 2; node->data.string_val = (char*)realloc(node->data.string_val, cap); }
                    node->data.string_val[len++] = **s;
                }
                (*s)++; (*col)++;
            }
            if (**s != '"') { free(node->data.string_val); free(node); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            node->data.string_val[len] = '\0';
            break;
        }
        case '[': {
            node->type = NL_JSON_ARRAY;
            (*s)++; (*col)++;
            size_t cap = 4; node->data.array_val = (nl_json_node**)malloc(sizeof(nl_json_node*) * cap);
            node->array_size = 0;
            if (!node->data.array_val) { free(node); *err = NL_ENOMEM; return NULL; }
            skip_ws(s, line, col);
            if (**s == ']') { (*s)++; break; }
            while (1) {
                if (node->array_size >= cap) { cap *= 2; node->data.array_val = (nl_json_node**)realloc(node->data.array_val, sizeof(nl_json_node*) * cap); }
                nl_json_node* item = parse_value(s, line, col, err);
                if (!item) { for (size_t i = 0; i < node->array_size; i++) free(node->data.array_val[i]); free(node->data.array_val); free(node); return NULL; }
                node->data.array_val[node->array_size++] = item;
                skip_ws(s, line, col);
                if (**s == ']') { (*s)++; break; }
                if (**s != ',') { for (size_t i = 0; i < node->array_size; i++) free(node->data.array_val[i]); free(node->data.array_val); free(node); *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
            }
            break;
        }
        case '{': {
            node->type = NL_JSON_OBJECT;
            node->data.object_val.keys = (char**)malloc(sizeof(char*) * 4);
            node->data.object_val.values = (nl_json_node**)malloc(sizeof(nl_json_node*) * 4);
            node->data.object_val.count = 0;
            size_t cap = 4;
            if (!node->data.object_val.keys || !node->data.object_val.values) { free(node); *err = NL_ENOMEM; return NULL; }
            (*s)++; (*col)++;
            skip_ws(s, line, col);
            if (**s == '}') { (*s)++; break; }
            while (1) {
                skip_ws(s, line, col);
                if (**s != '"') { *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
                size_t kcap = 32, klen = 0;
                char* key = (char*)malloc(kcap);
                if (!key) { free(node); *err = NL_ENOMEM; return NULL; }
                while (**s && **s != '"') {
                    if (klen + 1 >= kcap) { kcap *= 2; key = (char*)realloc(key, kcap); }
                    if (**s == '\\') { (*s)++; (*col)++; if (**s == '"') key[klen++] = '"'; else if (**s == '\\') key[klen++] = '\\'; else if (**s == 'n') key[klen++] = '\n'; else { free(key); *err = NL_ESYNTAX; return NULL; } }
                    else key[klen++] = **s;
                    (*s)++; (*col)++;
                }
                key[klen] = '\0';
                if (**s != '"') { free(key); *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
                skip_ws(s, line, col);
                if (**s != ':') { free(key); *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
                nl_json_node* val = parse_value(s, line, col, err);
                if (!val) { free(key); *err = NL_ESYNTAX; return NULL; }
                if (node->data.object_val.count >= cap) { cap *= 2; node->data.object_val.keys = (char**)realloc(node->data.object_val.keys, sizeof(char*) * cap); node->data.object_val.values = (nl_json_node**)realloc(node->data.object_val.values, sizeof(nl_json_node*) * cap); }
                node->data.object_val.keys[node->data.object_val.count] = key;
                node->data.object_val.values[node->data.object_val.count++] = val;
                skip_ws(s, line, col);
                if (**s == '}') { (*s)++; break; }
                if (**s != ',') { *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
            }
            break;
        }
        default: {
            if (**s == '-' || (**s >= '0' && **s <= '9')) {
                char* end; double d = strtod(*s, &end);
                if (end == *s) { free(node); *err = NL_ESYNTAX; return NULL; }
                if (d == (int64_t)d) { node->type = NL_JSON_INT; node->data.int_val = (int64_t)d; }
                else { node->type = NL_JSON_DOUBLE; node->data.double_val = d; }
                *s = end; *col += (int)(end - *s);
            } else { free(node); *err = NL_ESYNTAX; return NULL; }
        }
    }
    return node;
}

void* nl_json_parse(const char* json_str, nl_status_t* error_code, int* error_line, int* error_col) {
    if (!json_str) { if (error_code) *error_code = NL_EINVAL; return NULL; }
    nl_json_t* json = (nl_json_t*)calloc(1, sizeof(nl_json_t));
    if (!json) { if (error_code) *error_code = NL_ENOMEM; return NULL; }
    int line = 1, col = 0;
    nl_status_t err = NL_OK;
    json->root = parse_value(&json_str, &line, &col, &err);
    if (!json->root) { json->error_code = err; json->error_line = line; json->error_col = col; if (error_code) *error_code = err; if (error_line) *error_line = line; if (error_col) *error_col = col; free(json); return NULL; }
    if (error_code) *error_code = NL_OK;
    if (error_line) *error_line = line;
    if (error_col) *error_col = col;
    return json;
}

void* nl_json_parse_file(const char* file_path, nl_status_t* error_code) {
    if (!file_path) { if (error_code) *error_code = NL_EINVAL; return NULL; }
    FILE* fp = fopen(file_path, "r");
    if (!fp) { if (error_code) *error_code = NL_EFILE; return NULL; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(fp); if (error_code) *error_code = NL_ENOMEM; return NULL; }
    fread(buf, 1, sz, fp); buf[sz] = '\0'; fclose(fp);
    int el = 0, ec = 0;
    void* json = nl_json_parse(buf, error_code, &el, &ec);
    free(buf);
    return json;
}

static void free_node(nl_json_node* node) {
    if (!node) return;
    switch (node->type) {
        case NL_JSON_STRING: if (node->data.string_val) free(node->data.string_val); break;
        case NL_JSON_ARRAY: for (size_t i = 0; i < node->array_size; i++) free_node(node->data.array_val[i]); free(node->data.array_val); break;
        case NL_JSON_OBJECT: for (size_t i = 0; i < node->data.object_val.count; i++) { free(node->data.object_val.keys[i]); free_node(node->data.object_val.values[i]); } free(node->data.object_val.keys); free(node->data.object_val.values); break;
        default: break;
    }
    free(node);
}

static void stringify_node(nl_json_node* node, char** out, size_t* cap, size_t* len, int pretty, int indent) {
    char buf[128];
    switch (node->type) {
        case NL_JSON_NULL:
            while (*cap - *len < 4) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, "null", 4); *len += 4;
            break;
        case NL_JSON_BOOL:
            if (node->data.bool_val) {
                while (*cap - *len < 4) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
                memcpy(*out + *len, "true", 4); *len += 4;
            } else {
                while (*cap - *len < 5) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
                memcpy(*out + *len, "false", 5); *len += 5;
            }
            break;
        case NL_JSON_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)node->data.int_val);
            size_t ilen = strlen(buf);
            while (*cap - *len < ilen) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, buf, ilen); *len += ilen;
            break;
        case NL_JSON_DOUBLE:
            snprintf(buf, sizeof(buf), "%.17g", node->data.double_val);
            ilen = strlen(buf);
            while (*cap - *len < ilen) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, buf, ilen); *len += ilen;
            break;
        case NL_JSON_STRING: {
            while (*cap - *len < 2) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            (*out)[(*len)++] = '"';
            size_t slen = strlen(node->data.string_val);
            while (*cap - *len < slen + 2) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            for (size_t i = 0; i < slen; i++) {
                char c = node->data.string_val[i];
                if (c == '"' || c == '\\' || c == '/') {
                    (*out)[(*len)++] = '\\';
                    (*out)[(*len)++] = c;
                } else if (c == '\b') { (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 'b'; }
                else if (c == '\f') { (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 'f'; }
                else if (c == '\n') { (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 'n'; }
                else if (c == '\r') { (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 'r'; }
                else if (c == '\t') { (*out)[(*len)++] = '\\'; (*out)[(*len)++] = 't'; }
                else (*out)[(*len)++] = c;
            }
            (*out)[(*len)++] = '"';
            break;
        }
        case NL_JSON_ARRAY:
            (*out)[(*len)++] = '[';
            for (size_t i = 0; i < node->array_size; i++) {
                if (i > 0) { (*out)[(*len)++] = ','; }
                stringify_node(node->data.array_val[i], out, cap, len, pretty, indent);
            }
            (*out)[(*len)++] = ']';
            break;
        case NL_JSON_OBJECT:
            (*out)[(*len)++] = '{';
            for (size_t i = 0; i < node->data.object_val.count; i++) {
                if (i > 0) { (*out)[(*len)++] = ','; }
                size_t klen = strlen(node->data.object_val.keys[i]);
                while (*cap - *len < klen + 4) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
                (*out)[(*len)++] = '"';
                memcpy(*out + *len, node->data.object_val.keys[i], klen);
                *len += klen;
                (*out)[(*len)++] = '"';
                (*out)[(*len)++] = ':';
                stringify_node(node->data.object_val.values[i], out, cap, len, pretty, indent);
            }
            (*out)[(*len)++] = '}';
            break;
    }
}

void nl_json_destroy(void* json) { if (!json) return; nl_json_t* j = (nl_json_t*)json; if (j->root) free_node(j->root); free(j); }
int nl_json_get_type(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root ? j->root->type : 0; }
int nl_json_get_bool(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == 1 ? j->root->data.bool_val : 0; }
int64_t nl_json_get_int(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == 2 ? j->root->data.int_val : 0; }
double nl_json_get_double(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == 3 ? j->root->data.double_val : 0.0; }
const char* nl_json_get_string(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == 4 ? j->root->data.string_val : ""; }
size_t nl_json_array_size(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == 5 ? j->root->array_size : 0; }
void* nl_json_array_get(void* json, size_t index) { nl_json_t* j = (nl_json_t*)json; if (!j || !j->root || j->root->type != 5 || index >= j->root->array_size) return NULL; nl_json_t* r = (nl_json_t*)calloc(1, sizeof(nl_json_t)); if (r) r->root = j->root->data.array_val[index]; return r; }
void* nl_json_object_get(void* json, const char* key) { nl_json_t* j = (nl_json_t*)json; if (!j || !j->root || j->root->type != 6 || !key) return NULL; for (size_t i = 0; i < j->root->data.object_val.count; i++) if (strcmp(j->root->data.object_val.keys[i], key) == 0) { nl_json_t* r = (nl_json_t*)calloc(1, sizeof(nl_json_t)); if (r) r->root = j->root->data.object_val.values[i]; return r; } return NULL; }
int nl_json_has_key(void* json, const char* key) { nl_json_t* j = (nl_json_t*)json; if (!j || !j->root || j->root->type != 6 || !key) return 0; for (size_t i = 0; i < j->root->data.object_val.count; i++) if (strcmp(j->root->data.object_val.keys[i], key) == 0) return 1; return 0; }
char* nl_json_stringify(void* json, int pretty) { nl_json_t* j = (nl_json_t*)json; if (!j || !j->root) return (char*)""; size_t cap = 256, len = 0; char* out = (char*)malloc(cap); if (!out) return NULL; stringify_node(j->root, &out, &cap, &len, pretty, 0); out = (char*)realloc(out, len + 1); out[len] = '\0'; return out; }
int nl_json_save_file(void* json, const char* file_path, int pretty) { if (!json || !file_path) return NL_EINVAL; char* s = nl_json_stringify(json, pretty); if (!s) return NL_ENOMEM; FILE* fp = fopen(file_path, "w"); if (!fp) { free(s); return NL_EFILE; } fwrite(s, 1, strlen(s), fp); fclose(fp); free(s); return NL_OK; }

// JSON 字符串转义：把任意字符串（含 token/key 等敏感字段）转义为安全 JSON 字符串字面量。
// 调用方可将返回值拼接进 JSON 再输出，避免原值被直接打印造成信息泄露或注入。
// 返回值需调用方 free。NULL 输入返回指向空串的静态常量。
char* nl_json_escape(const char* input) {
    if (!input) return (char*)"";
    size_t len = strlen(input);
    size_t cap = len * 6 + 2;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    char* p = out;
    *p++ = '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        switch (c) {
            case '"':  *p++ = '\\'; *p++ = '"';  break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b';  break;
            case '\f': *p++ = '\\'; *p++ = 'f';  break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:
                if (c < 0x20) {
                    p += sprintf(p, "\\u%04x", c);
                } else {
                    *p++ = (char)c;
                }
                break;
        }
    }
    *p++ = '"';
    *p = '\0';
    return out;
}

const char* nl_json_error_message(nl_status_t error_code) {
    switch (error_code) {
        case NL_OK: return "No error";
        case NL_EINVAL: return "Invalid parameter";
        case NL_ENOMEM: return "Out of memory";
        case NL_EPARSE: return "Parse error";
        case NL_ESYNTAX: return "Syntax error";
        case NL_EFILE: return "File error";
        default: return "Unknown error";
    }
}

// =========================================
// TOML Parser Implementation
// =========================================

typedef struct nl_toml_node {
    nl_toml_type_t type;
    union {
        int bool_val;
        int64_t int_val;
        double float_val;
        char* string_val;
        struct nl_toml_node** array_val;
        struct {
            char** keys;
            struct nl_toml_node** values;
            size_t count;
        } table_val;
    } data;
    size_t array_size;
} nl_toml_node;

typedef struct nl_toml {
    nl_toml_node* root;
    nl_status_t error_code;
    int error_line;
    int error_col;
} nl_toml_t;

static void toml_skip_ws(const char** s, int* line, int* col) {
    while (**s && (unsigned char)**s <= 32) {
        if (**s == '\n') { (*line)++; *col = 0; }
        else if (**s == '\r') { (*col) = 0; }
        else (*col)++;
        (*s)++;
    }
    while (**s == '#') {
        while (**s && **s != '\n' && **s != '\r') { (*s)++; (*col)++; }
        toml_skip_ws(s, line, col);
    }
}

static int toml_parse_string(const char** s, int* line, int* col, char** out, nl_status_t* err) {
    (void)line;
    if (**s != '"') { *err = NL_ESYNTAX; return -1; }
    (*s)++; (*col)++;
    size_t cap = 32, len = 0;
    char* val = (char*)malloc(cap);
    if (!val) { *err = NL_ENOMEM; return -1; }
    while (**s && **s != '"') {
        if (**s == '\\') {
            (*s)++; (*col)++;
            if (!**s) { free(val); *err = NL_ESYNTAX; return -1; }
            char esc = 0;
            switch (**s) {
                case '"': esc = '"'; break;
                case '\\': esc = '\\'; break;
                case 'n': esc = '\n'; break;
                case 'r': esc = '\r'; break;
                case 't': esc = '\t'; break;
                default: free(val); *err = NL_ESYNTAX; return -1;
            }
            if (len + 1 >= cap) { cap *= 2; val = (char*)realloc(val, cap); }
            val[len++] = esc;
        } else {
            if (len + 1 >= cap) { cap *= 2; val = (char*)realloc(val, cap); }
            val[len++] = **s;
        }
        (*s)++; (*col)++;
    }
    if (**s != '"') { free(val); *err = NL_ESYNTAX; return -1; }
    (*s)++; (*col)++;
    if (len + 1 >= cap) val = (char*)realloc(val, len + 1);
    val[len] = '\0';
    *out = val;
    return 0;
}

// BUG-304: 删除未使用的冗余函数 toml_parse_int
// （数字解析统一走 toml_parse_float，整/浮点由 d == (int64_t)d 判定）
static double toml_parse_float(const char** s, int* col, nl_status_t* err) {
    (void)col;
    (void)err;  // BUG-304: err 在解析路径由调用方处理，本函数不写入
    int sign = 1;
    if (**s == '-') { sign = -1; (*s)++; (*col)++; }
    else if (**s == '+') { (*s)++; (*col)++; }
    double val = 0.0;
    while (**s >= '0' && **s <= '9') {
        val = val * 10 + (**s - '0');
        (*s)++; (*col)++;
    }
    if (**s == '.') {
        (*s)++; (*col)++;
        double frac = 0.1;
        while (**s >= '0' && **s <= '9') {
            val += (**s - '0') * frac;
            frac *= 0.1;
            (*s)++; (*col)++;
        }
    }
    if (**s == 'e' || **s == 'E') {
        (*s)++; (*col)++;
        int exp_sign = 1;
        if (**s == '-') { exp_sign = -1; (*s)++; (*col)++; }
        else if (**s == '+') { (*s)++; (*col)++; }
        int exp = 0;
        while (**s >= '0' && **s <= '9') {
            exp = exp * 10 + (**s - '0');
            (*s)++; (*col)++;
        }
        while (exp > 0) { val *= 10.0; exp--; }
        if (exp_sign < 0) val = 1.0 / val;
    }
    return val * sign;
}

static nl_toml_node* toml_parse_value(const char** s, int* line, int* col, nl_status_t* err) {
    toml_skip_ws(s, line, col);
    if (!**s || **s == '\0') { *err = NL_EPARSE; return NULL; }
    nl_toml_node* node = (nl_toml_node*)calloc(1, sizeof(nl_toml_node));
    if (!node) { *err = NL_ENOMEM; return NULL; }

    if (**s == '"') {
        node->type = NL_TOML_STRING;
        if (toml_parse_string(s, line, col, &node->data.string_val, err) != 0) { free(node); return NULL; }
    } else if (**s == '[') {
        node->type = NL_TOML_ARRAY;
        (*s)++; (*col)++;
        size_t cap = 4; node->data.array_val = (nl_toml_node**)malloc(sizeof(nl_toml_node*) * cap);
        node->array_size = 0;
        if (!node->data.array_val) { free(node); *err = NL_ENOMEM; return NULL; }
        toml_skip_ws(s, line, col);
        if (**s == ']') { (*s)++; (*col)++; return node; }
        while (1) {
            if (node->array_size >= cap) { cap *= 2; node->data.array_val = (nl_toml_node**)realloc(node->data.array_val, sizeof(nl_toml_node*) * cap); }
            nl_toml_node* item = toml_parse_value(s, line, col, err);
            if (!item) { for (size_t i = 0; i < node->array_size; i++) free(node->data.array_val[i]); free(node->data.array_val); free(node); return NULL; }
            node->data.array_val[node->array_size++] = item;
            toml_skip_ws(s, line, col);
            if (**s == ']') { (*s)++; (*col)++; break; }
            if (**s != ',') { for (size_t i = 0; i < node->array_size; i++) free(node->data.array_val[i]); free(node->data.array_val); free(node); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            toml_skip_ws(s, line, col);
        }
    } else if (**s == '{') {
        node->type = NL_TOML_TABLE;
        node->data.table_val.keys = (char**)malloc(sizeof(char*) * 4);
        node->data.table_val.values = (nl_toml_node**)malloc(sizeof(nl_toml_node*) * 4);
        node->data.table_val.count = 0;
        size_t cap = 4;
        if (!node->data.table_val.keys || !node->data.table_val.values) { free(node); *err = NL_ENOMEM; return NULL; }
        (*s)++; (*col)++;
        toml_skip_ws(s, line, col);
        if (**s == '}') { (*s)++; (*col)++; return node; }
        while (1) {
            toml_skip_ws(s, line, col);
            if (**s != '"') { *err = NL_ESYNTAX; return NULL; }
            char* key;
            if (toml_parse_string(s, line, col, &key, err) != 0) { *err = NL_ESYNTAX; return NULL; }
            toml_skip_ws(s, line, col);
            if (**s != '=') { free(key); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            nl_toml_node* val = toml_parse_value(s, line, col, err);
            if (!val) { free(key); *err = NL_ESYNTAX; return NULL; }
            if (node->data.table_val.count >= cap) { cap *= 2; node->data.table_val.keys = (char**)realloc(node->data.table_val.keys, sizeof(char*) * cap); node->data.table_val.values = (nl_toml_node**)realloc(node->data.table_val.values, sizeof(nl_toml_node*) * cap); }
            node->data.table_val.keys[node->data.table_val.count] = key;
            node->data.table_val.values[node->data.table_val.count++] = val;
            toml_skip_ws(s, line, col);
            if (**s == '}') { (*s)++; (*col)++; break; }
            if (**s != ',') { *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            toml_skip_ws(s, line, col);
        }
    } else if (**s == 't' && strncmp(*s, "true", 4) == 0) {
        node->type = NL_TOML_BOOL; node->data.bool_val = 1; (*s) += 4; (*col) += 4;
    } else if (**s == 'f' && strncmp(*s, "false", 5) == 0) {
        node->type = NL_TOML_BOOL; node->data.bool_val = 0; (*s) += 5; (*col) += 5;
    } else if (**s == '-' || **s == '+' || (**s >= '0' && **s <= '9')) {
        const char* start = *s;
        double d = toml_parse_float(s, col, err);
        if (start == *s) { free(node); *err = NL_ESYNTAX; return NULL; }
        if (d == (int64_t)d) { node->type = NL_TOML_INT; node->data.int_val = (int64_t)d; }
        else { node->type = NL_TOML_FLOAT; node->data.float_val = d; }
    } else {
        free(node); *err = NL_ESYNTAX; return NULL;
    }
    return node;
}

static void toml_free_node(nl_toml_node* node) {
    if (!node) return;
    switch (node->type) {
        case NL_TOML_NULL: break;
        case NL_TOML_STRING: free(node->data.string_val); break;
        case NL_TOML_ARRAY:
            for (size_t i = 0; i < node->array_size; i++) toml_free_node(node->data.array_val[i]);
            free(node->data.array_val);
            break;
        case NL_TOML_TABLE:
            for (size_t i = 0; i < node->data.table_val.count; i++) {
                free(node->data.table_val.keys[i]);
                toml_free_node(node->data.table_val.values[i]);
            }
            free(node->data.table_val.keys);
            free(node->data.table_val.values);
            break;
        default: break;
    }
    free(node);
}

static void toml_stringify_node(nl_toml_node* node, char** out, size_t* cap, size_t* len, int indent) {
    char buf[128];
    switch (node->type) {
        case NL_TOML_STRING: {
            size_t slen = strlen(node->data.string_val);
            size_t need = slen + 3;
            while (*cap - *len < need) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            (*out)[(*len)++] = '"';
            memcpy(*out + *len, node->data.string_val, slen);
            *len += slen;
            (*out)[(*len)++] = '"';
            break;
        }
        case NL_TOML_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)node->data.int_val);
            size_t ilen = strlen(buf);
            while (*cap - *len < ilen) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, buf, ilen);
            *len += ilen;
            break;
        case NL_TOML_FLOAT:
            snprintf(buf, sizeof(buf), "%.17g", node->data.float_val);
            ilen = strlen(buf);
            while (*cap - *len < ilen) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, buf, ilen);
            *len += ilen;
            break;
        case NL_TOML_BOOL:
            ilen = node->data.bool_val ? 4 : 5;
            while (*cap - *len < ilen) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, node->data.bool_val ? "true" : "false", ilen);
            *len += ilen;
            break;
        case NL_TOML_ARRAY:
            (*out)[(*len)++] = '[';
            for (size_t i = 0; i < node->array_size; i++) {
                if (i > 0) { (*out)[(*len)++] = ','; }
                toml_stringify_node(node->data.array_val[i], out, cap, len, indent);
            }
            (*out)[(*len)++] = ']';
            break;
        case NL_TOML_TABLE:
            (*out)[(*len)++] = '{';
            for (size_t i = 0; i < node->data.table_val.count; i++) {
                if (i > 0) { (*out)[(*len)++] = ','; }
                ilen = strlen(node->data.table_val.keys[i]);
                while (*cap - *len < ilen + 4) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
                (*out)[(*len)++] = '"';
                memcpy(*out + *len, node->data.table_val.keys[i], ilen);
                *len += ilen;
                (*out)[(*len)++] = '"';
                (*out)[(*len)++] = ':';
                toml_stringify_node(node->data.table_val.values[i], out, cap, len, indent);
            }
            (*out)[(*len)++] = '}';
            break;
        default: break;
    }
}

static nl_toml_node* toml_parse_main(const char** s, int* line, int* col, nl_status_t* err) {
    nl_toml_node* root = (nl_toml_node*)calloc(1, sizeof(nl_toml_node));
    if (!root) { *err = NL_ENOMEM; return NULL; }
    root->type = NL_TOML_TABLE;
    root->data.table_val.keys = (char**)malloc(sizeof(char*) * 4);
    root->data.table_val.values = (nl_toml_node**)malloc(sizeof(nl_toml_node*) * 4);
    root->data.table_val.count = 0;
    size_t cap = 4;
    if (!root->data.table_val.keys || !root->data.table_val.values) { free(root); *err = NL_ENOMEM; return NULL; }

    while (**s) {
        toml_skip_ws(s, line, col);
        if (!**s || **s == '\0') break;
        if (**s == '[') {
            (*s)++; (*col)++;
            toml_skip_ws(s, line, col);
            size_t kcap = 32, klen = 0;
            char* key = (char*)malloc(kcap);
            if (!key) { toml_free_node(root); *err = NL_ENOMEM; return NULL; }
            while (**s && **s != ']') {
                if (klen + 1 >= kcap) { kcap *= 2; key = (char*)realloc(key, kcap); }
                key[klen++] = **s;
                (*s)++; (*col)++;
            }
            if (**s != ']') { free(key); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
            key[klen] = '\0';
            (*s)++; (*col)++;
            toml_skip_ws(s, line, col);
            if (**s != '\n' && **s != '\r') { free(key); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
            nl_toml_node* tbl = (nl_toml_node*)calloc(1, sizeof(nl_toml_node));
            if (!tbl) { free(key); toml_free_node(root); *err = NL_ENOMEM; return NULL; }
            tbl->type = NL_TOML_TABLE;
            tbl->data.table_val.keys = (char**)malloc(sizeof(char*) * 4);
            tbl->data.table_val.values = (nl_toml_node**)malloc(sizeof(nl_toml_node*) * 4);
            tbl->data.table_val.count = 0;
            if (!tbl->data.table_val.keys || !tbl->data.table_val.values) { free(key); free(tbl); toml_free_node(root); *err = NL_ENOMEM; return NULL; }
            size_t tcap = 4;
            while (**s) {
                toml_skip_ws(s, line, col);
                if (!**s || **s == '[' || (**s >= 'a' && **s <= 'z') || (**s >= 'A' && **s <= 'Z') || **s == '_') break;
                if (**s == '"') {
                    (*s)++; (*col)++;
                    size_t kvcap = 32, kvlen = 0;
                    char* kval = (char*)malloc(kvcap);
                    if (!kval) { free(key); toml_free_node(tbl); toml_free_node(root); *err = NL_ENOMEM; return NULL; }
                    while (**s && **s != '"') {
                        if (kvlen + 1 >= kvcap) { kvcap *= 2; kval = (char*)realloc(kval, kvcap); }
                        if (**s == '\\') { (*s)++; (*col)++; if (**s == '"') kval[kvlen++] = '"'; else if (**s == '\\') kval[kvlen++] = '\\'; else if (**s == 'n') kval[kvlen++] = '\n'; else { free(kval); free(key); toml_free_node(tbl); toml_free_node(root); *err = NL_ESYNTAX; return NULL; } }
                        else kval[kvlen++] = **s;
                        (*s)++; (*col)++;
                    }
                    if (**s != '"') { free(kval); free(key); toml_free_node(tbl); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
                    (*s)++; (*col)++;
                    kval[kvlen] = '\0';
                    toml_skip_ws(s, line, col);
                    if (**s != '=') { free(kval); free(key); toml_free_node(tbl); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
                    (*s)++; (*col)++;
                    nl_toml_node* val = toml_parse_value(s, line, col, err);
                    if (!val) { free(kval); free(key); toml_free_node(tbl); toml_free_node(root); return NULL; }
                    if (tbl->data.table_val.count >= tcap) { tcap *= 2; tbl->data.table_val.keys = (char**)realloc(tbl->data.table_val.keys, sizeof(char*) * tcap); tbl->data.table_val.values = (nl_toml_node**)realloc(tbl->data.table_val.values, sizeof(nl_toml_node*) * tcap); }
                    tbl->data.table_val.keys[tbl->data.table_val.count] = kval;
                    tbl->data.table_val.values[tbl->data.table_val.count++] = val;
                    toml_skip_ws(s, line, col);
                    if (**s == '\n' || **s == '\r') { (*s)++; if (**s == '\n') { (*line)++; (*col) = 0; } }
                    else if (**s != ',') break;
                    if (**s == ',') { (*s)++; (*col)++; toml_skip_ws(s, line, col); }
                } else {
                    (*s)++; (*col)++;
                }
            }
            if (root->data.table_val.count >= cap) { cap *= 2; root->data.table_val.keys = (char**)realloc(root->data.table_val.keys, sizeof(char*) * cap); root->data.table_val.values = (nl_toml_node**)realloc(root->data.table_val.values, sizeof(nl_toml_node*) * cap); }
            root->data.table_val.keys[root->data.table_val.count] = key;
            root->data.table_val.values[root->data.table_val.count++] = tbl;
        } else if (**s == '"') {
            (*s)++; (*col)++;
            size_t kvcap = 32, kvlen = 0;
            char* kval = (char*)malloc(kvcap);
            if (!kval) { toml_free_node(root); *err = NL_ENOMEM; return NULL; }
            while (**s && **s != '"') {
                if (kvlen + 1 >= kvcap) { kvcap *= 2; kval = (char*)realloc(kval, kvcap); }
                if (**s == '\\') { (*s)++; (*col)++; if (**s == '"') kval[kvlen++] = '"'; else if (**s == '\\') kval[kvlen++] = '\\'; else if (**s == 'n') kval[kvlen++] = '\n'; else { free(kval); toml_free_node(root); *err = NL_ESYNTAX; return NULL; } }
                else kval[kvlen++] = **s;
                (*s)++; (*col)++;
            }
            if (**s != '"') { free(kval); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            kval[kvlen] = '\0';
            toml_skip_ws(s, line, col);
            if (**s != '=') { free(kval); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            nl_toml_node* val = toml_parse_value(s, line, col, err);
            if (!val) { free(kval); toml_free_node(root); return NULL; }
            if (root->data.table_val.count >= cap) { cap *= 2; root->data.table_val.keys = (char**)realloc(root->data.table_val.keys, sizeof(char*) * cap); root->data.table_val.values = (nl_toml_node**)realloc(root->data.table_val.values, sizeof(nl_toml_node*) * cap); }
            root->data.table_val.keys[root->data.table_val.count] = kval;
            root->data.table_val.values[root->data.table_val.count++] = val;
            toml_skip_ws(s, line, col);
            if (**s == '\n' || **s == '\r') { (*s)++; if (**s == '\n') { (*line)++; (*col) = 0; } }
            else if (**s == ',') { (*s)++; (*col)++; }
        } else {
            (*s)++; (*col)++;
        }
    }
    return root;
}

void* nl_toml_parse(const char* toml_str, nl_status_t* error_code, int* error_line, int* error_col) {
    if (!toml_str) { if (error_code) *error_code = NL_EINVAL; return NULL; }
    nl_toml_t* toml = (nl_toml_t*)calloc(1, sizeof(nl_toml_t));
    if (!toml) { if (error_code) *error_code = NL_ENOMEM; return NULL; }
    int line = 1, col = 0;
    nl_status_t err = NL_OK;
    toml->root = toml_parse_main(&toml_str, &line, &col, &err);
    if (!toml->root) { toml->error_code = err; toml->error_line = line; toml->error_col = col; if (error_code) *error_code = err; if (error_line) *error_line = line; if (error_col) *error_col = col; free(toml); return NULL; }
    if (error_code) *error_code = NL_OK;
    if (error_line) *error_line = line;
    if (error_col) *error_col = col;
    return toml;
}

void* nl_toml_parse_file(const char* file_path, nl_status_t* error_code) {
    if (!file_path) { if (error_code) *error_code = NL_EINVAL; return NULL; }
    FILE* fp = fopen(file_path, "r");
    if (!fp) { if (error_code) *error_code = NL_EFILE; return NULL; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(fp); if (error_code) *error_code = NL_ENOMEM; return NULL; }
    fread(buf, 1, sz, fp); buf[sz] = '\0'; fclose(fp);
    int el = 0, ec = 0;
    void* toml = nl_toml_parse(buf, error_code, &el, &ec);
    free(buf);
    return toml;
}

void nl_toml_destroy(void* toml) {
    if (!toml) return;
    nl_toml_t* t = (nl_toml_t*)toml;
    if (t->root) toml_free_node(t->root);
    free(t);
}

int nl_toml_get_type(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return t && t->root ? t->root->type : 0; }
const char* nl_toml_get_string(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_STRING) ? t->root->data.string_val : ""; }
int64_t nl_toml_get_int(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_INT) ? t->root->data.int_val : 0; }
double nl_toml_get_float(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_FLOAT) ? t->root->data.float_val : 0.0; }
int nl_toml_get_bool(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_BOOL) ? t->root->data.bool_val : 0; }
size_t nl_toml_array_size(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_ARRAY) ? t->root->array_size : 0; }
void* nl_toml_array_get(void* toml, size_t index) {
    nl_toml_t* t = (nl_toml_t*)toml;
    if (!t || !t->root || t->root->type != NL_TOML_ARRAY || index >= t->root->array_size) return NULL;
    nl_toml_t* r = (nl_toml_t*)calloc(1, sizeof(nl_toml_t));
    if (r) r->root = t->root->data.array_val[index];
    return r;
}
void* nl_toml_table_get(void* toml, const char* key) {
    nl_toml_t* t = (nl_toml_t*)toml;
    if (!t || !t->root || t->root->type != NL_TOML_TABLE || !key) return NULL;
    for (size_t i = 0; i < t->root->data.table_val.count; i++)
        if (strcmp(t->root->data.table_val.keys[i], key) == 0) {
            nl_toml_t* r = (nl_toml_t*)calloc(1, sizeof(nl_toml_t));
            if (r) r->root = t->root->data.table_val.values[i];
            return r;
        }
    return NULL;
}
int nl_toml_has_key(void* toml, const char* key) {
    nl_toml_t* t = (nl_toml_t*)toml;
    if (!t || !t->root || t->root->type != NL_TOML_TABLE || !key) return 0;
    for (size_t i = 0; i < t->root->data.table_val.count; i++)
        if (strcmp(t->root->data.table_val.keys[i], key) == 0) return 1;
    return 0;
}
char* nl_toml_stringify(void* toml) {
    nl_toml_t* t = (nl_toml_t*)toml;
    if (!t || !t->root) return (char*)"";
    size_t cap = 256, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    toml_stringify_node(t->root, &out, &cap, &len, 0);
    out = (char*)realloc(out, len + 1);
    out[len] = '\0';
    return out;
}
int nl_toml_save_file(void* toml, const char* file_path) {
    if (!toml || !file_path) return NL_EINVAL;
    char* s = nl_toml_stringify(toml);
    if (!s) return NL_ENOMEM;
    FILE* fp = fopen(file_path, "w");
    if (!fp) { free(s); return NL_EFILE; }
    fwrite(s, 1, strlen(s), fp);
    fclose(fp);
    free(s);
    return NL_OK;
}
const char* nl_toml_error_message(nl_status_t error_code) { return nl_json_error_message(error_code); }

