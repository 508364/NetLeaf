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
#include "../include/nl_util.h"
#include "../common/nl_proxy_core.h"
#include "../common/nl_uri.h"
#include "../common/nl_filesrv.h"
#include "../common/nl_px_pool.h"
#include "../common/nl_px_h2.h"
#include "../common/nl_px_local.h"

#ifdef NETLEAF_LEGACY_WORKER_POOL
#include "netleaf_worker_pool_windows.h"
#endif

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
// ---- 通用服务器（TCP/HTTP/WEBSOCKET）全 IOCP 化 ----
// 取代旧 worker 池（彻底废弃）：AcceptEx 预 post + 每连接 OVERLAPPED WSARecv +
// GetQueuedCompletionStatus 完成线程驱动 handler（模式同 Web 引擎 IOCP 先例）。
#define NL_SERVER_MAX_CONNS   1024   // 连接槽位上限（对齐旧 worker 队列深度）
#define NL_SERVER_IDLE_MS     30000  // 空闲断开，对齐旧 worker select 30s 超时
#define NL_SERVER_LISTEN_KEY  ((ULONG_PTR)0)  // completion key=0 → AcceptEx 完成

// 连接槽位状态机：FREE → ACTIVE(WSARecv pending) → PROCESSING(完成处理中) →
// ACTIVE（重 post）或 FREE（关闭回收）；CLOSING：空闲扫描已 closesocket，
// 槽位保持占用，等待 pending 完成事件回来后由完成处理段回收（避免槽位复用
// 与迟到完成事件竞态 —— 槽位内存预分配永不释放，无需 conn_id 代号）。
#define NL_CONN_FREE       0
#define NL_CONN_ACTIVE     1
#define NL_CONN_PROCESSING 2
#define NL_CONN_CLOSING    3

// UDP 接收环槽位数：同时 outstanding 的 WSARecvFrom 数。旧版单缓冲在 handler
// 同步执行期间零 outstanding，突发 datagram 仅靠内核 socket 队列兜底（易溢出
// 丢包）；多槽后 handler 处理第 k 个 datagram 期间其余槽位仍由内核异步填充，
// 突发吸收上限 = NL_UDP_RCV_SLOTS 个已缓冲 datagram + socket 队列。
#define NL_UDP_RCV_SLOTS 16

typedef struct nl_udp_slot {
    OVERLAPPED ov;      // 必须为首成员：完成事件指针直接转回槽位
    WSABUF     wb;
    char       buf[BUFFER_SIZE];
    // WSARecvFrom 需要的地址缓冲：size = SOCKADDR + 预留
    char       addrbuf[sizeof(SOCKADDR_IN) + 32];
} nl_udp_slot_t;

typedef struct nl_server_conn {
    OVERLAPPED ov;          // 必须为首个成员：完成事件指针直接转回槽位
    SOCKET fd;
    volatile long state;    // NL_CONN_*
    char* buf;              // 接收缓冲（槽位创建时 malloc，服务器拆卸时 free）
    size_t len;             // 当前累积长度（每次请求处理后清零）
    size_t cap;             // 缓冲容量（不足时倍增）
    ULONGLONG last_active;  // GetTickCount64，空闲扫描基准
} nl_server_conn_t;

struct nl_server {
    SOCKET fd;
    HANDLE iocp_handle;
    nl_protocol_t protocol;
    int port;
    nl_request_handler handler;
    nl_udp_message_handler udp_handler;
    nl_udp_message_handler_v2 udp_handler_v2;
    void* user_data;
    volatile long running;
    HANDLE thread_handle;
    int concurrency;
    // ---- TCP/HTTP/WEBSOCKET IOCP（取代旧 worker 池） ----
    nl_server_conn_t* conns;      // 预分配槽位数组（NL_SERVER_MAX_CONNS）
    int conn_count;               // 当前活动连接数（conn_lock 保护）
    CRITICAL_SECTION conn_lock;   // 槽位分配/释放/空闲扫描串行化
    int conn_lock_init;
    SOCKET accept_client;         // AcceptEx 备用 socket（预创建）
    HANDLE* io_threads;           // 完成消费线程（listener 自身为线程 0）
    int num_io_threads;
    // ---- UDP IOCP 接收环（多槽 outstanding WSARecvFrom，取代旧单缓冲） ----
    // nl_server_start 分配（仅 UDP），nl_server_destroy 在 fd 关闭后释放 ——
    // closesocket 会以 aborted 完成写回各槽 OVERLAPPED，故槽位必须活到 fd 关闭之后。
    nl_udp_slot_t* udp_slots;
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

/* 日志系统已下沉到 src/common/nl_logsys.c（第193轮）：
 * 级别状态、格式化输出（毫秒时间戳+线程ID）、callback 分发、
 * debug 模式、文件落盘/轮转统一由平台无关实现提供，
 * 本文件的业务日志调用点直接使用 nl_log*()。 */

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
    // 仅需解析首行(METHOD PATH VERSION)：只复制首行而非整个头部，且优先用栈缓冲，
    // 消除每个请求一次的 malloc + 全头部 memcpy
    size_t line_len = 0;
    while (line_len < header_len && data[line_len] != '\n') line_len++;
    if (line_len > 0 && data[line_len - 1] == '\r') line_len--;
    char stack_line[256];
    char* line;
    if (line_len < sizeof(stack_line)) {
        line = stack_line;
    } else {
        line = (char*)malloc(line_len + 1);
        if (!line) return;
    }
    memcpy(line, data, line_len);
    line[line_len] = '\0';

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

    if (line != stack_line) free(line);
}

// ---- TCP/HTTP/WEBSOCKET 全 IOCP 完成处理段（取代旧 worker_dispatch）----
#ifndef SO_UPDATE_ACCEPT_CONTEXT
#define SO_UPDATE_ACCEPT_CONTEXT 0x2000
#endif
// AcceptEx 的 OVERLAPPED / 地址缓冲 / 字节数变量必须持续到完成 —— 异步完成时
// 内核会写回 *lpdwBytesReceived 与地址数据，绝不可指向 post 后即失效的栈变量
// （曾因栈上 dummy 在 on_accept 返回后被写回而损坏调用方栈）。
// 同一时刻只 post 一个 AcceptEx，单一静态上下文安全（模式同 Web 引擎 g_pr_accept_ctx）。
// 地址缓冲不可省略为 NULL+0：本机实测返回 WSAEFAULT(10014)；按 MSDN 提供
// local/remote 各 sizeof(sockaddr_in)+16 的双份缓冲。
#define NL_ACCEPT_ADDR_LEN ((DWORD)(sizeof(struct sockaddr_in) + 16))
static OVERLAPPED g_server_accept_ctx;
static DWORD g_server_accept_bytes;
static char g_server_accept_addrbuf[2 * (sizeof(struct sockaddr_in) + 16)];

// post 一次 AcceptEx；成功或 ERROR_IO_PENDING 返回 0，其它返回 -1
static int nl_server_post_accept(nl_server_t* server) {
    memset(&g_server_accept_ctx, 0, sizeof(g_server_accept_ctx));
    if (!AcceptEx(server->fd, server->accept_client, g_server_accept_addrbuf, 0,
                  NL_ACCEPT_ADDR_LEN, NL_ACCEPT_ADDR_LEN, &g_server_accept_bytes,
                  (LPOVERLAPPED)&g_server_accept_ctx) &&
        WSAGetLastError() != ERROR_IO_PENDING) {
        return -1;
    }
    return 0;
}

// 重投 AcceptEx（调用方必须持有 conn_lock）：accept_client 为 INVALID 且
// running 时才创建+post。post 失败置 INVALID —— AcceptEx 是单 outstanding
// 模型，post 失败后没有任何完成事件会再来，不自愈即永久停止 accept；
// 由 nl_server_iocp_step 的 WAIT_TIMEOUT 分支周期重试。
// 锁内双检查保证同一时刻至多一个 outstanding AcceptEx（静态 g_server_accept_ctx
// 不可双投），与 on_accept 的重投路径互斥。
static void nl_server_repost_accept_locked(nl_server_t* server) {
    if (server->running != 1 || server->accept_client != INVALID_SOCKET) return;
    server->accept_client = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (server->accept_client == INVALID_SOCKET) return;
    if (nl_server_post_accept(server) != 0) {
        closesocket(server->accept_client);
        server->accept_client = INVALID_SOCKET;
    }
}

// 持有 conn_lock 时关闭槽位 socket（幂等；fd 置 INVALID 防双重关闭误伤复用句柄）
static void nl_server_conn_close_locked(nl_server_conn_t* c) {
    if (c->fd != INVALID_SOCKET) {
        closesocket(c->fd);
        c->fd = INVALID_SOCKET;
    }
}

// 持有 conn_lock 时释放槽位（无 pending I/O 前提下才可立即回收）
static void nl_server_conn_release_locked(nl_server_t* server, int idx) {
    nl_server_conn_t* c = &server->conns[idx];
    nl_server_conn_close_locked(c);
    c->state = NL_CONN_FREE;
    c->len = 0;
    // 惰性接收缓冲：连接关闭即回收，槽位回到 cap=0；下次 post_recv 会按需重新分配。
    // 释放安全：每槽位同一时刻至多一个 pending WSARecv，进入本函数时该 recv 已完成，
    // 内核不再引用 c->buf（fail 路径无 pending I/O；完成路径即该 recv 的完成事件）。
    free(c->buf);
    c->buf = NULL;
    c->cap = 0;
    if (server->conn_count > 0) server->conn_count--;
}

// post WSARecv 到槽位缓冲剩余空间；调用方必须持有 conn_lock。
// 成功返回 0（含 WSA_IO_PENDING 正常异步投递），失败返回 -1。
static int nl_server_post_recv_locked(nl_server_t* server, int idx) {
    nl_server_conn_t* c = &server->conns[idx];
    // 缓冲写满时倍增（WSARecv 直接写入 c->buf+len，扩容须在 post 前完成）
    if (c->cap - c->len == 0) {
        size_t ncap = c->cap ? c->cap * 2 : BUFFER_SIZE;
        char* nb = (char*)realloc(c->buf, ncap);
        if (!nb) return -1;
        c->buf = nb;
        c->cap = ncap;
    }
    WSABUF wb;
    wb.buf = c->buf + c->len;
    wb.len = (ULONG)(c->cap - c->len);
    DWORD flags = 0, dummy = 0;
    memset(&c->ov, 0, sizeof(c->ov));
    if (WSARecv(c->fd, &wb, 1, &dummy, &flags, &c->ov, NULL) == SOCKET_ERROR) {
        if (WSAGetLastError() != WSA_IO_PENDING) return -1;
    }
    return 0;
}

// 槽位分配失败/关联失败路径：持锁关闭并回收（此刻无 pending I/O）
static void nl_server_conn_fail_locked(nl_server_t* server, int idx) {
    nl_server_conn_release_locked(server, idx);
}

// 空闲扫描（仅完成线程 0 调用）：超时连接 closesocket 触发 pending WSARecv
// 以 aborted 完成，槽位转 CLOSING 由完成处理段回收 —— 保证槽位不会在
// pending OVERLAPPED 存活期间被复用。
static void nl_server_idle_scan(nl_server_t* server) {
    ULONGLONG now = GetTickCount64();
    EnterCriticalSection(&server->conn_lock);
    for (int i = 0; i < NL_SERVER_MAX_CONNS; i++) {
        nl_server_conn_t* c = &server->conns[i];
        if (c->state == NL_CONN_ACTIVE && now - c->last_active > NL_SERVER_IDLE_MS) {
            nl_server_conn_close_locked(c);
            c->state = NL_CONN_CLOSING;
        }
    }
    LeaveCriticalSection(&server->conn_lock);
}

// WSARecv 完成处理：active → processing（此阶段无 pending I/O，允许并发完成线程
// 各自处理不同连接，handler+send 不全局串行化 —— 对齐旧 worker 池并发语义）。
static void nl_server_on_recv(nl_server_t* server, int idx, int ok, DWORD bytes_xfer) {
    if (idx < 0 || idx >= NL_SERVER_MAX_CONNS) return;
    nl_server_conn_t* c = &server->conns[idx];

    EnterCriticalSection(&server->conn_lock);
    if (c->state == NL_CONN_CLOSING || c->state == NL_CONN_FREE) {
        // 空闲扫描/停止流程已关闭 socket，本完成事件仅用于回收槽位
        if (c->state == NL_CONN_CLOSING) nl_server_conn_release_locked(server, idx);
        LeaveCriticalSection(&server->conn_lock);
        return;
    }
    c->state = NL_CONN_PROCESSING;
    c->last_active = GetTickCount64();
    LeaveCriticalSection(&server->conn_lock);

    int close_conn = 0;
    if (!ok || bytes_xfer == 0) {
        close_conn = 1;   // 传输错误或对端优雅关闭
    } else {
        c->len += (size_t)bytes_xfer;   // 数据已由 WSARecv 直写入 c->buf
        if (server->handler) {
            char path[1024] = {0};
            nl_http_method_t method = NL_METHOD_GET;
            const char* body = NULL;
            size_t body_size = 0;
            parse_http_request(c->buf, c->len, path, sizeof(path), &method, &body, &body_size);

            char* response = NULL;
            size_t response_len = 0;
            server->handler(path, method, body, body_size, &response, &response_len, server->user_data);

            // BUG-002: 契约要求 *response 必须堆分配（malloc/calloc/realloc）。
            // 服务端在此处 free(response)：若 handler 传字符串字面量/静态数组
            // 会触发 free(): invalid pointer。无响应时 handler 须置 *response = NULL。
            if (response && response_len > 0) {
                size_t total = 0;
                while (total < response_len) {
                    int w = (int)send(c->fd, response + total, (int)(response_len - total), 0);
                    if (w <= 0) break;
                    total += (size_t)w;
                }
                free(response);
            }
        }
        // 无 handler 时丢弃已收数据（旧实现会无界累积直至空闲断开，改为有界）
        c->len = 0;
    }

    // 重 post 或回收（持锁；扫描线程只会触碰 ACTIVE 槽位，PROCESSING 期间不会被干预）
    EnterCriticalSection(&server->conn_lock);
    if (!close_conn && server->running == 1 && c->state == NL_CONN_PROCESSING &&
        c->fd != INVALID_SOCKET) {
        c->last_active = GetTickCount64();
        if (nl_server_post_recv_locked(server, idx) == 0) {
            c->state = NL_CONN_ACTIVE;
            LeaveCriticalSection(&server->conn_lock);
            return;
        }
    }
    nl_server_conn_release_locked(server, idx);
    LeaveCriticalSection(&server->conn_lock);
}

// AcceptEx 完成处理：新连接关联 IOCP（key=idx+1）→ post 首个 WSARecv →
// 预创建下一个备用 socket 并重 post AcceptEx（模式同 Web 引擎）。
static void nl_server_on_accept(nl_server_t* server) {
    SOCKET new_client = server->accept_client;
    server->accept_client = INVALID_SOCKET;

    if (new_client != INVALID_SOCKET) {
        if (server->running != 1) {
            closesocket(new_client);
        } else {
            int idx = -1;
            EnterCriticalSection(&server->conn_lock);
            for (int i = 0; i < NL_SERVER_MAX_CONNS; i++) {
                if (server->conns[i].state == NL_CONN_FREE) {
                    nl_server_conn_t* c = &server->conns[i];
                    c->fd = new_client;
                    c->len = 0;
                    c->last_active = GetTickCount64();
                    c->state = NL_CONN_ACTIVE;
                    server->conn_count++;
                    idx = i;
                    break;
                }
            }
            if (idx >= 0) {
                // SO_UPDATE_ACCEPT_CONTEXT：保留旧 accept() 语义（getsockname/
                // getpeername 在 accepted socket 上可用）
                setsockopt(new_client, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                           (const char*)&server->fd, sizeof(server->fd));
                set_tcp_nodelay(new_client, 1);
                set_tcp_keepalive(new_client, 1, 7200, 75, 9);
                if (CreateIoCompletionPort((HANDLE)new_client, server->iocp_handle,
                                           (ULONG_PTR)(idx + 1), 0) == NULL ||
                    nl_server_post_recv_locked(server, idx) != 0) {
                    nl_server_conn_fail_locked(server, idx);
                }
            }
            LeaveCriticalSection(&server->conn_lock);
            if (idx < 0) {
                closesocket(new_client);   // 槽位耗尽，直接拒绝
            }
        }
    }

    // 预先创建下一个备用 socket，post 下一次 AcceptEx；
    // 失败置 INVALID，由 iocp_step 的 WAIT_TIMEOUT 分支自愈重投
    EnterCriticalSection(&server->conn_lock);
    nl_server_repost_accept_locked(server);
    LeaveCriticalSection(&server->conn_lock);
}

// 一次 GQCS 取包 + 完成分发；返回 -1 表示 iocp 已损坏需退出线程。
// do_idle_scan：仅完成线程 0 传 1（每 ~200ms 超时 tick 扫一次空闲连接）。
static int nl_server_iocp_step(nl_server_t* server, DWORD timeout_ms, int do_idle_scan) {
    DWORD bytes_xfer = 0;
    ULONG_PTR key = 0;
    LPOVERLAPPED pov = NULL;
    BOOL ok = GetQueuedCompletionStatus(server->iocp_handle, &bytes_xfer, &key, &pov, timeout_ms);
    if (!ok) {
        DWORD gle = GetLastError();
        if (gle == WAIT_TIMEOUT) {
            if (do_idle_scan) nl_server_idle_scan(server);
            // AcceptEx 停摆自愈：上次 post 失败（WSASocket 耗尽/AcceptEx 同步错误）
            // 后 accept_client 为 INVALID 且无完成事件驱动，超时分支补投
            EnterCriticalSection(&server->conn_lock);
            nl_server_repost_accept_locked(server);
            LeaveCriticalSection(&server->conn_lock);
            return 0;
        }
        if (pov == NULL) {
            // 非 I/O 完成的失败（iocp/句柄损坏）→ 退出线程
            if (gle == ERROR_ABANDONED_WAIT_0 || gle == ERROR_INVALID_HANDLE) return -1;
            return 0;
        }
        // I/O 完成但带错误（含 ERROR_OPERATION_ABORTED）→ 按关闭路径分发
        bytes_xfer = 0;
        ok = FALSE;
    }
    if (key == NL_SERVER_LISTEN_KEY) {
        nl_server_on_accept(server);
        return 0;
    }
    nl_server_on_recv(server, (int)key - 1, ok ? 1 : 0, bytes_xfer);
    return 0;
}

// 额外完成消费线程（listener 为线程 0；running=0 后最多一个 200ms 超时即退出）
static DWORD WINAPI nl_server_iocp_consumer(LPVOID lpParam) {
    nl_server_t* server = (nl_server_t*)lpParam;
    while (server->running == 1) {
        if (nl_server_iocp_step(server, 200, 0) != 0) break;
    }
    return 0;
}

static DWORD WINAPI listener_thread(LPVOID lpParam) {
    nl_server_t* server = (nl_server_t*)lpParam;

    if (server->protocol == NL_PROTO_UDP) {
        // ---- UDP 接收环（多槽 outstanding WSARecvFrom） ----
        // 前置：nl_server_create 已 CreateIoCompletionPort 把 UDP fd 挂进
        // server->iocp_handle（completion key = (ULONG_PTR)server->fd）；
        // nl_server_start 已分配 server->udp_slots（NL_UDP_RCV_SLOTS 槽）。
        //
        // 相比旧单缓冲：handler 同步执行期间仍有 NL_UDP_RCV_SLOTS-1 个
        // WSARecvFrom 由内核异步填充，突发 datagram 先落进空闲槽位，不再只靠
        // 内核 socket 队列兜底 —— 消除 handler 慢时的丢包风险（吸收上限 =
        // 16 个已缓冲 datagram + socket 队列）。
        //
        // 策略：有 handler → 一次性 post 全部槽位；无 handler → 不 post，
        //        GQCS 等 timeout 自然睡眠，零 CPU。
        if (server->udp_slots && (server->udp_handler || server->udp_handler_v2)) {
            for (int i = 0; i < NL_UDP_RCV_SLOTS; i++) {
                nl_udp_slot_t* slot = &server->udp_slots[i];
                memset(&slot->ov, 0, sizeof(slot->ov));
                slot->wb.buf = slot->buf;
                slot->wb.len = BUFFER_SIZE;
                DWORD wsa_flags = 0;
                DWORD dummy_bytes = 0;
                int addr_len = (int)sizeof(slot->addrbuf);
                int rc = WSARecvFrom(server->fd, &slot->wb, 1, &dummy_bytes, &wsa_flags,
                                     (struct sockaddr*)slot->addrbuf, &addr_len,
                                     &slot->ov, NULL);
                // WSA_IO_PENDING = 正常异步投递；0 = 立即完成（少见）
                if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                    nl_log(NL_LOG_ERROR, "Windows UDP IOCP: slot %d 首次 WSARecvFrom 失败 gle=%d",
                                i, WSAGetLastError());
                }
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
                nl_log(NL_LOG_ERROR, "Windows UDP IOCP: GetQueuedCompletionStatus 失败 gle=%lu", gle);
                break;
            }

            // completion key 应该就是 server->fd（nl_server_create 里挂进 iocp 时设的）
            if ((SOCKET)completion_key != server->fd) continue;

            // 完成事件指针反查槽位（ov 为首成员，指针等值）
            nl_udp_slot_t* slot = (nl_udp_slot_t*)pov;
            if (!server->udp_slots ||
                slot < server->udp_slots ||
                slot >= server->udp_slots + NL_UDP_RCV_SLOTS) {
                continue;   // 非接收环完成事件（理论上不应出现）
            }

            if (bytes_xfer > 0) {
                // BUG-201：peer 地址从该槽位的 addrbuf 解出来
                char peer_addr[INET6_ADDRSTRLEN] = "0.0.0.0";
                int  peer_port = 0;
                struct sockaddr_in* sin = (struct sockaddr_in*)slot->addrbuf;
                if (sin->sin_family == AF_INET) {
                    inet_ntop(AF_INET, &sin->sin_addr, peer_addr, sizeof(peer_addr));
                    peer_port = (int)ntohs(sin->sin_port);
                }

                // 丢包修复：先拷出 datagram 并【立即重投该槽】，再同步调用 handler。
                // 旧序（handler 先跑、函数末尾才重投）在同步 handler 执行期间该槽不可收，
                // 突发流量下 16 槽环 + socket 队列被打满即丢包（实测发61万/丢约千）。
                // 先重投保证 handler 执行期间仍保持全部 NL_UDP_RCV_SLOTS 个 outstanding
                // WSARecvFrom，内核持续异步填充空闲槽位。
                size_t dlen = (size_t)bytes_xfer;
                // 第205轮性能：datagram 拷贝改用栈缓冲（dlen ≤ 槽位 wb.len =
                // BUFFER_SIZE，由 WSARecvFrom 保证），消除每包一次 malloc/free；
                // 语义不变——缓冲仍仅在本次 handler 调用期间有效，且栈缓冲
                // 不再有 malloc 失败导致丢弃该包的分支。
                char copy[BUFFER_SIZE];
                memcpy(copy, slot->buf, dlen);

                // handler 仍在 → 立即重投（addrbuf/buf 归还内核；peer 已解到局部变量）
                // handler 被动态清空 → 不重投，槽位耗尽后 GQCS 自然睡眠
                if (server->running == 1 && (server->udp_handler || server->udp_handler_v2)) {
                    memset(&slot->ov, 0, sizeof(slot->ov));
                    slot->wb.buf = slot->buf;
                    slot->wb.len = BUFFER_SIZE;
                    DWORD wsa_flags2 = 0;
                    DWORD dummy_bytes2 = 0;
                    int addr_len2 = (int)sizeof(slot->addrbuf);
                    int rc = WSARecvFrom(server->fd, &slot->wb, 1, &dummy_bytes2, &wsa_flags2,
                                         (struct sockaddr*)slot->addrbuf, &addr_len2,
                                         &slot->ov, NULL);
                    if (rc == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                        nl_log(NL_LOG_ERROR, "Windows UDP IOCP: slot 重 post WSARecvFrom 失败 gle=%d",
                                    WSAGetLastError());
                    }
                }

                // 拷贝缓冲只在本次 handler 调用期间有效（与旧 slot->buf 契约一致：
                // handler 不得在调用后保留指针）
                if (server->udp_handler_v2) {
                    server->udp_handler_v2(copy, dlen, peer_addr, peer_port, server->user_data);
                } else if (server->udp_handler) {
                    server->udp_handler(copy, dlen, server->user_data);
                }
            } else {
                // bytes_xfer == 0 + gle==ERROR_OPERATION_ABORTED 常见于 server 关闭
                // 非 abort 的 0 长度 datagram 理论上合法但罕见
            }
        }
        return 0;
    }

    // ---- TCP / HTTP / WEBSOCKET 路径：全 IOCP（彻底取代 worker 池） ----
    // listener 自身即完成线程 0：预 post 首个 AcceptEx → GQCS 完成主循环；
    // concurrency-1 个额外消费线程跑同一完成循环（concurrency 即 IOCP 线程数）。
    server->accept_client = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (server->accept_client == INVALID_SOCKET) {
        // 瞬时失败不再退出线程：WAIT_TIMEOUT 自愈分支会周期重试
        nl_log(NL_LOG_ERROR, "Windows: WSASocket (accept) failed: %d，等待超时自愈", WSAGetLastError());
    }
    if (nl_server_post_accept(server) != 0) {
        // 初始 post 失败不再直接退出线程：置 INVALID 后由 WAIT_TIMEOUT 分支
        // 周期自愈补投，避免一次瞬时失败（句柄耗尽等）导致服务永久无 accept
        nl_log(NL_LOG_ERROR, "Windows: AcceptEx initial post failed: %d，等待超时自愈", WSAGetLastError());
        closesocket(server->accept_client);
        server->accept_client = INVALID_SOCKET;
    }

    // 额外完成消费线程（失败仅降级线程数，不影响正确性）
    {
        int target = (server->concurrency >= 1 && server->concurrency <= MAX_CONCURRENCY)
                     ? server->concurrency : DEFAULT_CONCURRENCY;
        server->io_threads = (HANDLE*)calloc((size_t)target, sizeof(HANDLE));
        for (int t = 1; server->io_threads && t < target; t++) {
            HANDLE th = CreateThread(NULL, 0, nl_server_iocp_consumer, server, 0, NULL);
            if (!th) {
                nl_log(NL_LOG_ERROR, "Windows: failed to create IOCP thread %d", t);
                break;
            }
            server->io_threads[server->num_io_threads++] = th;
        }
    }

    while (server->running == 1) {
        if (nl_server_iocp_step(server, 200, 1) != 0) {
            nl_log(NL_LOG_ERROR, "Windows: IOCP handle broken, listener exiting");
            break;
        }
    }

    // join 额外消费线程（running=0 后最多一个 GQCS 超时 200ms 即退出；
    // join 完成前不得释放任何槽位数组）
    for (int t = 0; t < server->num_io_threads; t++) {
        if (server->io_threads[t]) {
            WaitForSingleObject(server->io_threads[t], 5000);
            CloseHandle(server->io_threads[t]);
        }
    }
    free(server->io_threads);
    server->io_threads = NULL;
    server->num_io_threads = 0;

    // 关闭残留连接与备用 accept socket（pending 完成事件随 iocp 关闭丢弃；
    // 槽位数组在 nl_server_stop 中 CloseHandle(iocp) 之后才释放，无 UAF）
    if (server->conns && server->conn_lock_init) {
        EnterCriticalSection(&server->conn_lock);
        for (int i = 0; i < NL_SERVER_MAX_CONNS; i++) {
            if (server->conns[i].state != NL_CONN_FREE) {
                nl_server_conn_close_locked(&server->conns[i]);
                server->conns[i].state = NL_CONN_FREE;
                server->conns[i].len = 0;
                if (server->conn_count > 0) server->conn_count--;
            }
        }
        LeaveCriticalSection(&server->conn_lock);
    }
    if (server->accept_client != INVALID_SOCKET) {
        closesocket(server->accept_client);
        server->accept_client = INVALID_SOCKET;
    }

    return 0;
}

// IOCP 资源拆卸（幂等）：关闭 iocp 先于释放槽位数组 —— CloseHandle 丢弃
// 队列中未取走的完成事件，此后内核不再引用槽位中的 OVERLAPPED，可安全 free。
// 调用前提：listener 与全部完成线程已 join（无用户线程再访问 conns）。
static void server_iocp_cleanup(nl_server_t* server) {
    if (server->accept_client != INVALID_SOCKET) {
        closesocket(server->accept_client);
        server->accept_client = INVALID_SOCKET;
    }
    if (server->iocp_handle) {
        CloseHandle(server->iocp_handle);
        server->iocp_handle = NULL;
    }
    if (server->conns) {
        for (int i = 0; i < NL_SERVER_MAX_CONNS; i++) {
            if (server->conns[i].fd != INVALID_SOCKET) closesocket(server->conns[i].fd);
            free(server->conns[i].buf);
        }
        free(server->conns);
        server->conns = NULL;
    }
    if (server->conn_lock_init) {
        DeleteCriticalSection(&server->conn_lock);
        server->conn_lock_init = 0;
    }
    free(server->io_threads);
    server->io_threads = NULL;
    server->num_io_threads = 0;
    server->conn_count = 0;
}

nl_server_t* nl_server_create(nl_protocol_t protocol, int port) {
    if (init_winsock() != 0) return NULL;
    
    nl_server_t* server = calloc(1, sizeof(nl_server_t));
    if (!server) return NULL;
    
    server->protocol = protocol;
    server->port = port;
    server->concurrency = 0;
    server->accept_client = INVALID_SOCKET;

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
    
    // completion key：UDP = socket 句柄（UDP 读循环按此校验，见 listener_thread）；
    // TCP = NL_SERVER_LISTEN_KEY(0)，AcceptEx 完成事件凭 key=0 识别（conn key = idx+1 ≥ 1）
    CreateIoCompletionPort((HANDLE)server->fd, server->iocp_handle,
                           (protocol == NL_PROTO_UDP) ? (ULONG_PTR)server->fd
                                                      : NL_SERVER_LISTEN_KEY, 0);
    
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
    
    nl_log(NL_LOG_INFO, "Windows: Server created on port %d (IOCP)", port);
    return server;
}

void nl_server_destroy(nl_server_t* server) {
    if (!server) return;
    if (server->thread_handle) {
        // Join listener thread BEFORE closing the listening socket:
        // the listener blocks on GetQueuedCompletionStatus()/recvfrom() on
        // server->fd, so closing fd first makes the wait never terminate -> deadlock.
        nl_server_stop(server);
    }
    // 兜底：未 start 或 stop 已清理时以下均为 NULL/INVALID（幂等）
    server_iocp_cleanup(server);
    if (server->fd != INVALID_SOCKET) closesocket(server->fd);
    if (server->udp_slots) {            // stop 未走到时的兜底释放（幂等）
        free(server->udp_slots);
        server->udp_slots = NULL;
    }
    free(server);
}

int nl_server_start(nl_server_t* server) {
    if (!server) return NL_EINVAL;

    int io_target = 0;

    if (server->protocol == NL_PROTO_TCP || server->protocol == NL_PROTO_HTTP ||
        server->protocol == NL_PROTO_WEBSOCKET) {
        if (listen(server->fd, SOMAXCONN) == SOCKET_ERROR) {
            nl_log(NL_LOG_ERROR, "Windows: listen() failed: %d", WSAGetLastError());
            return NL_ERROR;
        }

        // 全 IOCP 初始化：预分配连接槽位表 + 完成处理临界区。
        // 接收缓冲改为惰性分配：槽位仅需 fd=INVALID_SOCKET（calloc 已置 buf=NULL/cap=0/state=FREE），
        // 首次 post_recv 时按需 malloc(BUFFER_SIZE)，连接释放时回收 —— 空载不再常驻 1024×8KB。
        server->conns = (nl_server_conn_t*)calloc(NL_SERVER_MAX_CONNS, sizeof(nl_server_conn_t));
        if (!server->conns) {
            nl_log(NL_LOG_ERROR, "Windows: failed to allocate IOCP connection slots");
            return NL_ERROR;
        }
        for (int i = 0; i < NL_SERVER_MAX_CONNS; i++) {
            server->conns[i].fd = INVALID_SOCKET;
        }
        InitializeCriticalSection(&server->conn_lock);
        server->conn_lock_init = 1;
        server->conn_count = 0;
        io_target = (server->concurrency >= 1 && server->concurrency <= MAX_CONCURRENCY)
                    ? server->concurrency : DEFAULT_CONCURRENCY;
    } else if (server->protocol == NL_PROTO_UDP) {
        // UDP 接收环槽位：必须在 listener 启动前就绪（listener 首先 post 全部槽位）
        if (!server->udp_slots) {
            server->udp_slots = (nl_udp_slot_t*)calloc(NL_UDP_RCV_SLOTS, sizeof(nl_udp_slot_t));
            if (!server->udp_slots) {
                nl_log(NL_LOG_ERROR, "Windows: failed to allocate UDP receive ring slots");
                return NL_ERROR;
            }
            // 放大 UDP socket 内核接收缓冲：配合"先重投再 handler"的接收环，
            // 突发 datagram 先落内核队列，减少槽环+队列同时打满导致的丢包。
            // 仅在当前缓冲偏小(<1MB)时提升，不覆盖用户经 NL_OPT_SO_RCVBUF 的显式配置。
            {
                int cur_rcvbuf = 0; int cb = (int)sizeof(cur_rcvbuf);
                if (getsockopt(server->fd, SOL_SOCKET, SO_RCVBUF, (char*)&cur_rcvbuf, &cb) != SOCKET_ERROR &&
                    cur_rcvbuf < 1024 * 1024) {
                    int udp_rcvbuf = 4 * 1024 * 1024;
                    setsockopt(server->fd, SOL_SOCKET, SO_RCVBUF, (const char*)&udp_rcvbuf, sizeof(udp_rcvbuf));
                }
            }
        }
    }

    InterlockedExchange(&server->running, 1);
    server->thread_handle = CreateThread(NULL, 0, listener_thread, server, 0, NULL);
    if (!server->thread_handle) {
        InterlockedExchange(&server->running, 0);
        server_iocp_cleanup(server);
        nl_log(NL_LOG_ERROR, "Windows: failed to create listener thread");
        return NL_ERROR;
    }

    nl_log(NL_LOG_INFO, "Windows: Server started (IOCP), socket=%lu, io_threads=%d",
                (ULONG)server->fd, io_target);
    return NL_OK;
}

void nl_server_stop(nl_server_t* server) {
    if (!server) return;
    // running=0 后，listener/额外完成线程最多一个 GQCS 超时（200ms）即退出
    InterlockedExchange(&server->running, 0);

    if (server->thread_handle) {
        WaitForSingleObject(server->thread_handle, INFINITE);
        CloseHandle(server->thread_handle);
        server->thread_handle = NULL;
    }
    // listener 已 join（残留连接在其退出段关闭）；此后拆卸 IOCP 资源与监听 socket
    server_iocp_cleanup(server);
    if (server->fd != INVALID_SOCKET) {
        closesocket(server->fd);
        server->fd = INVALID_SOCKET;
    }
    // UDP 接收环：closesocket 会使 outstanding WSARecvFrom 以 aborted 完成写回
    // 槽位 OVERLAPPED，故必须在 fd 关闭后才能释放（幂等）
    if (server->udp_slots) {
        free(server->udp_slots);
        server->udp_slots = NULL;
    }
    nl_log(NL_LOG_INFO, "Windows: Server stopped");
}

int nl_server_set_concurrency(nl_server_t* server, int threads) {
    if (!server) return NL_EINVAL;
    if (threads <= 0 || threads > MAX_CONCURRENCY) return NL_EINVAL;
    server->concurrency = threads;
    return NL_OK;
}

int nl_server_start_workers(nl_server_t* server) {
    if (!server) return NL_EINVAL;
    /* Windows 服务端是纯 IOCP（listener 线程自身参与完成处理），没有 worker 池 */
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
    nl_log(NL_LOG_DEBUG, "Windows: Client created (socket=%lu)", (ULONG)client->fd);
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
            nl_log(NL_LOG_ERROR, "Windows: connect() failed: %d", err);
            return NL_ECONNECT;
        }
    }
    
    client->connected = 1;
    nl_log(NL_LOG_INFO, "Windows: Client connected to %s:%d", host, port);
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
    
    nl_log(NL_LOG_INFO, "Windows: Config loaded from %s", path);
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

/* nl_log_set_level / nl_log_set_callback 已下沉到 src/common/nl_logsys.c */

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

/* 第192轮：以下文件服务辅助已下沉 src/common/nl_filesrv.c（三平台一份实现）。
 * 保留旧名字的宏别名，全部调用点零改动。 */
#define get_file_extension nl_filesrv_ext
#define get_mime_type      nl_filesrv_mime
#define read_file          nl_filesrv_read
#define send_http_response nl_filesrv_send_response
#define send_http_error    nl_filesrv_send_error
#define send_418_response  nl_filesrv_send_teapot
#define is_april_fools_day nl_filesrv_is_teapot_day

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
        size_t rl_len = strcspn(buffer, "\r\n");
        if (nl_px_parse_request_line(buffer, rl_len, method, sizeof(method),
                                     path, sizeof(path), protocol, sizeof(protocol)) != 0) {
            send_http_error(client, 400, "Bad Request");
            closesocket(client);
            continue;
        }
        
        /* 第192轮：RFC 3986 端点规范化（剥 query/fragment、percent-decode、
         * dot-segment 归一）。非法端点 400，越根 403。 */
        char norm_path[4096];
        int uri_rc = nl_uri_normalize(path, norm_path, sizeof(norm_path));
        if (uri_rc != NL_URI_OK) {
            int esc = (uri_rc == NL_URI_ESCAPES);
            send_http_error(client, esc ? 403 : 400,
                            esc ? "Forbidden" : "Bad Request");
            closesocket(client);
            continue;
        }

        if (server->enable_easter_egg && is_april_fools_day()) {
            const char* tea_path = "/tea";
            if (strncmp(norm_path, tea_path, strlen(tea_path)) == 0) {
                send_418_response(client);
                closesocket(client);
                continue;
            }
        }

        /* 物理文件隔离：请求路径 → 磁盘的拼接统一经 nl_uri_resolve_file */
        char filepath[8192];
        int fr = nl_uri_resolve_file(server->directory, norm_path,
                                     filepath, sizeof(filepath));
        if (fr != NL_URI_OK) {
            int deny403 = (fr == NL_URI_ESCAPES || fr == NL_URI_EROOT);
            send_http_error(client, deny403 ? 403 : 400,
                            deny403 ? "Forbidden" : "Bad Request");
            closesocket(client);
            continue;
        }

        DWORD attrs = GetFileAttributesA(filepath);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // BUG-304: index_path 与 filepath 同容量（8192），拼接与回拷容量一致，
            // GCC 推导不会越界，消除 -Wformat-truncation
            char index_path[8192];
            snprintf(index_path, sizeof(index_path), "%s/%s",
                     filepath, server->index_file);
            /* index_file 来自配置，拼接后仍做一次根目录隔离二次校验 */
            if (GetFileAttributesA(index_path) != INVALID_FILE_ATTRIBUTES &&
                nl_uri_is_under(server->directory, index_path)) {
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
    
    nl_log(NL_LOG_INFO, "File server started on port %d, serving %s", server->port, server->directory);
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
    
    nl_log(NL_LOG_INFO, "File server stopped");
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
        size_t rl_len = strcspn(buffer, "\r\n");
        if (nl_px_parse_request_line(buffer, rl_len, method_str, sizeof(method_str),
                                     path, sizeof(path), protocol, sizeof(protocol)) != 0) {
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
        else if (strcmp(method_str, "TRACE") == 0) method = NL_METHOD_TRACE;     /* 第205轮 */
        else if (strcmp(method_str, "CONNECT") == 0) method = NL_METHOD_CONNECT; /* 第205轮 */
        
        char* body_start = strstr(buffer, "\r\n\r\n");
        const char* body = NULL;
        size_t body_size = 0;
        
        if (body_start) {
            body = body_start + 4;
            body_size = (size_t)(received - (body - buffer));
        }

        /* 第192轮：RFC 3986 端点规范化；路由匹配/静态回退用 norm_path，
         * handler 仍收原始 path（既有 handler 会自行从 path 截取 query）。 */
        char norm_path[4096];
        int uri_rc = nl_uri_normalize(path, norm_path, sizeof(norm_path));
        if (uri_rc != NL_URI_OK) {
            int esc = (uri_rc == NL_URI_ESCAPES);
            send_http_error(client, esc ? 403 : 400,
                            esc ? "Forbidden" : "Bad Request");
            closesocket(client);
            continue;
        }

        struct nl_route* route = router_find_route(rs->router, norm_path, method);
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
            /* 第192轮：静态目录回退统一走物理文件隔离解析器 */
            char filepath[8192];
            int fr = nl_uri_resolve_file(rs->router->static_dir, norm_path,
                                         filepath, sizeof(filepath));

            if (fr == NL_URI_OK) {
                DWORD attrs = GetFileAttributesA(filepath);
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    char index_path[8192];
                    snprintf(index_path, sizeof(index_path), "%s/index.html", filepath);
                    if (GetFileAttributesA(index_path) != INVALID_FILE_ATTRIBUTES &&
                        nl_uri_is_under(rs->router->static_dir, index_path)) {
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
            if (fr != NL_URI_OK) {
                int deny403 = (fr == NL_URI_ESCAPES || fr == NL_URI_EROOT);
                send_http_error(client, deny403 ? 403 : 400,
                                deny403 ? "Forbidden" : "Bad Request");
            } else {
                send_http_error(client, 404, "Not Found");
            }
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
    nl_log(NL_LOG_INFO, "Router server started on port %d", port);
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
    NL_ROUTE_TYPE_PROXY = 3,      // Reverse proxy (upstream http/https/tcp)
    NL_ROUTE_TYPE_DIR = 4         // 第203轮：目录/文件端点（端点即根）
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
    unsigned char is_prefix;      // 第195轮：1=目录（前缀）路由（nginx location 语义）
    char up_uri[256];             // 第195轮：上游 URI 部分（如 "/v1/"，空=路径原样透传）
    struct nl_web_route* next;
} nl_web_route_t;

// ============ 上游 keep-alive 连接池（公共层 nl_px_pool，IOCP 引擎持有）============
// 分桶、探活、回收策略统一在 src/common/nl_px_pool.c；这里只提供平台动作。
// 说明：Windows socket 是 64 位句柄，公共池用 intptr_t 承载（INVALID_SOCKET 映射为 -1）。
// 当前毫秒（GetTickCount64 为系统毫秒计数，用于空闲回收）
static int64_t pr_pool_now_ms(void) {
    return (int64_t)GetTickCount64();
}
// 关闭池中 socket（平台差异集中于此）
static void pr_pool_close_fd(intptr_t fd) { if (fd >= 0) closesocket((SOCKET)fd); }
// 探测空闲上游连接是否仍存活：MSG_PEEK 非阻塞偷看，WSAEWOULDBLOCK=健康
static int pr_pool_probe_alive(intptr_t fd) {
    if (fd < 0) return 0;
    char c;
    int r = recv((SOCKET)fd, &c, 1, MSG_PEEK);
    if (r == 0) return 0;                 // 对端已关闭
    if (r > 0)  return 0;                 // 有残留数据，不复用
    int err = WSAGetLastError();
    return (err == WSAEWOULDBLOCK) ? 1 : 0;
}
static const nl_px_pool_ops g_pr_pool_ops = {
    pr_pool_now_ms, pr_pool_probe_alive, pr_pool_close_fd
};

// 第191轮：AcceptEx 并发 outstanding 数。单 outstanding 时 accept 吞吐 = 1 次
// 完成事件往返，256 并发短连接建连时 backlog 溢出、客户端 connect 排队 45ms；
// 批量预投让内核并行完成握手，accept 不再串行。
#define PR_ACCEPT_PENDING 16
// 第191轮：延迟关闭数组容量（完成事件处理段收集、离开 pr_loop_cs 后统一执行）
#define PR_DEFER_CLOSE_MAX 32

// 第191轮：延迟操作槽 —— 离开 pr_loop_cs 后由 consume loop 执行。
// kind=PR_DEFER_CLOSE：has_plan=1 按 plan 发送响应头+body 后关闭（CONTENT 快路径；
// 第204轮起仅 ka=0 且无 DRAIN 时入队，ka=1 走同步路径发完复用）；
// kind=PR_DEFER_RECV ：post 首包 WSARecv（accept 路径的内核调用移出锁，~150us/次）。
#define PR_DEFER_CLOSE 0
#define PR_DEFER_RECV  1
typedef struct pr_defer {
    int    kind;                // PR_DEFER_*
    SOCKET fd;                  // 待关闭/待 post 的连接
    int    has_plan;            // 1=按 plan 发送（kind=CLOSE）
    nl_px_local_plan plan;      // 内嵌拷贝（head[1024] 内嵌数组，必须整体拷出栈）
    int    idx;                 // kind=RECV：ring slot（cctx/buf 由 consume loop 按 idx 取）
} pr_defer_t;

struct nl_web_server {
    int port;
    SOCKET sock;
    int af;                   // 监听地址族（AF_INET / AF_INET6 双栈）
    int listen_mode;          // -1 = 未设置（读环境变量）；NL_PX_LISTEN_* 见 nl_px_local.h
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
    // ---- 上游 keep-alive 连接池（公共层实现，引擎持有）----
    nl_px_pool* up_pool;     // 空闲上游连接池（NULL=未启用）
    // 响应 framing 解析（每上游连接一份状态，旁路判定响应是否完整可复用）
    nl_px_upstate* pr_up;    // [idx] -> framing 状态（解析状态/CL/chunk/HEAD）
    char   **pr_up_key;      // [idx] -> 池 key（归还时使用，slot 归还即释放）
    // 上游→客户端方向的延迟写：客户端写慢（send WSAEWOULDBLOCK）时按 slot 缓冲待补发字节
    char   **pr_up_pend;     // [idx] -> 待补发缓冲（懒分配，PR_PROXY_BUF_SZ）
    int     *pr_up_pend_len; // [idx] -> 待补发剩余字节数
    int     *pr_up_req_len;  // [idx] -> 异步 connect 前已构造的上游请求字节数（完成后续发）
    HANDLE iocp;             // IOCP 完成端口（NULL=select 回退，非 NULL=IOCP 主路径）
    // 第191轮：AcceptEx 批量 outstanding（单 outstanding 时 accept 吞吐受限，
    // 高并发建连 backlog 溢出）。每槽独立 OVERLAPPED/地址缓冲/预创建 socket。
    void*  pr_accept_ctxs;        // [PR_ACCEPT_PENDING] AcceptEx OVERLAPPED（pr_iocp_ctx_t*）
    char*  pr_accept_addrs;       // [PR_ACCEPT_PENDING] 每槽地址输出缓冲（连续块）
    SOCKET pr_accept_socks[PR_ACCEPT_PENDING];  // 每槽预创建 client socket（INVALID=空闲）
    // 第190轮：ring 满背压 —— AcceptEx 单 outstanding 模型下暂停重投，
    // 已 accept 的连接暂存 pending，拿到空槽后再接管，避免无响应关闭（RST）
    SOCKET pr_pending_client;  // ring 满时暂存的已 accept socket（INVALID=无）
    int    pr_accept_paused;   // 1=accept 已暂停（ring 满），空槽出现时恢复
    char*  pr_accept_addr;   // AcceptEx 输出缓冲（local+remote 地址；NULL 会 WSAEFAULT）
    void*  pr_iocp_client_ctx;  // [cap] 客户端侧 OVERLAPPED 上下文（pr_iocp_ctx_t*）
    void*  pr_iocp_up_ctx;      // [cap] 上游侧 OVERLAPPED 上下文（pr_iocp_ctx_t*）
    void*  pr_iocp_cw_ctx;      // [cap] 客户端写侧 OVERLAPPED 上下文（补发上游→客户端余量）
    // P6/P7 配置字段（与 Linux 对齐；Windows IOCP 引擎暂不启用多实例，仅记字段不生效）
    int engine_threads_cfg;   // P6: 引擎实例数（0=自动单实例；Linux SO_REUSEPORT）
    int max_conns_cfg;        // P6: 总连接预算（0=自动 ncpu*64）
    int idle_timeout_ms;      // P7: 空闲超时 ms（0=默认 30000）
    int force_legacy_pool;    // P5: 强制走 legacy worker 池（0=默认引擎）
    // ---- P6/P7 能力对齐（Windows IOCP 引擎）----
    int64_t *pr_last_active_ms; // [idx] -> 最近活跃毫秒（GetTickCount64 时钟，P7 空闲扫描）
    int64_t *pr_req_drain;     // [idx] -> 第204轮：请求 body 待消费余量（DRAIN 阶段递减）
    DWORD   *pr_conn_id;        // [idx] -> slot 连接代号（slot 复用竞态防护）
    DWORD    pr_conn_id_next;   // 连接代号单调递增计数（从 1 起）
    CRITICAL_SECTION pr_loop_cs; // IOCP 完成处理临界区（P6 多消费线程串行化）
    // 第191轮：AcceptEx 槽位独立锁 —— WSASocket/AcceptEx 内核调用（~300us/次）
    // 不再占 pr_loop_cs，accept 与读路径处理真正并行（嵌套顺序固定 loop→accept）
    CRITICAL_SECTION pr_accept_cs;
    HANDLE  *pr_engine_threads; // P6: 额外 IOCP 消费线程句柄数组（engine_threads_cfg-1 个）
    int pr_engine_thread_count; // 额外消费线程实际创建数
    // HTTPS 上游隧道线程计数：拆卸时须等待归零，避免线程访问已释放的槽位数组
    volatile LONG pr_tls_threads;
    // 第205轮 CONNECT：目标白名单（固定 16 条，不 malloc；host 精确匹配不区分
    // 大小写，port=0 任意端口；空名单 = 拒绝所有 CONNECT，默认安全）
    char connect_allow_host[16][256];
    int  connect_allow_port[16];
    int  connect_allow_cnt;
};
static struct nl_web_server* g_web_servers = NULL;
static CRITICAL_SECTION g_web_servers_mutex;
static int g_auto_cleanup_enabled = 0;

static int g_mutex_initialized = 0;

// 路由 hash 表辅助函数（FNV-1a + 线性探测，桶存 route* 指针，不修改 route->next）
static const int NL_ROUTE_HASH_SIZE = 128;

static unsigned nl_route_hash(const char* path) {
    /* 第192轮：FNV-1a 实现已下沉 src/common/nl_util.c（三平台一份实现） */
    return nl_hash_fnv1a(path) % NL_ROUTE_HASH_SIZE;
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

#ifdef NETLEAF_LEGACY_WORKER_POOL
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
#endif /* NETLEAF_LEGACY_WORKER_POOL */

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
// 异步连接辅助（ConnectEx）与 TLS 包装：定义在文件后段，完成事件分支需前置声明。
static void nl_proxy_resolve_upstream(const nl_web_route_t* route, char* host,
                                      size_t host_size, int* out_port);
/* host:port -> 已解析地址（AF_UNSPEC，支持 IPv6 字面量与域名）；返回地址族，0=失败 */
static int  nl_proxy_fill_addr(const char* host, int port, struct sockaddr_storage* ss);
static int  nl_proxy_start_connect_async(nl_web_server_t* s, int idx, const nl_web_route_t* route);
// HTTPS 上游：把连接移交给 TLS 隧道线程（成功返回 1；失败已回 502/回收槽位，返回 0）
static int  pr_tls_start_tunnel(nl_web_server_t* s, int idx, SOCKET cfd, SOCKET ufd,
                                const char* host, const char* init, int init_len);
// HTTP/2（h2c）：把客户端连接移交给公共层桥接线程（成功返回 1）
static int  pr_h2_start_bridge(nl_web_server_t* s, int idx, SOCKET cfd,
                               const char* init, int init_len, int is_upgrade);

// ---------------------------------------------------------------------------
// 数据驱动反向代理引擎（Windows select 版）
// ---------------------------------------------------------------------------
#define PR_ST_DIR_W   0x01   // 方向：等上游可读（写下游）
#define PR_ST_UP_EOF  0x02   // 上游已 EOF，写尽后关闭
#define PR_ST_KA      0x04   // 第204轮：客户端请求意图 keep-alive（响应头与复用判定依据）
#define PR_ST_DRAIN   0x08   // 第204轮：请求 body 消费阶段（收完按 KA 复用/关闭）
#define PR_ST_TLS     0x40   // HTTPS 上游：已交给隧道线程，引擎不再碰该槽的 fd
#define PR_ST_ACTIVE  0x80   // 该槽活跃
#define PR_PROXY_BUF_SZ 65536

// ---- 上游响应 framing 解析状态（旁路判定响应是否完整可复用）----
#define PR_UP_HDR     0  // 响应头未收全
#define PR_UP_CL      1  // 按 Content-Length 计 body
#define PR_UP_CHUNK   2  // Transfer-Encoding: chunked
#define PR_UP_DONE    3  // 完成，可复用
#define PR_UP_EOF     4  // 不可复用（body 以 EOF 结束 / Connection: close）
#define PR_UP_TUNNEL  5  // 101 已升级为透明隧道（WS），双向裸字节转发，不可复用
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
#define IOCP_EV_UPSTREAM_CONNECT 5  // ConnectEx 异步连接完成（不再阻塞引擎消费线程）
#define IOCP_LISTEN_KEY 0xFFFFFFFFUL  // listen socket completion key 哨兵

// IOCP OVERLAPPED 上下文（嵌 OVERLAPPED，每次 WSARecv/AcceptEx 唯一实例）
typedef struct {
    OVERLAPPED ov;
    int  idx;        // ring slot 索引
    int  ev;         // IOCP_EV_* 事件类型
    SOCKET sock;     // 关联的 socket（AcceptEx 完成时填新 client）
    char *buf;       // 读写缓冲（非 OWN，指向 pr_hbuf[idx] / 临时缓冲）
    DWORD buflen;    // 缓冲长度
    DWORD conn_id;   // post 时的 slot 连接代号：slot 复用后旧连接迟到完成被丢弃
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
    // 分配 slot 即发新连接代号（post WSARecv 前写入 ctx->conn_id）
    if (s->pr_conn_id) s->pr_conn_id[idx] = ++s->pr_conn_id_next;
    return idx;
}

static void pr_free_slot(nl_web_server_t* s, int idx) {
    if (idx < 0 || idx >= s->pr_cap) return;
    if (s->pr_hbuf[idx])     { free(s->pr_hbuf[idx]);     s->pr_hbuf[idx] = NULL; }
    if (s->pr_hbuf_out[idx]) { free(s->pr_hbuf_out[idx]); s->pr_hbuf_out[idx] = NULL; }
    if (s->pr_up_key && s->pr_up_key[idx]) { free(s->pr_up_key[idx]); s->pr_up_key[idx] = NULL; }
    if (s->pr_up_pend && s->pr_up_pend[idx]) { free(s->pr_up_pend[idx]); s->pr_up_pend[idx] = NULL; }
    if (s->pr_up_pend_len) s->pr_up_pend_len[idx] = 0;
    if (s->pr_up_req_len) s->pr_up_req_len[idx] = 0;
    s->pr_state[idx] = 0;
    s->pr_fd_client[idx]   = INVALID_SOCKET;
    s->pr_fd_upstream[idx] = 0;
    s->pr_route_idx[idx]   = -1;
    if (s->pr_hdr_len) s->pr_hdr_len[idx] = 0;
    if (s->pr_remain)  s->pr_remain[idx]  = 0;
    if (s->pr_up) nl_px_upstate_reset(&s->pr_up[idx]);
    if (s->pr_last_active_ms) s->pr_last_active_ms[idx] = 0;
    if (s->pr_req_drain) s->pr_req_drain[idx] = 0;   // 第204轮
    // 作废当前连接代号：旧连接的迟到完成事件（含空闲超时强关后的）将被丢弃
    if (s->pr_conn_id) s->pr_conn_id[idx] = ++s->pr_conn_id_next;
    s->pr_free[++(s->pr_free_top)] = idx;
    if (s->pr_count > 0) s->pr_count--;
}

/* 第204轮：keep-alive 复用 —— 清 per-请求状态、切回非阻塞并重新挂起客户端
 * 接收。调用前提：响应已发完、请求 body 已收齐、hbuf 无管道化残留。 */
static void pr_conn_keep_win(nl_web_server_t* s, int idx) {
    SOCKET cfd = s->pr_fd_client[idx];
    char*  hbuf = s->pr_hbuf[idx];
    s->pr_state[idx]       = PR_ST_ACTIVE;   // 清 DIR_W / KA / DRAIN / UP_EOF
    s->pr_route_idx[idx]   = -1;
    s->pr_remain[idx]      = 0;
    s->pr_up_pend_len[idx] = 0;
    s->pr_up_req_len[idx]  = 0;
    if (s->pr_up) nl_px_upstate_reset(&s->pr_up[idx]);
    s->pr_hdr_len[idx]     = 0;
    if (s->pr_req_drain) s->pr_req_drain[idx] = 0;
    if (s->pr_last_active_ms) s->pr_last_active_ms[idx] = pr_pool_now_ms();
    u_long nb_on = 1;
    ioctlsocket(cfd, FIONBIO, &nb_on);
    pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
    memset(cctx, 0, sizeof(*cctx));
    cctx->idx      = idx;
    cctx->conn_id  = s->pr_conn_id[idx];
    cctx->ev       = IOCP_EV_CLIENT_READ;
    cctx->sock     = cfd;
    cctx->buf      = hbuf;
    cctx->buflen   = 8192;
    WSABUF wb = {8192, hbuf};
    DWORD flags = 0, recvd = 0;
    WSARecv(cfd, &wb, 1, &recvd, &flags, (LPOVERLAPPED)cctx, NULL);
}

/* 第204轮：按剩余量挂起一次 body 消费接收（len 精确限制，段内超出 body 的
 * 字节留在内核缓冲，作为下一请求由正常接收路径读取）。 */
static void pr_post_drain_recv(nl_web_server_t* s, int idx) {
    SOCKET cfd = s->pr_fd_client[idx];
    char*  hbuf = s->pr_hbuf[idx];
    size_t want = 8192;
    if (s->pr_req_drain && s->pr_req_drain[idx] > 0 &&
        (size_t)s->pr_req_drain[idx] < want)
        want = (size_t)s->pr_req_drain[idx];
    u_long nb_on = 1;
    ioctlsocket(cfd, FIONBIO, &nb_on);
    pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
    memset(cctx, 0, sizeof(*cctx));
    cctx->idx      = idx;
    cctx->conn_id  = s->pr_conn_id[idx];
    cctx->ev       = IOCP_EV_CLIENT_READ;
    cctx->sock     = cfd;
    cctx->buf      = hbuf;
    cctx->buflen   = (int)want;
    WSABUF wb;
    wb.buf = hbuf;
    wb.len = (ULONG)want;
    DWORD flags = 0, recvd = 0;
    WSARecv(cfd, &wb, 1, &recvd, &flags, (LPOVERLAPPED)cctx, NULL);
}

// 响应 framing（header/chunked/101 隧道）与请求重组已下沉至 src/common/nl_proxy_core.c，
// 三平台共用同一份实现（含忽略大小写前缀比较 nl_px_ci_starts）；此处只保留
// “槽位数组 ↔ 解析上下文”的映射。
static void pr_parse_upstream_bytes(nl_web_server_t* s, int idx, const char* data, size_t len) {
    if (!s || !s->pr_up || !s->pr_hbuf_out || !s->pr_hbuf_out[idx]) return;
    (void)nl_px_upstate_feed(&s->pr_up[idx], s->pr_hbuf_out[idx], 8192, data, len);
}

// 第203轮：路径匹配三阶段（path 可能含 query，来自引擎请求行）：
//   ① 字面精确（剥 query、不解码不折叠）—— 保持"精确 > 前缀"；
//   ② DIR 字面前缀（最长优先，边界 = 下一字符 '/' 或结尾）—— 目录端点把
//      端点当根，"/web/../../x" 这类全局越根路径必须先按字面命中 /web
//      端点，再由公共层 clamp 解释，不能被 normalize 折叠成 /x 后错配；
//   ③ 规范化匹配（nl_px_path_match 内部 normalize，精确 > 最长前缀；
//      DIR 路由不参与 —— 其前缀匹配由②带边界负责，避免 /webx 被 /web 吞掉）。
// 命中优先级：精确 > DIR 前缀 > 其它前缀。
static int pr_lookup_route_idx(nl_web_server_t* s, const char* path) {
    char lit[4096];                               // 字面 path（剥 query/fragment；引擎 path 缓冲 4096）
    size_t ln = 0;
    while (path[ln] && path[ln] != '?' && path[ln] != '#') ln++;
    if (ln >= sizeof(lit)) ln = sizeof(lit) - 1;  // 超长截断 → 与 normalize ENOSPC 同为拒绝
    memcpy(lit, path, ln);
    lit[ln] = '\0';

    int idx = 0;
    int dir_idx = -1, best_idx = -1;
    size_t dir_len = 0, best_len = 0;
    EnterCriticalSection(&s->mutex);
    for (nl_web_route_t* r = s->routes; r; r = r->next, idx++) {
        if (strcmp(r->path, lit) == 0) {          // ① 字面精确
            LeaveCriticalSection(&s->mutex);
            return idx;
        }
        if (r->type == NL_ROUTE_TYPE_DIR) {       // ② DIR 字面前缀
            size_t plen = strlen(r->path);
            if (ln >= plen && memcmp(r->path, lit, plen) == 0 &&
                (lit[plen] == '\0' || lit[plen] == '/') &&
                (dir_idx < 0 || plen > dir_len)) {
                dir_len = plen;
                dir_idx = idx;
            }
        }
    }
    idx = 0;
    for (nl_web_route_t* r = s->routes; r; r = r->next, idx++) {
        if (r->type == NL_ROUTE_TYPE_DIR) continue;       // ③ 排除 DIR（边界由②负责）
        if (nl_px_path_match(r->path, path)) {
            LeaveCriticalSection(&s->mutex);
            return idx;
        }
        if (r->is_prefix && nl_px_path_match_prefix(r->path, path)) {
            size_t plen = strlen(r->path);
            if (best_idx < 0 || plen > best_len) {   // 最长前缀；同长取链表先出现者
                best_len = plen;
                best_idx = idx;
            }
        }
    }
    LeaveCriticalSection(&s->mutex);
    if (dir_idx >= 0) return dir_idx;             // DIR 端点优先于其它前缀
    return best_idx;
}

// 构造上游请求，输出到 out，返回字节数（-1=失败）
static int pr_build_upstream_http(nl_web_server_t* s, int idx,
                                  const char* method, const char* path,
                                  const char* proto,
                                  const char* raw_hdrs, size_t raw_hdrs_len,
                                  char* out, size_t out_cap) {
    nl_web_route_t* r = NULL;
    EnterCriticalSection(&s->mutex);
    int rd = s->pr_route_idx[idx];
    int cur = 0;
    for (nl_web_route_t* it = s->routes; it; it = it->next, cur++) {
        if (cur == rd) { r = it; break; }
    }
    if (rd < 0 || !r) { LeaveCriticalSection(&s->mutex); return -1; }
    int is_tcp = (r->protocol == NL_PROXY_TCP);
    char upstream[512];
    snprintf(upstream, sizeof(upstream), "%s", r->upstream);
    /* 第195轮：目录转发改写所需字段一并复制（解锁后不再解引用共享路由） */
    int is_prefix = r->is_prefix;
    char match_path[256], up_uri[256];
    snprintf(match_path, sizeof(match_path), "%s", r->path);
    snprintf(up_uri, sizeof(up_uri), "%s", r->up_uri);
    LeaveCriticalSection(&s->mutex);

    if (is_tcp) return nl_px_passthrough(out, out_cap, raw_hdrs, raw_hdrs_len);

    // 真实客户端 IP（旧实现硬编码 X-Forwarded-For: 127.0.0.1）；
    // IPv4/IPv6 统一走公共 helper（双栈监听下 IPv4 以映射地址出现）
    char peer_ip[64];
    peer_ip[0] = '\0';
    struct sockaddr_storage pa;
    int pa_len = (int)sizeof(pa);
    memset(&pa, 0, sizeof(pa));
    if (getpeername(s->pr_fd_client[idx], (struct sockaddr*)&pa, &pa_len) == 0)
        (void)nl_px_sockaddr_ip((const struct sockaddr*)&pa, peer_ip, sizeof(peer_ip));
    /* 第195轮：目录转发路径改写（nginx proxy_pass URI 拼接语义）；
     * up_uri 为空时 nl_px_dir_rewrite 原样拷贝，热路径开销仅为一次 strlen。 */
    char rpath[4096];
    const char* fwd_path = path;
    if (up_uri[0]) {
        if (nl_px_dir_rewrite(match_path, is_prefix, up_uri, path,
                              rpath, sizeof(rpath)) > 0)
            fwd_path = rpath;
    }
    // 请求行改写 / Host / XFF / 客户端头透传 / 空行终止符：公共层实现
    return nl_px_build_request(out, out_cap, method, fwd_path, proto, upstream,
                               raw_hdrs, raw_hdrs_len, peer_ip[0] ? peer_ip : NULL);
}

/* ============================================================
 * 进程内路由分发（H2 stream 直调，免去本机回放 socket 一跳）
 * 回调由公共层 nl_px_local_dispatch 调用：只读路由表 / 建上游连接。
 * ============================================================ */
/* 第204轮：请求级路由预查缓存（线程本地）——引擎 dispatch 已查过一次路由，
 * plan_build 内部回调命中此缓存可免再走 pr_lookup_route_idx + 链表遍历的
 * 两次 s->mutex（缺陷4：多消费线程下路由表锁竞争拖垮吞吐）。仅引擎分发
 * 路径写入、同请求内 plan_build 一次性消费；H2/legacy 等调用方为 NULL。 */
#ifdef _MSC_VER
#define NL_WIN_TLS __declspec(thread)   /* MSVC */
#else
#define NL_WIN_TLS __thread             /* MinGW/GCC（__declspec(thread) 会告警） */
#endif
static NL_WIN_TLS nl_web_route_t* g_pr_pre_route = NULL;
static NL_WIN_TLS char g_pr_pre_path[4096];

static int pr_local_lookup(void* ctx, const char* path, nl_px_local_route* out) {
    nl_web_server_t* s = (nl_web_server_t*)ctx;
    if (!s || !path || !out) return 0;
    if (g_pr_pre_route && strcmp(g_pr_pre_path, path) == 0) {
        /* 命中预查缓存：免两次 s->mutex；一次性消费杜绝陈旧指针 */
        nl_web_route_t* pre = g_pr_pre_route;
        g_pr_pre_route = NULL;
        memset(out, 0, sizeof(*out));
        out->redirect_code = (s->redirect_type == NL_REDIRECT_PERMANENT) ? 301 : 302;
        switch (pre->type) {
        case NL_ROUTE_TYPE_CONTENT:
            out->type = NL_PX_LOCAL_CONTENT;
            out->content = pre->content;
            out->content_len = pre->content_size;
            out->content_type = pre->content_type;
            return 1;
        case NL_ROUTE_TYPE_FILE:
            out->type = NL_PX_LOCAL_FILE;
            out->file_path = pre->file_path;
            out->content_type = pre->content_type;
            return 1;
        case NL_ROUTE_TYPE_REDIRECT:
            out->type = NL_PX_LOCAL_REDIRECT;
            out->redirect_url = pre->redirect_url;
            return 1;
        case NL_ROUTE_TYPE_PROXY:
            out->type = NL_PX_LOCAL_PROXY;
            out->protocol = (pre->protocol == NL_PROXY_HTTPS) ? NL_PX_LOCAL_PROTO_HTTPS
                          : (pre->protocol == NL_PROXY_TCP) ? NL_PX_LOCAL_PROTO_TCP
                          : NL_PX_LOCAL_PROTO_HTTP;
            out->upstream = pre->upstream;
            out->dir_prefix = pre->is_prefix ? pre->path : NULL;
            out->up_uri = pre->up_uri;
            return 1;
        case NL_ROUTE_TYPE_DIR:
            out->type = NL_PX_LOCAL_DIR;
            out->file_path = pre->file_path;
            out->dir_prefix = pre->path;
            return 1;
        default:
            return 0;
        }
    }
    int ridx = pr_lookup_route_idx(s, path);
    if (ridx < 0) return 0;
    nl_web_route_t* r = NULL;
    EnterCriticalSection(&s->mutex);
    int cur = 0;
    for (nl_web_route_t* it = s->routes; it; it = it->next, cur++)
        if (cur == ridx) { r = it; break; }
    memset(out, 0, sizeof(*out));
    out->redirect_code = (s->redirect_type == NL_REDIRECT_PERMANENT) ? 301 : 302;
    int ok = 1;
    switch (r ? r->type : NL_ROUTE_TYPE_CONTENT) {
    case NL_ROUTE_TYPE_CONTENT:
        out->type = NL_PX_LOCAL_CONTENT;
        out->content = r->content;
        out->content_len = r->content_size;
        out->content_type = r->content_type;
        break;
    case NL_ROUTE_TYPE_FILE:
        out->type = NL_PX_LOCAL_FILE;
        out->file_path = r->file_path;
        out->content_type = r->content_type;
        break;
    case NL_ROUTE_TYPE_REDIRECT:
        out->type = NL_PX_LOCAL_REDIRECT;
        out->redirect_url = r->redirect_url;
        break;
    case NL_ROUTE_TYPE_PROXY:
        out->type = NL_PX_LOCAL_PROXY;
        out->protocol = (r->protocol == NL_PROXY_HTTPS) ? NL_PX_LOCAL_PROTO_HTTPS
                      : (r->protocol == NL_PROXY_TCP) ? NL_PX_LOCAL_PROTO_TCP
                      : NL_PX_LOCAL_PROTO_HTTP;
        out->upstream = r->upstream;
        out->dir_prefix = r->is_prefix ? r->path : NULL;   // 第195轮：目录转发
        out->up_uri = r->up_uri;                           // 空=路径原样透传
        break;
    case NL_ROUTE_TYPE_DIR:                       // 第203轮：目录/文件端点
        out->type = NL_PX_LOCAL_DIR;
        out->file_path = r->file_path;            // 端点根（目录或文件）
        out->dir_prefix = r->path;                // URL 端点前缀
        break;
    default:
        ok = 0;
        break;
    }
    if (!r) ok = 0;
    LeaveCriticalSection(&s->mutex);
    return ok;
}

static intptr_t pr_local_connect_upstream(void* ctx, const nl_px_local_route* route) {
    (void)ctx;
    if (!route || !route->upstream) return -1;
    char host[256];
    int port = 80;
    if (!nl_px_split_hostport(route->upstream, host, sizeof(host), &port, 80)) return -1;
    struct sockaddr_storage ss;
    int af = nl_proxy_fill_addr(host, port, &ss);
    if (af == 0) return -1;
    SOCKET fd = socket(af, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return -1;
    DWORD tv = 5000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    int slen = (af == AF_INET6) ? (int)sizeof(struct sockaddr_in6)
                                : (int)sizeof(struct sockaddr_in);
    if (connect(fd, (struct sockaddr*)&ss, slen) != 0) {
        closesocket(fd);
        return -1;
    }
    return (intptr_t)fd;
}

static const nl_px_local_ops g_pr_local_ops = {
    pr_local_lookup, pr_local_connect_upstream
};

// select 数据驱动主循环：单线程 select 驱动 N 条 proxy 连接
// 空闲 200ms 退避（select 超时），非阻塞 socket（WSAEMSGSIZE/EWOULDBLOCK）。
// P6: IOCP 完成消费循环 / 完成处理段 前置声明（引擎函数先引用后定义）
static void pr_iocp_consume_loop(nl_web_server_t* s, SOCKET listen_sock);
static DWORD WINAPI nl_web_iocp_consumer_thread(LPVOID arg);
static void pr_post_accept_slot(nl_web_server_t* s, SOCKET listen_sock, int k);
/* AcceptEx 单个地址区长度：双栈监听下对端可能是 IPv6，必须按 sockaddr_storage 预留 */
#define PR_ACCEPT_ADDR_LEN ((int)(sizeof(SOCKADDR_STORAGE) + 16))
void nl_web_proxy_engine(nl_web_server_t* s, SOCKET listen_sock) {
    int cap = nl_web_proxy_ring_capacity();
    s->pr_cap = cap;
    // P6: 完成处理临界区（多消费线程串行化 slot/pool 状态）；所有出口统一在 cleanup_fail 释放
    InitializeCriticalSection(&s->pr_loop_cs);
    // 第191轮：AcceptEx 槽位独立锁（post/claim 不占 pr_loop_cs）
    InitializeCriticalSection(&s->pr_accept_cs);
    // 第191轮：accept 批量槽与背压字段在任何 goto cleanup_fail 之前复位
    //（cleanup_fail 会遍历关闭，未初始化的 SOCKET=0 不是 INVALID，会误关句柄 0）
    for (int k = 0; k < PR_ACCEPT_PENDING; k++) s->pr_accept_socks[k] = INVALID_SOCKET;
    s->pr_accept_ctxs = NULL;
    s->pr_accept_addrs = NULL;
    s->pr_pending_client = INVALID_SOCKET;
    s->pr_accept_paused = 0;
    s->pr_fd_client   = (SOCKET*)calloc((size_t)cap, sizeof(SOCKET));
    s->pr_fd_upstream = (SOCKET*)calloc((size_t)cap, sizeof(SOCKET));
    s->pr_route_idx   = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_state       = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_hbuf        = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_hbuf_out    = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_hdr_len     = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_remain      = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_free        = (int*)malloc((size_t)cap * sizeof(int));
    s->pr_up          = (nl_px_upstate*)calloc((size_t)cap, sizeof(nl_px_upstate));
    s->pr_up_key      = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_up_pend     = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_up_pend_len = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_up_req_len  = (int*)calloc((size_t)cap, sizeof(int));
    // P7: 空闲超时扫描 + slot 复用竞态防护（conn_id 代号）
    s->pr_last_active_ms = (int64_t*)calloc((size_t)cap, sizeof(int64_t));
    s->pr_conn_id        = (DWORD*)calloc((size_t)cap, sizeof(DWORD));
    s->pr_req_drain      = (int64_t*)calloc((size_t)cap, sizeof(int64_t));   // 第204轮
    for (int i = 0; i < cap; i++) {
        s->pr_fd_client[i]   = INVALID_SOCKET;
        s->pr_fd_upstream[i] = 0;
        s->pr_route_idx[i]   = -1;
        nl_px_upstate_reset(&s->pr_up[i]);
        s->pr_free[i] = cap - 1 - i;
    }
    s->pr_free_top = cap - 1;
    s->pr_count = 0;
    if (!s->pr_fd_client || !s->pr_fd_upstream || !s->pr_route_idx ||
        !s->pr_state || !s->pr_hbuf || !s->pr_hbuf_out ||
        !s->pr_hdr_len || !s->pr_remain || !s->pr_free ||
        !s->pr_up || !s->pr_up_key ||
        !s->pr_up_pend || !s->pr_up_pend_len || !s->pr_up_req_len ||
        !s->pr_last_active_ms || !s->pr_conn_id || !s->pr_req_drain) {
        goto cleanup_fail;
    }
    // 初始化上游 keep-alive 连接池（公共层实现；全局空闲 256，单 key 8，空闲 30s 回收）
    s->up_pool = (nl_px_pool*)malloc(sizeof(nl_px_pool));
    if (!s->up_pool) goto cleanup_fail;
    nl_px_pool_init(s->up_pool, &g_pr_pool_ops, 256, 8, 30000);

    // ---- IOCP 完成端口初始化 ----
    s->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!s->iocp) {
        nl_log(NL_LOG_ERROR, "IOCP: CreateIoCompletionPort 失败 gle=%lu", GetLastError());
        goto cleanup_fail;
    }
    // listen socket 关联到 IOCP，completion key 用哨兵
    CreateIoCompletionPort((HANDLE)listen_sock, s->iocp, (ULONG_PTR)IOCP_LISTEN_KEY, 0);

    // 预创建 per-slot OVERLAPPED 上下文数组（客户端侧 + 上游侧各 cap 个）
    s->pr_iocp_client_ctx = calloc((size_t)cap, sizeof(pr_iocp_ctx_t));
    s->pr_iocp_up_ctx     = calloc((size_t)cap, sizeof(pr_iocp_ctx_t));
    s->pr_iocp_cw_ctx     = calloc((size_t)cap, sizeof(pr_iocp_ctx_t));
    if (!s->pr_iocp_client_ctx || !s->pr_iocp_up_ctx || !s->pr_iocp_cw_ctx) {
        nl_log(NL_LOG_ERROR, "IOCP: OVERLAPPED ctx calloc 失败");
        goto cleanup_fail;
    }

    // 第191轮：多槽 AcceptEx 上下文 —— 每槽独立 OVERLAPPED + 地址输出缓冲
    //（AcceptEx 完成前 OVERLAPPED/缓冲必须唯一且不可重用，不能共享单缓冲）
    s->pr_accept_ctxs  = calloc(PR_ACCEPT_PENDING, sizeof(pr_iocp_ctx_t));
    s->pr_accept_addrs = (char*)calloc(PR_ACCEPT_PENDING, (size_t)PR_ACCEPT_ADDR_LEN * 2);
    if (!s->pr_accept_ctxs || !s->pr_accept_addrs) {
        nl_log(NL_LOG_ERROR, "IOCP: AcceptEx 多槽上下文 calloc 失败");
        goto cleanup_fail;
    }
    for (int k = 0; k < PR_ACCEPT_PENDING; k++) {
        pr_iocp_ctx_t* a = &((pr_iocp_ctx_t*)s->pr_accept_ctxs)[k];
        memset(a, 0, sizeof(*a));
        a->ev  = IOCP_EV_ACCEPT;
        a->idx = k;
    }
    // AcceptEx 的输出地址缓冲：dwReceiveDataLength=0 时仍必须提供合法的
    // local+remote 地址区（传 NULL 会返回 WSAEFAULT，导致 accept 永不完成）。
    // 每服务器独立、每 pending 槽独立一份。
    // 批量 post 全部 pending（失败槽由消费循环超时分支自愈补投）
    // 第191轮：pr_post_accept_slot 内部自持 pr_accept_cs，启动阶段单线程亦可直接调用
    for (int k = 0; k < PR_ACCEPT_PENDING; k++)
        pr_post_accept_slot(s, listen_sock, k);

    // P6: engine_threads>1 时创建额外 IOCP 消费线程跑同一完成循环。
    // 注：Windows engine_threads=IOCP 消费线程数；当前完成处理经 pr_loop_cs 临界区
    // 串行化，多线程主要提升 GQCS 取包并发（语义正确优先，不做无锁化改造）。
    {
        int extra = s->engine_threads_cfg - 1;
        if (extra > 0) {
            s->pr_engine_threads = (HANDLE*)calloc((size_t)extra, sizeof(HANDLE));
            if (s->pr_engine_threads) {
                for (int t = 0; t < extra; t++) {
                    HANDLE th = CreateThread(NULL, 0, nl_web_iocp_consumer_thread, s, 0, NULL);
                    if (!th) {
                        nl_log(NL_LOG_ERROR, "IOCP: 创建消费线程 %d 失败 gle=%lu", t + 1, GetLastError());
                        break;
                    }
                    s->pr_engine_threads[s->pr_engine_thread_count++] = th;
                }
            }
        }
    }

    // ---- IOCP 主循环（主线程即消费线程 0）----
    pr_iocp_consume_loop(s, listen_sock);

    // P6: join 额外消费线程（running=0 后最多一个 GQCS 超时 200ms 即退出）；
    // join 完成前不得释放任何 ring 数组。
    for (int t = 0; t < s->pr_engine_thread_count; t++) {
        if (s->pr_engine_threads[t]) {
            WaitForSingleObject(s->pr_engine_threads[t], 5000);
            CloseHandle(s->pr_engine_threads[t]);
            s->pr_engine_threads[t] = NULL;
        }
    }
    free(s->pr_engine_threads); s->pr_engine_threads = NULL;
    s->pr_engine_thread_count = 0;

    // 唤醒 TLS 隧道线程：只 shutdown 不 close（句柄由隧道线程关闭，避免二次关闭）
    for (int i = 0; i < cap; i++) {
        if (!(s->pr_state[i] & PR_ST_ACTIVE) || !(s->pr_state[i] & PR_ST_TLS)) continue;
        if (s->pr_fd_client[i] != INVALID_SOCKET) shutdown(s->pr_fd_client[i], SD_BOTH);
        if (s->pr_fd_upstream[i] > 0) shutdown(s->pr_fd_upstream[i], SD_BOTH);
    }
    // 隧道线程会在引擎锁内回收槽位；等待归零后才能释放槽位数组
    for (int w = 0; w < 1000 && InterlockedCompareExchange(&s->pr_tls_threads, 0, 0) > 0; w++)
        Sleep(10);

    // 清理所有活动连接（TLS 槽位已由隧道线程回收，这里仅处理普通连接）
    for (int i = 0; i < cap; i++) {
        if (s->pr_state[i] & PR_ST_ACTIVE) {
            if (!(s->pr_state[i] & PR_ST_TLS)) {
                if (s->pr_fd_client[i] != INVALID_SOCKET) closesocket(s->pr_fd_client[i]);
                if (s->pr_fd_upstream[i] > 0) closesocket(s->pr_fd_upstream[i]);
            }
            s->pr_state[i] = 0;
        }
        if (s->pr_hbuf[i])     free(s->pr_hbuf[i]);
        if (s->pr_hbuf_out[i]) free(s->pr_hbuf_out[i]);
        if (s->pr_up_key && s->pr_up_key[i]) free(s->pr_up_key[i]);
        if (s->pr_up_pend && s->pr_up_pend[i]) free(s->pr_up_pend[i]);
    }

cleanup_fail:
    // 销毁上游 keep-alive 连接池（关闭所有空闲连接）
    if (s->up_pool) { nl_px_pool_destroy(s->up_pool); free(s->up_pool); s->up_pool = NULL; }
    // IOCP 清理：CloseHandle 会自动取消所有 pending I/O
    if (s->iocp) { CloseHandle(s->iocp); s->iocp = NULL; }
    // 第191轮：关闭全部批量 AcceptEx 预创建 socket（CloseHandle(iocp) 只取消 I/O，句柄仍需关）
    for (int k = 0; k < PR_ACCEPT_PENDING; k++) {
        if (s->pr_accept_socks[k] != INVALID_SOCKET) {
            closesocket(s->pr_accept_socks[k]);
            s->pr_accept_socks[k] = INVALID_SOCKET;
        }
    }
    free(s->pr_accept_ctxs);  s->pr_accept_ctxs  = NULL;
    free(s->pr_accept_addrs); s->pr_accept_addrs = NULL;
    // 第190轮：关闭 ring 满背压暂存的已 accept 连接
    if (s->pr_pending_client != INVALID_SOCKET) {
        closesocket(s->pr_pending_client);
        s->pr_pending_client = INVALID_SOCKET;
    }
    s->pr_accept_paused = 0;
    free(s->pr_accept_addr); s->pr_accept_addr = NULL;
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
    free(s->pr_up);          s->pr_up          = NULL;
    free(s->pr_up_key);      s->pr_up_key      = NULL;
    free(s->pr_up_pend);     s->pr_up_pend     = NULL;
    free(s->pr_up_pend_len); s->pr_up_pend_len = NULL;
    free(s->pr_up_req_len);  s->pr_up_req_len  = NULL;
    free(s->pr_last_active_ms); s->pr_last_active_ms = NULL;  // P7
    free(s->pr_conn_id);        s->pr_conn_id        = NULL;  // slot 复用竞态防护
    free(s->pr_req_drain);      s->pr_req_drain      = NULL;  // 第204轮
    s->pr_cap = s->pr_count = 0;
    // P6: 释放完成处理临界区（与引擎入口 InitializeCriticalSection 配对；
    // 重复 start/stop 每次引擎启动重新初始化，不泄漏）
    DeleteCriticalSection(&s->pr_loop_cs);
    DeleteCriticalSection(&s->pr_accept_cs);   // 第191轮
}

// ---- IOCP 完成事件处理段（调用方持有 pr_loop_cs，串行化执行）----
// 入口校验 ctx->conn_id：slot 复用后旧连接的迟到完成事件直接丢弃（P7 空闲强关会放大该竞态）。

// 第191轮：向指定 pending 槽 post 一个 AcceptEx。内部只在 claim 槽位时持
// pr_accept_cs（几 us），WSASocket/AcceptEx 内核调用（~300us）完全无锁执行 ——
// 不再占用 pr_loop_cs，accept 与读处理并行。槽内已有 outstanding、背压暂停或
// 引擎停止时直接返回（幂等，可全量补投）；post 失败置空槽，由消费循环超时
// 分支周期自愈补投 —— 否则 accept 永久停摆。
static void pr_post_accept_slot(nl_web_server_t* s, SOCKET listen_sock, int k) {
    if (!s->running || !s->pr_accept_ctxs || !s->pr_accept_addrs) return;
    if (k < 0 || k >= PR_ACCEPT_PENDING) return;
    SOCKET cs = WSASocket(s->af ? s->af : AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (cs == INVALID_SOCKET) return;
    // claim 槽位：与完成分支的 clear、其它线程的补投互斥（背压检查也在此读，
    // paused 的写点均持 pr_accept_cs，读写同步）
    EnterCriticalSection(&s->pr_accept_cs);
    if (s->pr_accept_paused || s->pr_accept_socks[k] != INVALID_SOCKET) {
        LeaveCriticalSection(&s->pr_accept_cs);
        closesocket(cs);
        return;
    }
    s->pr_accept_socks[k] = cs;
    LeaveCriticalSection(&s->pr_accept_cs);
    pr_iocp_ctx_t* a = &((pr_iocp_ctx_t*)s->pr_accept_ctxs)[k];
    char* addr = s->pr_accept_addrs + (size_t)k * PR_ACCEPT_ADDR_LEN * 2;
    DWORD bytes_recv = 0;
    if (!AcceptEx(listen_sock, cs, addr, 0,
                  PR_ACCEPT_ADDR_LEN, PR_ACCEPT_ADDR_LEN,
                  &bytes_recv, (LPOVERLAPPED)a) &&
        WSAGetLastError() != ERROR_IO_PENDING) {
        // AcceptEx 同步失败：收回槽位（claim 后完成事件不可能已来，保守条件仍写）
        EnterCriticalSection(&s->pr_accept_cs);
        int mine = (s->pr_accept_socks[k] == cs);
        if (mine) s->pr_accept_socks[k] = INVALID_SOCKET;
        LeaveCriticalSection(&s->pr_accept_cs);
        closesocket(cs);
        if (!mine) return;   // 槽已被接管：不得二次关闭
    }
}

// 全量补投：遍历所有 pending 槽，缺哪个补哪个（已有 outstanding 的跳过）。
// 供完成事件与超时自愈分支调用；背压暂停时内部直接返回。第191轮起不再要求
// 持 pr_loop_cs（内部自持 pr_accept_cs），可从锁外调用。
static void pr_repost_accept_slots(nl_web_server_t* s, SOCKET listen_sock) {
    if (!s->running) return;
    for (int k = 0; k < PR_ACCEPT_PENDING; k++)
        pr_post_accept_slot(s, listen_sock, k);
}

// 第191轮：把已 accept 的 socket 装入指定空闲槽位并初始化读上下文。
// 调用方必须持有 pr_loop_cs，且已通过 pr_alloc_slot 拿到 idx。
// 首包 WSARecv（~150us 内核调用）不在本函数执行 —— 返回 cctx，由调用方塞进
// defer，在离开 pr_loop_cs 后 post。返回 NULL = 初始化失败（已清理）。
// 顺序要求：last_active 必须先于 PR_ST_ACTIVE 写入 —— 锁外 post 期间其它
// 消费线程的空闲扫描（持 pr_loop_cs）可能看到 ACTIVE 槽，last=0 会被误强关。
static pr_iocp_ctx_t* pr_adopt_client_locked(nl_web_server_t* s, int idx, SOCKET new_client) {
    // 新 client 关联到 IOCP（completion key = idx）
    CreateIoCompletionPort((HANDLE)new_client, s->iocp, (ULONG_PTR)idx, 0);
    s->pr_fd_client[idx] = new_client;
    if (s->pr_last_active_ms) s->pr_last_active_ms[idx] = pr_pool_now_ms();  // P7: accept 即计入活跃
    s->pr_hbuf[idx]     = (char*)malloc(8193);  // +1：留 hbuf[*hlen]='\0' 终止符空间
    // 第190轮：hbuf_out(8KB 请求草稿)/up_key(576B 池 key) 只有 proxy 路由用，
    // 延迟到分发时分配 —— 本地路由每连接省 8768 字节 + 2 次 malloc/free
    s->pr_hdr_len[idx]  = 0;
    s->pr_remain[idx]   = 0;
    nl_px_upstate_reset(&s->pr_up[idx]);
    if (!s->pr_hbuf[idx]) {
        closesocket(new_client);
        pr_free_slot(s, idx);
        return NULL;
    }
    // 初始化首包读上下文（WSARecv 由调用方锁外 post）
    pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
    memset(cctx, 0, sizeof(*cctx));
    cctx->idx = idx;
    cctx->conn_id = s->pr_conn_id[idx];
    cctx->ev  = IOCP_EV_CLIENT_READ;
    cctx->sock = new_client;
    cctx->buf = s->pr_hbuf[idx];
    cctx->buflen = 8192;
    s->pr_state[idx] = PR_ST_ACTIVE;   // 最后置 ACTIVE（此前空闲扫描视本槽为空）
    return cctx;
}

// 第191轮：把 closesocket 推迟到离开 pr_loop_cs 后执行（consume loop 统一
// 循环关闭）。数组溢出时兜底立即关闭，绝不丢 fd。0/INVALID 视为"无连接"跳过。
static inline void pr_close_defer(pr_defer_t* arr, int* n, SOCKET fd) {
    if (fd == INVALID_SOCKET || fd == 0) return;
    if (*n < PR_DEFER_CLOSE_MAX) {
        pr_defer_t* d = &arr[(*n)++];
        d->kind = PR_DEFER_CLOSE;
        d->fd = fd;
        d->has_plan = 0;
    } else {
        closesocket(fd);
    }
}

/* 第205轮 CONNECT：直连隧道目标（nl_proxy_fill_addr 解析 → socket → connect），
 * 5 秒收发超时防挂死。不复用 nl_proxy_connect_upstream（它按 route 取地址，
 * CONNECT 语义要求直接用解析出的 host:port 建连）。成功返回 socket，
 * 失败返回 INVALID_SOCKET。 */
static SOCKET pr_connect_direct(const char* host, int port) {
    struct sockaddr_storage ss;
    int af = nl_proxy_fill_addr(host, port, &ss);
    if (af == 0) return INVALID_SOCKET;
    SOCKET fd = socket(af, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return INVALID_SOCKET;
    DWORD tv_ms = 5000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
    int len = (af == AF_INET6) ? (int)sizeof(struct sockaddr_in6)
                               : (int)sizeof(struct sockaddr_in);
    if (connect(fd, (struct sockaddr*)&ss, len) == SOCKET_ERROR) {
        closesocket(fd);
        return INVALID_SOCKET;
    }
    return fd;
}

static void pr_iocp_handle_completion(nl_web_server_t* s, SOCKET listen_sock,
                                      ULONG_PTR compkey, DWORD bytes_trans, pr_iocp_ctx_t* ctx,
                                      int completion_ok, pr_defer_t* defer, int* ndefer,
                                      int* out_post_k) {
    (void)listen_sock;   // 第191轮：accept 补投已移出本函数（consume loop 锁外执行）
    int cap = s->pr_cap;
        // ---- AcceptEx 完成 ----
        if (compkey == (ULONG_PTR)IOCP_LISTEN_KEY) {
            // 第191轮：多槽批量 AcceptEx —— 完成 OVERLAPPED 即该槽 ctx，
            // AcceptEx 刚把新 client 填入对应 pr_accept_socks[k]
            int k = (ctx && ctx->ev == IOCP_EV_ACCEPT) ? ctx->idx : -1;
            SOCKET new_client = INVALID_SOCKET;
            if (k >= 0 && k < PR_ACCEPT_PENDING) {
                // 槽 clear 与其它线程的 post claim 互斥（accept 独立锁，几 us）
                EnterCriticalSection(&s->pr_accept_cs);
                new_client = s->pr_accept_socks[k];
                s->pr_accept_socks[k] = INVALID_SOCKET;   // 槽空出，可重新投递
                LeaveCriticalSection(&s->pr_accept_cs);
            }
            if (!completion_ok) {
                // 监听 AcceptEx 失败：立即关闭坏 socket，下方全量补投
                if (new_client != INVALID_SOCKET) closesocket(new_client);
            } else if (new_client != INVALID_SOCKET) {
                int idx = pr_alloc_slot(s);
                if (idx < 0) {
                    /* 第190轮：ring 满背压 —— 不再重投 AcceptEx（连接留在内核
                     * backlog），已 accept 的这条暂存 pending，超时分支拿到空槽
                     * 后再接管；旧实现直接 closesocket 造成客户端无响应 RST。 */
                    if (s->pr_pending_client == INVALID_SOCKET)
                        s->pr_pending_client = new_client;
                    else
                        closesocket(new_client);
                    EnterCriticalSection(&s->pr_accept_cs);   // paused 写同步于 post 的读
                    s->pr_accept_paused = 1;
                    LeaveCriticalSection(&s->pr_accept_cs);
                    return;
                }
                pr_iocp_ctx_t* cctx = pr_adopt_client_locked(s, idx, new_client);
                if (cctx && *ndefer < PR_DEFER_CLOSE_MAX) {
                    // 首包 WSARecv 推迟到锁外 post（~150us 内核调用不占 pr_loop_cs）
                    pr_defer_t* d = &defer[(*ndefer)++];
                    d->kind = PR_DEFER_RECV;
                    d->fd = new_client;
                    d->idx = idx;
                    d->has_plan = 0;
                } else if (cctx) {
                    // defer 满（极端突发）：锁内兜底 post，绝不丢连接
                    WSABUF wb = {8192, s->pr_hbuf[idx]};
                    DWORD fl = 0, dm = 0;
                    WSARecv(new_client, &wb, 1, &dm, &fl, (LPOVERLAPPED)cctx, NULL);
                }
            }
            // 第191轮：补投本槽推迟到离开 pr_loop_cs 后（WSASocket/AcceptEx 不占锁；
            // 背压暂停时 post 内部 claim 时跳过，失败槽由超时分支自愈）
            if (out_post_k) *out_post_k = k;
            return;
        }

        // ---- client / upstream socket 完成（compkey == idx）----
        int idx = (int)compkey;
        if (idx < 0 || idx >= cap) return;
        // slot 复用竞态防护：旧连接迟到完成（正常关闭/空闲强关后的残余事件）丢弃
        if (ctx->conn_id != s->pr_conn_id[idx]) return;
        if (!(s->pr_state[idx] & PR_ST_ACTIVE)) return;

        SOCKET cfd = s->pr_fd_client[idx];
        if (cfd == INVALID_SOCKET) return;

        if (ctx->ev == IOCP_EV_UPSTREAM_CONNECT) {
            // ---- ConnectEx 异步上游连接完成（不阻塞引擎消费线程）----
            SOCKET ufd = s->pr_fd_upstream[idx];
            if (!completion_ok || ufd == 0 || ufd == INVALID_SOCKET) {
                // 连接失败 / 无 socket：回 502 并回收槽位
                s->pr_up_req_len[idx] = 0;
                if (ufd > 0 && ufd != INVALID_SOCKET) closesocket(ufd);
                s->pr_fd_upstream[idx] = 0;
                send_http_error(cfd, 502, "Bad Gateway");
                closesocket(cfd);
                s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                return;
            }

            // 取回路由（HTTPS 需要 host 做 TLS 握手）
            nl_web_route_t* r = NULL;
            int ridx = s->pr_route_idx[idx];
            EnterCriticalSection(&s->mutex);
            int cur = 0;
            for (nl_web_route_t* it = s->routes; it; it = it->next, cur++)
                if (cur == ridx) { r = it; break; }
            LeaveCriticalSection(&s->mutex);

            // HTTPS 上游：整条连接交给 TLS 隧道线程（握手与双向收发都在线程内，
            // 引擎不再对该 fd 投递 WSARecv/WSASend）；失败由辅助函数回 502 并回收。
            int req_len = s->pr_up_req_len[idx];
            s->pr_up_req_len[idx] = 0;
            if (r && r->protocol == NL_PROXY_HTTPS) {
                char up_host[512];
                int up_port = 0;
                nl_proxy_resolve_upstream(r, up_host, sizeof(up_host), &up_port);
                (void)pr_tls_start_tunnel(s, idx, cfd, ufd, up_host,
                                          s->pr_hbuf_out[idx], req_len);
                return;
            }

            int stage_ok = 1;
            // 发送异步 connect 前已构造好的上游请求（此时 socket 仍为阻塞态）
            if (req_len > 0 && s->pr_hbuf_out[idx]) {
                int off = 0;
                while (off < req_len) {
                    int w = send(ufd, s->pr_hbuf_out[idx] + off, req_len - off, 0);
                    if (w <= 0) { stage_ok = 0; break; }
                    off += w;
                }
            }
            // 懒分配上游响应缓冲（后续响应走 OVERLAPPED WSARecv 写入该缓冲）
            if (stage_ok && !s->pr_up_pend[idx]) {
                s->pr_up_pend[idx] = (char*)malloc(PR_PROXY_BUF_SZ);
                if (!s->pr_up_pend[idx]) stage_ok = 0;
            }
            if (!stage_ok) {
                if (s->pr_up_pend[idx]) { free(s->pr_up_pend[idx]); s->pr_up_pend[idx] = NULL; }
                closesocket(ufd); s->pr_fd_upstream[idx] = 0;
                send_http_error(cfd, 502, "Bad Gateway");
                closesocket(cfd); s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                return;
            }

            // 连接建立完成：切非阻塞，后续全部走 OVERLAPPED 读写
            u_long nb_on = 1;
            ioctlsocket(ufd, FIONBIO, &nb_on);

            // post 上游 WSARecv（响应回来 → 转发客户端）
            pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
            memset(uctx, 0, sizeof(*uctx));
            uctx->idx = idx;
            uctx->conn_id = s->pr_conn_id[idx];
            uctx->ev  = IOCP_EV_UPSTREAM_READ;
            uctx->sock = ufd;
            uctx->buf  = s->pr_up_pend[idx];
            uctx->buflen = PR_PROXY_BUF_SZ;
            WSABUF wsb_up = {PR_PROXY_BUF_SZ, s->pr_up_pend[idx]};
            DWORD flg_up = 0, dummy_up = 0;
            WSARecv(ufd, &wsb_up, 1, &dummy_up, &flg_up, (LPOVERLAPPED)uctx, NULL);

            // post 客户端 WSARecv（可能还有 body 未读）
            pr_iocp_ctx_t* cctx2 = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
            memset(cctx2, 0, sizeof(*cctx2));
            cctx2->idx = idx;
            cctx2->conn_id = s->pr_conn_id[idx];
            cctx2->ev  = IOCP_EV_CLIENT_READ;
            cctx2->sock = cfd;
            cctx2->buf  = s->pr_hbuf[idx];
            cctx2->buflen = 8192;
            WSABUF wsabuf_c = {8192, s->pr_hbuf[idx]};
            DWORD flags_c = 0, dummy_c = 0;
            WSARecv(cfd, &wsabuf_c, 1, &dummy_c, &flags_c, (LPOVERLAPPED)cctx2, NULL);

            /* 第204轮：保留 PR_ST_KA（完成点复用判定依据），清 UP_EOF/DRAIN 等旧状态 */
s->pr_state[idx] = (s->pr_state[idx] & PR_ST_KA) | PR_ST_DIR_W | PR_ST_ACTIVE;
            return;
        }

        if (ctx->ev == IOCP_EV_CLIENT_READ) {
            // ---- WSARecv 客户端读完成 ----
            int nread = (int)bytes_trans;
            if (nread <= 0) {
                // 客户端 EOF / 错误
                if (s->pr_fd_upstream[idx] > 0) { closesocket(s->pr_fd_upstream[idx]); s->pr_fd_upstream[idx] = 0; }
                closesocket(cfd); s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                return;
            }

            if (nread > 0 && s->pr_last_active_ms) s->pr_last_active_ms[idx] = pr_pool_now_ms();  // P7: 客户端数据到达，刷新活跃时间

            /* 第204轮：请求 body 消费阶段 —— 响应已发完，按 Content-Length 丢弃
             * 余量（本地路由 body 无人读）。收完后按 keep-alive 决定复用或关闭；
             * 字节不计入 hbuf/hlen，缓冲留给复用后的下一请求。 */
            if (s->pr_state[idx] & PR_ST_DRAIN) {
                if (s->pr_req_drain) {
                    s->pr_req_drain[idx] -= nread;
                    if (s->pr_req_drain[idx] < 0) s->pr_req_drain[idx] = 0;
                }
                if (s->pr_req_drain && s->pr_req_drain[idx] > 0) {
                    pr_post_drain_recv(s, idx);   // 未收齐：按余量继续收
                    return;
                }
                s->pr_state[idx] &= (uint8_t)~PR_ST_DRAIN;
                if (s->pr_state[idx] & PR_ST_KA) {
                    pr_conn_keep_win(s, idx);     // body 收齐 → 复用连接
                    return;
                }
                closesocket(cfd);
                s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);             // ka=0 → body 收完再关（防 RST）
                return;
            }

            char* hbuf = s->pr_hbuf[idx];
            int* hlen = &s->pr_hdr_len[idx];
            *hlen += nread;
            hbuf[*hlen] = '\0';
            /* HTTP/2 明文（h2c prior-knowledge）：首包即连接预流。必须在请求行
             * 解析之前判定，否则 "PRI * HTTP/2.0" 会被当作合法 HTTP/1 请求行。 */
            if (s->pr_fd_upstream[idx] == 0) {
                int h2 = nl_px_h2_preface_check(hbuf, (size_t)*hlen);
                if (h2 == 2) {
                    (void)pr_h2_start_bridge(s, idx, cfd, hbuf, *hlen, 0);
                    return;
                }
                if (h2 == 1) {
                    /* 预流未收满 24 字节，继续收，暂不按 HTTP/1 解析 */
                    pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                    WSABUF wsabuf = {8192 - (ULONG)*hlen, hbuf + *hlen};
                    DWORD flags = 0, dummy = 0;
                    WSARecv(cfd, &wsabuf, 1, &dummy, &flags, (LPOVERLAPPED)cctx, NULL);
                    return;
                }
            }


            if (s->pr_fd_upstream[idx] == 0) {
                // 首次收到首行：解析 method/path/proto，路由，连上游
                char* line_end = memchr(hbuf, '\n', (size_t)*hlen);
                if (!line_end) {
                    // 缓冲满仍未见首行 → 400；否则等更多数据再 post WSARecv
                    if (*hlen >= 8192) {
                        send_http_error(cfd, 400, "Bad Request");
                        closesocket(cfd);
                        pr_free_slot(s, idx);
                        return;
                    }
                    pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                    WSABUF wsabuf = {8192 - (ULONG)*hlen, hbuf + *hlen};
                    DWORD flags = 0, dummy = 0;
                    WSARecv(cfd, &wsabuf, 1, &dummy, &flags, (LPOVERLAPPED)cctx, NULL);
                    return;
                }
                // 公共请求行解析：尾部 CRLF 自动剥离，不修改缓冲
                char method[16] = {0}, path[4096] = {0}, proto[32] = {0};
                if (nl_px_parse_request_line(hbuf, (size_t)(line_end - hbuf),
                                             method, sizeof(method),
                                             path, sizeof(path),
                                             proto, sizeof(proto)) != 0) {
                    send_http_error(cfd, 400, "Bad Request");
                    closesocket(cfd);
                    pr_free_slot(s, idx);
                    return;
                }
                /* ---- 第205轮 CONNECT：HTTP CONNECT 真隧道 + 目标白名单 ----
                 * path 是 authority（host:port，无 '/'），查路由表必 404 —— 必须
                 * 在 pr_lookup_route_idx 之前分发。空名单 = 拒绝所有 CONNECT
                 * （默认安全）。隧道自成一体：up_parse=TUNNEL 后双向裸字节经
                 * CLIENT_READ / UPSTREAM_READ 完成分支透传，不走 KA 复用收口。 */
                if (strcmp(method, "CONNECT") == 0) {
                    /* a. 白名单检查：host 精确匹配（不区分大小写），port=0 任意端口 */
                    char thost[256];
                    int tport = 443;
                    int allowed = 0;
                    if (nl_px_split_hostport(path, thost, sizeof(thost), &tport, 443)) {
                        EnterCriticalSection(&s->mutex);
                        for (int ai = 0; ai < s->connect_allow_cnt; ai++) {
                            if ((s->connect_allow_port[ai] == 0 ||
                                 s->connect_allow_port[ai] == tport) &&
                                _stricmp(s->connect_allow_host[ai], thost) == 0) {
                                allowed = 1;
                                break;
                            }
                        }
                        LeaveCriticalSection(&s->mutex);
                    }
                    if (!allowed) {
                        send_http_error(cfd, 403, "Forbidden");
                        pr_close_defer(defer, ndefer, cfd);
                        pr_free_slot(s, idx);
                        return;
                    }
                    /* 请求头必须收齐（\r\n\r\n）：否则再 post 一次 WSARecv 等剩余头 */
                    char* che = NULL;
                    for (int i = (int)(line_end - hbuf) + 1; i + 3 < *hlen; i++) {
                        if (hbuf[i] == '\r' && hbuf[i + 1] == '\n' &&
                            hbuf[i + 2] == '\r' && hbuf[i + 3] == '\n') {
                            che = hbuf + i;
                            break;
                        }
                    }
                    if (!che) {
                        if (*hlen < 8192) {
                            pr_iocp_ctx_t* cctx0 = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                            memset(cctx0, 0, sizeof(*cctx0));
                            cctx0->idx = idx;
                            cctx0->conn_id = s->pr_conn_id[idx];
                            cctx0->ev = IOCP_EV_CLIENT_READ;
                            cctx0->sock = cfd;
                            cctx0->buf = hbuf;
                            cctx0->buflen = 8192;
                            WSABUF wb0 = {8192, hbuf};
                            DWORD fl0 = 0, dm0 = 0;
                            WSARecv(cfd, &wb0, 1, &dm0, &fl0, (LPOVERLAPPED)cctx0, NULL);
                            return;
                        }
                        send_http_error(cfd, 400, "Bad Request");
                        pr_close_defer(defer, ndefer, cfd);
                        pr_free_slot(s, idx);
                        return;
                    }
                    /* b. 建连：阻塞 connect（5 秒超时），不走 route/up_pool */
                    SOCKET ufd_new = pr_connect_direct(thost, tport);
                    if (ufd_new == INVALID_SOCKET) {
                        send_http_error(cfd, 502, "Bad Gateway");
                        pr_close_defer(defer, ndefer, cfd);
                        pr_free_slot(s, idx);
                        return;
                    }
                    /* c. 回 200 Connection Established（临时切阻塞保证一次写完） */
                    static const char ok_hdr[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
                    u_long nb_off200 = 0;
                    ioctlsocket(cfd, FIONBIO, &nb_off200);
                    int ok_sent = 0;
                    while (ok_sent < (int)sizeof(ok_hdr) - 1) {
                        int w = send(cfd, ok_hdr + ok_sent,
                                     (int)sizeof(ok_hdr) - 1 - ok_sent, 0);
                        if (w <= 0) break;
                        ok_sent += w;
                    }
                    u_long nb_on200 = 1;
                    ioctlsocket(cfd, FIONBIO, &nb_on200);   /* 隧道期客户端保持非阻塞 */
                    if (ok_sent != (int)sizeof(ok_hdr) - 1) {
                        closesocket(ufd_new);
                        pr_close_defer(defer, ndefer, cfd);
                        pr_free_slot(s, idx);
                        return;
                    }
                    /* 头后与 CONNECT 同包到达的残留字节属于隧道负载，转给上游 */
                    int hdr_total = (int)(che + 4 - hbuf);
                    int leftn = *hlen - hdr_total;
                    if (leftn > 0) {
                        u_long nb_offl = 0;
                        ioctlsocket(ufd_new, FIONBIO, &nb_offl);
                        int offl = 0;
                        while (offl < leftn) {
                            int w = send(ufd_new, hbuf + hdr_total + offl, leftn - offl, 0);
                            if (w <= 0) break;
                            offl += w;
                        }
                    }
                    u_long nb_onu = 1;
                    ioctlsocket(ufd_new, FIONBIO, &nb_onu);
                    /* d. 进入隧道泵：关联 IOCP + 双向 post WSARecv；
                     *    up_parse=TUNNEL 让泵只做裸字节透传，不再做 HTTP 解析。
                     *    任一侧 EOF/错误 → CLIENT_READ / UPSTREAM_READ 完成分支
                     *    统一关双端回收槽位。 */
                    CreateIoCompletionPort((HANDLE)ufd_new, s->iocp, (ULONG_PTR)idx, 0);
                    s->pr_fd_upstream[idx] = ufd_new;
                    s->pr_route_idx[idx]   = -1;
                    s->pr_up_req_len[idx]  = 0;
                    s->pr_up_pend_len[idx] = 0;
                    s->pr_state[idx]       = PR_ST_DIR_W | PR_ST_ACTIVE;   /* KA/DRAIN 全清：不回 KA 复用 */
                    s->pr_up[idx].up_parse = PR_UP_TUNNEL;
                    *hlen = 0;   /* 请求头已消费，之后字节全是隧道负载 */
                    if (!s->pr_up_pend[idx]) {
                        s->pr_up_pend[idx] = (char*)malloc(PR_PROXY_BUF_SZ);
                        if (!s->pr_up_pend[idx]) {
                            closesocket(ufd_new);
                            s->pr_fd_upstream[idx] = 0;
                            pr_close_defer(defer, ndefer, cfd);
                            pr_free_slot(s, idx);
                            return;
                        }
                    }
                    /* 上游→客户端方向：post 上游 WSARecv */
                    pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
                    memset(uctx, 0, sizeof(*uctx));
                    uctx->idx = idx;
                    uctx->conn_id = s->pr_conn_id[idx];
                    uctx->ev  = IOCP_EV_UPSTREAM_READ;
                    uctx->sock = ufd_new;
                    uctx->buf  = s->pr_up_pend[idx];
                    uctx->buflen = PR_PROXY_BUF_SZ;
                    WSABUF wsb_up = {PR_PROXY_BUF_SZ, s->pr_up_pend[idx]};
                    DWORD flg_up = 0, dummy_up = 0;
                    WSARecv(ufd_new, &wsb_up, 1, &dummy_up, &flg_up, (LPOVERLAPPED)uctx, NULL);
                    /* 客户端→上游方向：再 post 客户端 WSARecv（隧道裸字节） */
                    pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                    memset(cctx, 0, sizeof(*cctx));
                    cctx->idx = idx;
                    cctx->conn_id = s->pr_conn_id[idx];
                    cctx->ev  = IOCP_EV_CLIENT_READ;
                    cctx->sock = cfd;
                    cctx->buf  = hbuf;
                    cctx->buflen = 8192;
                    WSABUF wsabuf_c = {8192, hbuf};
                    DWORD flg_c = 0, dummy_c = 0;
                    WSARecv(cfd, &wsabuf_c, 1, &dummy_c, &flg_c, (LPOVERLAPPED)cctx, NULL);
                    return;
                }
                s->pr_up[idx].req_is_head = (strcmp(method, "HEAD") == 0) ? 1 : 0;

                // 第204轮缺陷4：一次临界区内拿到下标+指针（Windows 临界区同线程
                // 可递归进入，外层只产生一次真实争用，替代原先两次 Enter/Leave）；
                // 并缓存给 plan_build 内部 lookup，免再走两次 s->mutex。
                nl_web_route_t* r = NULL;
                int ridx;
                EnterCriticalSection(&s->mutex);
                ridx = pr_lookup_route_idx(s, path);
                if (ridx >= 0) {
                    int cur = 0;
                    for (nl_web_route_t* it = s->routes; it; it = it->next, cur++)
                        if (cur == ridx) { r = it; break; }
                }
                LeaveCriticalSection(&s->mutex);
                if (!r) {
                    send_http_error(cfd, 404, "Not Found");
                    closesocket(cfd);
                    pr_free_slot(s, idx);
                    return;
                }
                g_pr_pre_route = r;   /* 同请求内 plan_build 一次性消费 */
                snprintf(g_pr_pre_path, sizeof(g_pr_pre_path), "%s", path);

                // ---------- 请求头完整性检查 ----------
                // HTTP/HTTPS 上游必须等客户端头收齐（\r\n\r\n）才能改写构造转发；
                // TCP 原始透传不需要 HTTP 头结构，整包直接交给上游。
                int is_tcp_proxy = (r->type == NL_ROUTE_TYPE_PROXY && r->protocol == NL_PROXY_TCP);
                char* hdr_end = NULL;
                if (!is_tcp_proxy) {
                    int start = (int)(line_end - hbuf) + 1;
                    for (int i = start; i + 3 < *hlen; i++) {
                        if (hbuf[i] == '\r' && hbuf[i + 1] == '\n' &&
                            hbuf[i + 2] == '\r' && hbuf[i + 3] == '\n') {
                            hdr_end = hbuf + i;
                            break;
                        }
                    }
                    if (!hdr_end) {
                        if (*hlen >= 8192) {
                            send_http_error(cfd, 400, "Bad Request");
                            closesocket(cfd);
                            pr_free_slot(s, idx);
                            return;
                        }
                        // 头未收齐：再 post 一次 WSARecv 等待剩余头字节
                        pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                        WSABUF wsabuf = {8192 - (ULONG)*hlen, hbuf + *hlen};
                        DWORD flags = 0, dummy = 0;
                        WSARecv(cfd, &wsabuf, 1, &dummy, &flags, (LPOVERLAPPED)cctx, NULL);
                        return;
                    }
                }

                /* 第204轮：keep-alive / Content-Length 判定（公共层统一规则：
                 * HTTP/1.1 默认持久、1.0 默认关闭；chunked 或非法 CL 一律关闭）。
                 * TCP 原始透传不适用 HTTP 语义，保持默认关闭。 */
                int req_ka = 0;
                long long req_cl = 0;
                if (!is_tcp_proxy) {
                    char*  hdrs = hbuf + (line_end - hbuf) + 1;
                    size_t hdrs_len = (size_t)((hdr_end + 4) - hdrs);
                    req_ka = nl_px_req_keepalive(proto, hdrs, hdrs_len);
                    req_cl = nl_px_req_content_length(hdrs, hdrs_len);
                    if (req_cl < 0) req_cl = 0;
                    if (req_ka) s->pr_state[idx] |= PR_ST_KA;
                    else        s->pr_state[idx] &= (uint8_t)~PR_ST_KA;
                }

                /* 第205轮：TRACE 回显（RFC 9110 §9.3.1）—— 200 + message/http，
                 * body = 原始请求（行+头）。必须在下方 body 剥离前取字节
                 * （剥离会 memmove 掉头块）；同步构造，分发处发送后走状态机收口。 */
                char* trace_buf = NULL;
                int   trace_len = 0;
                if (!is_tcp_proxy && r->type != NL_ROUTE_TYPE_PROXY &&
                    strcmp(method, "TRACE") == 0) {
                    int raw_tlen = (int)(hdr_end + 4 - hbuf);
                    if (raw_tlen < 0) raw_tlen = 0;
                    char shead[256];
                    int hn = snprintf(shead, sizeof(shead),
                                      "HTTP/1.1 200 OK\r\n"
                                      "Content-Type: message/http\r\n"
                                      "Content-Length: %d\r\n"
                                      "Connection: %s\r\n\r\n",
                                      raw_tlen, req_ka ? "keep-alive" : "close");
                    if (hn > 0) {
                        trace_buf = (char*)malloc((size_t)hn + (size_t)raw_tlen + 1);
                        if (trace_buf) {
                            memcpy(trace_buf, shead, (size_t)hn);
                            memcpy(trace_buf + hn, hbuf, (size_t)raw_tlen);
                            trace_len = hn + raw_tlen;
                        }
                    }
                    if (!trace_buf) {
                        send_http_error(cfd, 500, "Internal Server Error");
                        pr_close_defer(defer, ndefer, cfd);
                        pr_free_slot(s, idx);
                        return;
                    }
                }

                /* HTTP/1.1 Upgrade: h2c —— 请求头已收齐时移交公共层 H2 桥接，
                 * 这条 HTTP/1.1 请求本身作为 stream 1；平台此后不再处理该连接。 */
                if (!is_tcp_proxy && hdr_end &&
                    nl_px_h2_upgrade_check(line_end + 1,
                        (size_t)(hdr_end - (line_end + 1))) == 1) {
                    (void)pr_h2_start_bridge(s, idx, cfd, hbuf, *hlen, 1);
                    return;
                }

                // 原始客户端头（首行之后、空行之前）与首包中已到的 body
                const char* raw;
                size_t raw_len, body_off;
                if (is_tcp_proxy) {
                    raw = hbuf;
                    raw_len = (size_t)*hlen;
                    body_off = (size_t)*hlen;
                } else {
                    raw = line_end + 1;
                    raw_len = (size_t)(hdr_end - raw);
                    body_off = (size_t)(hdr_end - hbuf) + 4;
                }
                size_t body_len = (size_t)*hlen - body_off;

                /* 第204轮：本地路由 body 剥离 —— body 无人消费，复用前必须从缓冲
                 * 剥离：已全到 → 压缩保留其后的管道化残留；未到全 → 记入 drain 等
                 * 响应完成后异步收齐（DRAIN 阶段）。proxy 路由不剥离（raw/body_off
                 * 指向的首包 body 还要转发给上游）。 */
                if (!is_tcp_proxy && r->type != NL_ROUTE_TYPE_PROXY) {
                    int boff = (int)(hdr_end + 4 - hbuf);
                    int already = *hlen - boff;
                    if (req_cl > 0) {
                        if ((long long)already >= req_cl) {
                            int tail = (int)((long long)already - req_cl);
                            if (tail > 0) memmove(hbuf, hbuf + boff + (int)req_cl, (size_t)tail);
                            *hlen = tail;
                            if (s->pr_req_drain) s->pr_req_drain[idx] = 0;
                        } else {
                            *hlen = 0;
                            if (s->pr_req_drain) s->pr_req_drain[idx] = req_cl - already;
                        }
                    } else {
                        if (already > 0) memmove(hbuf, hbuf + boff, (size_t)already);
                        *hlen = already;
                        if (s->pr_req_drain) s->pr_req_drain[idx] = 0;
                    }
                    hbuf[*hlen] = '\0';
                }

                // ---------- 完整混合路由分发 ----------
                if (r->type != NL_ROUTE_TYPE_PROXY) {
                    // 第205轮：TRACE 回显已构造 —— 同步发送后按状态机收口
                    if (trace_buf) {
                        u_long nb_off_t = 0;
                        ioctlsocket(cfd, FIONBIO, &nb_off_t);   /* 临时切阻塞发完 */
                        send(cfd, trace_buf, trace_len, 0);
                        free(trace_buf);
                        int has_drain_t = (s->pr_req_drain && s->pr_req_drain[idx] > 0);
                        if (has_drain_t) {
                            s->pr_state[idx] |= PR_ST_DRAIN;
                            pr_post_drain_recv(s, idx);
                            return;
                        }
                        if ((s->pr_state[idx] & PR_ST_KA) && *hlen == 0) {
                            pr_conn_keep_win(s, idx);
                            return;
                        }
                        pr_close_defer(defer, ndefer, cfd);
                        pr_free_slot(s, idx);
                        return;
                    }
                    // 非 proxy：临时切阻塞发响应
                    u_long nb_off = 0;
                    ioctlsocket(cfd, FIONBIO, &nb_off);

                    // 查路由 + 状态 + 响应头统一由公共层生成（与 H2 进程内分发同源）；
                    // body 仍按平台最优方式发送：CONTENT 直接 send，FILE 走 TransmitFile 零拷贝。
                    // 第204轮：keep-alive 由状态机判定（PR_ST_KA 位）传入，头与复用行为一致。
                    int req_ka_bit = (s->pr_state[idx] & PR_ST_KA) != 0;
                    nl_px_local_plan plan;
                    int built = nl_px_local_plan_build(s, &g_pr_local_ops, path,
                                                       s->pr_up[idx].req_is_head,
                                                       req_ka_bit, &plan);
                    int has_drain = (s->pr_req_drain && s->pr_req_drain[idx] > 0);
                    /* 第191轮 CONTENT 快路径（锁外发完即关）：仅 ka=0 且无待消费
                     * body 时可用 —— ka=1 必须走同步路径才能发完后复用连接，
                     * body 未收齐必须走 DRAIN 异步收完再关（防带未读数据 RST）。 */
                    if (built == 1 && plan.type == NL_PX_LOCAL_CONTENT &&
                        !req_ka_bit && !has_drain && *ndefer < PR_DEFER_CLOSE_MAX) {
                        pr_defer_t* d = &defer[(*ndefer)++];
                        d->kind = PR_DEFER_CLOSE;
                        d->fd = cfd;
                        d->plan = plan;
                        d->has_plan = 1;
                        pr_free_slot(s, idx);
                        return;
                    }
                    if (built == 1) {
                        if (plan.head_len > 0)
                            send(cfd, plan.head, (int)plan.head_len, 0);
                        if (plan.type == NL_PX_LOCAL_CONTENT && plan.body_len > 0) {
                            send(cfd, plan.body, (int)plan.body_len, 0);
                        } else if (plan.type == NL_PX_LOCAL_FILE && plan.file_size > 0 &&
                                   !s->pr_up[idx].req_is_head) {
                            HANDLE hFile = CreateFileA(plan.file_path, GENERIC_READ,
                                                       FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                                       NULL);
                            if (hFile != INVALID_HANDLE_VALUE) {
                                TransmitFile(cfd, hFile, 0, 0, NULL, NULL, 0);
                                CloseHandle(hFile);
                            }
                        }
                    } else {
                        send_http_error(cfd, 404, "Not Found");
                    }
                    /* 第204轮：状态机收口 —— 响应已发完，不再无条件关闭：
                     * ① body 未收齐 → 异步 DRAIN，收完再按 KA 位定去向；
                     * ② ka=1 且无管道化残留 → 复用连接（修复缺陷5 的长连接用例）；
                     * ③ 其余（ka=0 / 错误响应固定 close / 有残留）→ 延迟关闭。 */
                    if (built == 1 && plan.status < 400) {
                        if (has_drain) {
                            s->pr_state[idx] |= PR_ST_DRAIN;
                            pr_post_drain_recv(s, idx);
                            return;
                        }
                        if (req_ka_bit && *hlen == 0) {
                            pr_conn_keep_win(s, idx);
                            return;
                        }
                        /* ka=1 但 hbuf 有管道化残留：暂不支持复用中续解析，
                         * 按 RFC 允许拒绝 —— 关闭（顺序请求不受影响，残留=0）。 */
                    }
                    // 第191轮：closesocket 延迟到锁外执行（见 consume loop）
                    pr_close_defer(defer, ndefer, cfd);   // 取消所有 pending I/O
                    pr_free_slot(s, idx);
                    return;
                }

                // ---------- proxy 路由：优先复用池中 keep-alive 连接 ----------
                // 第190轮：proxy 专用缓冲（8KB 请求草稿 + 576B 池 key）延迟到分发时才分配
                if (!s->pr_hbuf_out[idx])
                    s->pr_hbuf_out[idx] = (char*)malloc(8192);
                if (!s->pr_up_key[idx])
                    s->pr_up_key[idx] = (char*)malloc(576);
                if (!s->pr_hbuf_out[idx] || !s->pr_up_key[idx]) {
                    send_http_error(cfd, 500, "Internal Server Error");
                    pr_close_defer(defer, ndefer, cfd);
                    s->pr_fd_client[idx] = INVALID_SOCKET;
                    pr_free_slot(s, idx);
                    return;
                }
                // 解析上游 host:port（供池 key 使用；tcp 不参与池）
                char up_host[512];
                int  up_port = 0;
                {
                    int def_port = (r->protocol == NL_PROXY_HTTPS) ? 443 : 80;
                    up_port = def_port;
                    if (!nl_px_split_hostport(r->upstream, up_host, sizeof(up_host),
                                              &up_port, def_port))
                        snprintf(up_host, sizeof(up_host), "%s", r->upstream);
                    if (up_port <= 0) up_port = def_port;
                }
                SOCKET ufd_new = INVALID_SOCKET;
                // HTTPS 上游连接带 TLS 会话状态，不能与裸 socket 池混用
                if (r->protocol != NL_PROXY_TCP && r->protocol != NL_PROXY_HTTPS) {
                    ufd_new = (SOCKET)nl_px_pool_get(s->up_pool, up_host, up_port, (int)r->protocol);
                    if (ufd_new != INVALID_SOCKET) {
                        // 命中池：重关联到当前 slot（completion key = idx）
                        CreateIoCompletionPort((HANDLE)ufd_new, s->iocp, (ULONG_PTR)idx, 0);
                    }
                }
                if (ufd_new == INVALID_SOCKET) {
                    // ---- 异步连接上游（ConnectEx）：不阻塞引擎消费线程/其它连接 ----
                    // 先构造请求写入 pr_hbuf_out[idx] 并记长度，连接完成后由
                    // IOCP_EV_UPSTREAM_CONNECT 分支负责 TLS 握手 + 发送 + post 读。
                    char* ubuf_async = s->pr_hbuf_out[idx];
                    int build_n = pr_build_upstream_http(s, idx, method, path, proto,
                                                         raw, raw_len, ubuf_async, 8192);
                    // 首包中已到的 body 追加到请求尾部（异步发送走 pr_hbuf_out 一次发完）
                    if (build_n > 0 && body_len > 0) {
                        size_t need = (size_t)build_n + body_len;
                        if (need > 8192) {
                            char* nb = (char*)realloc(s->pr_hbuf_out[idx], need);
                            if (nb) { s->pr_hbuf_out[idx] = nb; ubuf_async = nb; }
                            else build_n = -1;
                        }
                        if (build_n > 0) {
                            memcpy(ubuf_async + build_n, hbuf + body_off, body_len);
                            build_n += (int)body_len;
                        }
                    }
                    if (build_n > 0 && nl_proxy_start_connect_async(s, idx, r) > 0) {
                        // 记录池 key / 路由（归还与 TLS 握手需要），随后等完成事件
                        if (r->protocol != NL_PROXY_TCP && r->protocol != NL_PROXY_HTTPS) {
                            nl_px_pool_make_key(s->pr_up_key[idx], 576, up_host, up_port, (int)r->protocol);
                        } else {
                            s->pr_up_key[idx][0] = '\0';
                        }
                        s->pr_route_idx[idx]  = ridx;
                        s->pr_up_req_len[idx] = build_n;
                        s->pr_state[idx]      = PR_ST_DIR_W | PR_ST_ACTIVE;
                        *hlen = 0;   // 头+首包 body 已全部交给上游请求缓冲
                        return;   // 等待 IOCP_EV_UPSTREAM_CONNECT 完成事件
                    }
                    s->pr_up_req_len[idx] = 0;
                    // 回退：ConnectEx 不可用 / 投递失败，走同步 connect
                    ufd_new = nl_proxy_connect_upstream(r);
                }
                if (ufd_new == INVALID_SOCKET) {
                    send_http_error(cfd, 502, "Bad Gateway");
                    closesocket(cfd);
                    pr_free_slot(s, idx);
                    return;
                }
                // 记录池 key（归还时使用）；tcp/https 置空表示不参与池
                if (r->protocol != NL_PROXY_TCP && r->protocol != NL_PROXY_HTTPS) {
                    nl_px_pool_make_key(s->pr_up_key[idx], 576, up_host, up_port, (int)r->protocol);
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
                int build_n = pr_build_upstream_http(s, idx, method, path, proto, raw, raw_len, ubuf, 8192);
                if (build_n > 0 && body_len > 0) {
                    size_t need = (size_t)build_n + body_len;
                    if (need > 8192) {
                        char* nb = (char*)realloc(s->pr_hbuf_out[idx], need);
                        if (nb) { s->pr_hbuf_out[idx] = nb; ubuf = nb; }
                        else build_n = -1;
                    }
                    if (build_n > 0) {
                        memcpy(ubuf + build_n, hbuf + body_off, body_len);
                        build_n += (int)body_len;
                    }
                }
                // HTTPS 上游（同步 connect 回退路径）：同样交给 TLS 隧道线程
                if (r->protocol == NL_PROXY_HTTPS) {
                    (void)pr_tls_start_tunnel(s, idx, cfd, ufd_new, up_host,
                                              ubuf, build_n > 0 ? build_n : 0);
                    *hlen = 0;
                    return;
                }
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
                *hlen = 0;   // 首包已消费（请求头+首包 body 均已交给上游）

                // post 上游 WSARecv（响应回来 → pump 回客户端）
                // 懒分配 per-slot 上游响应缓冲（pr_up_pend[idx]），替换共享 pump_buf 避免并发竞争
                if (!s->pr_up_pend[idx]) {
                    s->pr_up_pend[idx] = (char*)malloc(PR_PROXY_BUF_SZ);
                    if (!s->pr_up_pend[idx]) {
                        if (ufd_new > 0) { closesocket(ufd_new); s->pr_fd_upstream[idx] = 0; }
                        closesocket(cfd); s->pr_fd_client[idx] = INVALID_SOCKET;
                        pr_free_slot(s, idx);
                        return;
                    }
                }
                pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
                memset(uctx, 0, sizeof(*uctx));
                uctx->idx = idx;
                uctx->conn_id = s->pr_conn_id[idx];
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
                cctx->conn_id = s->pr_conn_id[idx];
                cctx->ev  = IOCP_EV_CLIENT_READ;
                cctx->sock = cfd;
                cctx->buf  = hbuf;
                cctx->buflen = 8192;
                WSABUF wsabuf2 = {8192, hbuf};
                DWORD flags2 = 0, dummy2 = 0;
                WSARecv(cfd, &wsabuf2, 1, &dummy2, &flags2, (LPOVERLAPPED)cctx, NULL);

                /* 第204轮：保留 PR_ST_KA（完成点复用判定依据），清 UP_EOF/DRAIN 等旧状态 */
s->pr_state[idx] = (s->pr_state[idx] & PR_ST_KA) | PR_ST_DIR_W | PR_ST_ACTIVE;
                return;
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
                *hlen = 0;   // 第204轮：body 已转发，缓冲归零（否则 hlen 跨段累加
                             // 会越界，且 proxy 完成点复用判定要求 hlen==0）
                // 继续 post 客户端 WSARecv
                pr_iocp_ctx_t* cctx = &((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[idx];
                memset(cctx, 0, sizeof(*cctx));
                cctx->idx = idx;
                cctx->conn_id = s->pr_conn_id[idx];
                cctx->ev  = IOCP_EV_CLIENT_READ;
                cctx->sock = cfd;
                cctx->buf  = hbuf;
                cctx->buflen = 8192;
                WSABUF wsabuf3 = {8192, hbuf};
                DWORD flags3 = 0, dummy3 = 0;
                WSARecv(cfd, &wsabuf3, 1, &dummy3, &flags3, (LPOVERLAPPED)cctx, NULL);
            }
            return;
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
                return;
            }

            if (nread_up > 0 && s->pr_last_active_ms) s->pr_last_active_ms[idx] = pr_pool_now_ms();  // P7: 上游数据到达，刷新活跃时间

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
                return;
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
                wctx->conn_id = s->pr_conn_id[idx];
                wctx->ev  = IOCP_EV_CLIENT_WRITE;
                wctx->sock = cfd;
                wctx->buf  = s->pr_up_pend[idx];
                wctx->buflen = (DWORD)rem;
                WSABUF wsb_w = {(ULONG)rem, s->pr_up_pend[idx]};
                DWORD wsent = 0, wflg = 0;
                WSASend(cfd, &wsb_w, 1, &wsent, wflg, (LPOVERLAPPED)wctx, NULL);
                // 补发期间不 post 上游 WSARecv，避免响应字节乱序
                return;
            }

            // 响应完整且已全部转发给客户端 → 上游归还池；客户端按 keep-alive 复用/关闭
            if (s->pr_up[idx].up_parse == PR_UP_DONE) {
                if (s->pr_up_key[idx] && s->pr_up_key[idx][0] != '\0') {
                    // 归还池（不关闭 socket；IOCP 无显式摘除 API，重关联时更新 key）
                    nl_px_pool_put(s->up_pool, s->pr_up_key[idx], (intptr_t)ufd);
                } else {
                    closesocket(ufd);   // 不参与池（如 tcp）直接关闭
                }
                s->pr_fd_upstream[idx] = 0;
                /* 第204轮：上游响应 framing=keep-alive 时透传给客户端的头也是
                 * keep-alive，连接必须真的复用（修复缺陷1 头与行为不一致）。
                 * 客户端侧 WSARecv 仍在挂起（分发/body 转发时 post），无需再 post。 */
                if (s->pr_state[idx] & PR_ST_KA) {
                    s->pr_state[idx]    = PR_ST_ACTIVE;   // 清 DIR_W / KA / UP_EOF
                    s->pr_route_idx[idx] = -1;
                    s->pr_remain[idx]   = 0;
                    s->pr_up_pend_len[idx] = 0;
                    s->pr_up_req_len[idx]  = 0;
                    nl_px_upstate_reset(&s->pr_up[idx]);
                    if (s->pr_last_active_ms) s->pr_last_active_ms[idx] = pr_pool_now_ms();
                    return;
                }
                closesocket(cfd);
                s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                return;
            }

            // 继续 post 上游 WSARecv
            if (ufd > 0 && (s->pr_state[idx] & PR_ST_ACTIVE)) {
                pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
                memset(uctx, 0, sizeof(*uctx));
                uctx->idx = idx;
                uctx->conn_id = s->pr_conn_id[idx];
                uctx->ev  = IOCP_EV_UPSTREAM_READ;
                uctx->sock = ufd;
                uctx->buf  = s->pr_up_pend[idx];
                uctx->buflen = PR_PROXY_BUF_SZ;
                WSABUF wsb_up2 = {PR_PROXY_BUF_SZ, s->pr_up_pend[idx]};
                DWORD flg_up2 = 0, dummy_up2 = 0;
                WSARecv(ufd, &wsb_up2, 1, &dummy_up2, &flg_up2, (LPOVERLAPPED)uctx, NULL);
            }
            return;
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
                return;
            }
            if (sent < rem) {
                // 仍有剩余：前移后重新投递 WSASend
                memmove(s->pr_up_pend[idx], s->pr_up_pend[idx] + sent, (size_t)(rem - sent));
                s->pr_up_pend_len[idx] = rem - sent;
                pr_iocp_ctx_t* wctx2 = &((pr_iocp_ctx_t*)s->pr_iocp_cw_ctx)[idx];
                memset(wctx2, 0, sizeof(*wctx2));
                wctx2->idx = idx;
                wctx2->conn_id = s->pr_conn_id[idx];
                wctx2->ev  = IOCP_EV_CLIENT_WRITE;
                wctx2->sock = cfd;
                wctx2->buf  = s->pr_up_pend[idx];
                wctx2->buflen = (DWORD)(rem - sent);
                WSABUF wsb_w2 = {(ULONG)(rem - sent), s->pr_up_pend[idx]};
                DWORD wsent2 = 0, wflg2 = 0;
                WSASend(cfd, &wsb_w2, 1, &wsent2, wflg2, (LPOVERLAPPED)wctx2, NULL);
                return;
            }

            // 余量全部补发完成
            s->pr_up_pend_len[idx] = 0;
            // 响应完整 → 上游归还池；客户端按 keep-alive 复用/关闭（与上游读完成分支一致）
            if (s->pr_up[idx].up_parse == PR_UP_DONE) {
                if (s->pr_up_key[idx] && s->pr_up_key[idx][0] != '\0') {
                    nl_px_pool_put(s->up_pool, s->pr_up_key[idx], (intptr_t)ufd);
                } else {
                    closesocket(ufd);
                }
                s->pr_fd_upstream[idx] = 0;
                /* 第204轮：复用判定同上游读完成分支（见上） */
                if (s->pr_state[idx] & PR_ST_KA) {
                    s->pr_state[idx]    = PR_ST_ACTIVE;
                    s->pr_route_idx[idx] = -1;
                    s->pr_remain[idx]   = 0;
                    s->pr_up_pend_len[idx] = 0;
                    s->pr_up_req_len[idx]  = 0;
                    nl_px_upstate_reset(&s->pr_up[idx]);
                    if (s->pr_last_active_ms) s->pr_last_active_ms[idx] = pr_pool_now_ms();
                    return;
                }
                closesocket(cfd);
                s->pr_fd_client[idx] = INVALID_SOCKET;
                pr_free_slot(s, idx);
                return;
            }
            // 响应未完整：继续 post 上游 WSARecv
            if (ufd > 0 && (s->pr_state[idx] & PR_ST_ACTIVE)) {
                pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
                memset(uctx, 0, sizeof(*uctx));
                uctx->idx = idx;
                uctx->conn_id = s->pr_conn_id[idx];
                uctx->ev  = IOCP_EV_UPSTREAM_READ;
                uctx->sock = ufd;
                uctx->buf  = s->pr_up_pend[idx];
                uctx->buflen = PR_PROXY_BUF_SZ;
                WSABUF wsb_up3 = {PR_PROXY_BUF_SZ, s->pr_up_pend[idx]};
                DWORD flg_up3 = 0, dummy_up3 = 0;
                WSARecv(ufd, &wsb_up3, 1, &dummy_up3, &flg_up3, (LPOVERLAPPED)uctx, NULL);
            }
            return;
        }
        // 其他 ev 类型：忽略
}

// P6: IOCP 完成消费循环。GQCS 等待本身不持锁；完成事件处理段持 pr_loop_cs 串行化
// （引擎 slot/pool 状态为无锁单线程设计；nl_proxy_connect_upstream 阻塞 connect
// 亦在锁内执行，与单线程现状串行语义一致，无回归）。
// P7: 空闲超时扫描节流（~500ms 一轮），超过 idle_ms 未活跃的连接强关回收（slowloris 防护）。
static void pr_iocp_consume_loop(nl_web_server_t* s, SOCKET listen_sock) {
    int cap = s->pr_cap;
    // P7: 空闲超时（slowloris 防护）。0=默认 30s；<=0 视为默认
    int idle_ms = s->idle_timeout_ms;
    if (idle_ms <= 0) idle_ms = 30000;
    int64_t last_scan_ms = pr_pool_now_ms();
    while (s->running) {
        DWORD bytes_trans;
        ULONG_PTR compkey;
        LPOVERLAPPED p_ov = NULL;
        BOOL ok = GetQueuedCompletionStatus(s->iocp, &bytes_trans, &compkey, &p_ov, 200);
        DWORD gle = GetLastError();
        if (!ok && gle == WAIT_TIMEOUT) {
            // 空闲退避：持锁回收空闲超时池连接 + P7 空闲连接扫描（每 ~500ms 一轮）
            pr_defer_t tdefer[PR_DEFER_CLOSE_MAX];   // 第191轮：pending 接管的 WSARecv 锁外 post
            int ntdefer = 0;
            EnterCriticalSection(&s->pr_loop_cs);
            // AcceptEx 停摆自愈：上一次 post 失败（WSASocket 耗尽/AcceptEx 同步错误）
            // 后 pr_accept_socks[] 全为空且无完成事件驱动，只有超时分支能补投
            // 第190轮：背压恢复 —— ring 满暂存的连接拿到空槽后接管并恢复 accept
            if (s->pr_accept_paused) {
                if (s->pr_pending_client != INVALID_SOCKET) {
                    int pidx = pr_alloc_slot(s);
                    if (pidx >= 0) {
                        SOCKET pend = s->pr_pending_client;
                        s->pr_pending_client = INVALID_SOCKET;
                        EnterCriticalSection(&s->pr_accept_cs);   // paused 写同步于 post 的读
                        s->pr_accept_paused = 0;
                        LeaveCriticalSection(&s->pr_accept_cs);
                        pr_iocp_ctx_t* cctx = pr_adopt_client_locked(s, pidx, pend);
                        if (cctx && ntdefer < PR_DEFER_CLOSE_MAX) {
                            pr_defer_t* d = &tdefer[ntdefer++];
                            d->kind = PR_DEFER_RECV;
                            d->fd = pend;
                            d->idx = pidx;
                            d->has_plan = 0;
                        } else if (cctx) {
                            WSABUF wb; wb.buf = s->pr_hbuf[pidx]; wb.len = 8192;
                            DWORD fl = 0, dm = 0;
                            WSARecv(pend, &wb, 1, &dm, &fl, (LPOVERLAPPED)cctx, NULL);
                        }
                    }
                } else {
                    EnterCriticalSection(&s->pr_accept_cs);
                    s->pr_accept_paused = 0;
                    LeaveCriticalSection(&s->pr_accept_cs);
                }
            }
            nl_px_pool_reap(s->up_pool, pr_pool_now_ms());
            {
                int64_t now_ms = pr_pool_now_ms();
                if (now_ms - last_scan_ms >= 500) {
                    last_scan_ms = now_ms;
                    for (int si = 0; si < cap; si++) {
                        if (!(s->pr_state[si] & PR_ST_ACTIVE)) continue;
                        if (s->pr_state[si] & PR_ST_TLS) continue;   // TLS 隧道自管超时
                        int64_t last = s->pr_last_active_ms ? s->pr_last_active_ms[si] : 0;
                        if (last <= 0 || (now_ms - last) >= idle_ms) {
                            SOCKET ufd_t = s->pr_fd_upstream[si];
                            SOCKET cfd_t = s->pr_fd_client[si];
                            if (ufd_t > 0) { closesocket(ufd_t); s->pr_fd_upstream[si] = 0; }
                            if (cfd_t != INVALID_SOCKET) closesocket(cfd_t);
                            // 空闲强关：pr_free_slot 内递增 conn_id，迟到完成事件将被丢弃
                            pr_free_slot(s, si);
                        }
                    }
                }
            }
            LeaveCriticalSection(&s->pr_loop_cs);
            // 第191轮：全量补投 AcceptEx 与接管连接的 WSARecv 移到锁外执行
            //（WSASocket/AcceptEx/WSARecv 合计 ~450us，锁内串行会拖垮读处理）
            pr_repost_accept_slots(s, listen_sock);
            for (int i = 0; i < ntdefer; i++) {
                WSABUF wb;
                wb.buf = s->pr_hbuf[tdefer[i].idx];
                wb.len = 8192;
                DWORD fl = 0, dm = 0;
                WSARecv(tdefer[i].fd, &wb, 1, &dm, &fl,
                        (LPOVERLAPPED)&((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[tdefer[i].idx], NULL);
            }
            continue;   // 空闲退避
        }
        if (!p_ov) continue;   // 空 OVERLAPPED：跳过（仅 GQCS 自身失败/超时）
        pr_iocp_ctx_t* ctx = (pr_iocp_ctx_t*)p_ov;
        pr_defer_t defer_ops[PR_DEFER_CLOSE_MAX];   // 延迟操作（锁外执行）
        int    ndefer = 0;
        int    post_k = -1;      // 第191轮：accept 槽补投（锁外执行）
        // P6: 完成事件处理段持锁串行化；GQCS 取包本身并发、处理收敛
        // 注意：失败完成事件（ok == FALSE）仍要交给处理段 —— 异步上游连接失败
        // 必须走 502 收尾，不能静默丢弃导致槽位泄漏。
        EnterCriticalSection(&s->pr_loop_cs);
        pr_iocp_handle_completion(s, listen_sock, compkey, bytes_trans, ctx, (int)ok,
                                  defer_ops, &ndefer, &post_k);
        LeaveCriticalSection(&s->pr_loop_cs);
        // 第191轮：锁外执行延迟操作 —— closesocket 是 Windows 重系统调用（TCP 终止
        // 序列 + WFP 钩子）、send 阻塞发送，锁内串行执行会把多消费线程的并行度
        // 打回单线程。plan 发送用阻塞 fd（本地响应路径已切阻塞），循环直到发完。
        // accept 槽补投（WSASocket/AcceptEx ~300us）同样锁外执行。
        if (post_k >= 0) pr_post_accept_slot(s, listen_sock, post_k);
        for (int i = 0; i < ndefer; i++) {
            pr_defer_t* d = &defer_ops[i];
            if (d->kind == PR_DEFER_RECV) {
                // 首包 WSARecv post（ctx/buf 由锁内初始化；slot 在 post 前不会复用）
                WSABUF wb;
                wb.buf = s->pr_hbuf[d->idx];
                wb.len = 8192;
                DWORD fl = 0, dm = 0;
                WSARecv(d->fd, &wb, 1, &dm, &fl,
                        (LPOVERLAPPED)&((pr_iocp_ctx_t*)s->pr_iocp_client_ctx)[d->idx], NULL);
                continue;
            }
            if (d->has_plan) {
                const char* parts[2];
                int lens[2];
                parts[0] = d->plan.head; lens[0] = (int)d->plan.head_len;
                parts[1] = d->plan.body; lens[1] = (int)d->plan.body_len;
                for (int p = 0; p < 2; p++) {
                    int sent = 0;
                    while (parts[p] && sent < lens[p]) {
                        int w = send(d->fd, parts[p] + sent, lens[p] - sent, 0);
                        if (w <= 0) break;
                        sent += w;
                    }
                }
            }
            closesocket(d->fd);
        }
    }
}

// P6: 额外 IOCP 消费线程入口（与主线程跑同一完成循环）
static DWORD WINAPI nl_web_iocp_consumer_thread(LPVOID arg) {
    nl_web_server_t* s = (nl_web_server_t*)arg;
    // 建线程时缓存 listen socket（stop 会把 s->sock 置 INVALID，仅影响之后的 accept 预投递）
    SOCKET ls = s->sock;
    pr_iocp_consume_loop(s, ls);
    return 0;
}

// 引擎线程入口
static DWORD WINAPI nl_web_proxy_engine_thread(LPVOID arg) {
    nl_web_server_t* s = (nl_web_server_t*)arg;
    nl_web_proxy_engine(s, s->sock);
    return 0;
}

// 上游 upstream 字符串 -> host:port（统一解析，供同步/异步连接复用）
// 支持 "[::1]:8080" 形式；缺省端口按协议补 80/443。
static void nl_proxy_resolve_upstream(const nl_web_route_t* route, char* host,
                                      size_t host_size, int* out_port) {
    int def_port = (route->protocol == NL_PROXY_HTTPS) ? 443 : 80;
    int port = def_port;
    if (!nl_px_split_hostport(route->upstream, host, host_size, &port, def_port))
        snprintf(host, host_size, "%s", route->upstream);
    if (port <= 0) port = def_port;
    *out_port = port;
}

// host:port -> 已解析地址（AF_UNSPEC：IPv4/IPv6 字面量与域名）。
// 返回 AF_INET / AF_INET6，失败返回 0（调用方回 502，不回退明文/回环）。
static int nl_proxy_fill_addr(const char* host, int port, struct sockaddr_storage* ss) {
    memset(ss, 0, sizeof(*ss));
    if (!host || !host[0] || port <= 0) return 0;
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) return 0;
    int af = res->ai_family;
    if (res->ai_addrlen > 0 && res->ai_addrlen <= sizeof(*ss))
        memcpy(ss, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    return (af == AF_INET || af == AF_INET6) ? af : 0;
}

/* ============================================================
 * HTTPS 上游：整条连接移交 TLS 隧道线程。
 *
 * 握手 + 双向收发全部在线程内完成（不占用 IOCP 消费线程），收发一律走
 * https 扩展的会话对象；契约不满足或握手失败时明确回 502，绝不降级明文。
 * ============================================================ */
typedef struct pr_tls_tunnel_arg {
    nl_web_server_t* s;
    int    idx;
    SOCKET cfd;
    SOCKET ufd;
    DWORD  conn_id;
    int    init_len;
    char*  init;
    char   host[512];
} pr_tls_tunnel_arg_t;

static DWORD WINAPI pr_tls_tunnel_thread(LPVOID param) {
    pr_tls_tunnel_arg_t* a = (pr_tls_tunnel_arg_t*)param;
    nl_web_server_t* s = a->s;
    int idx = a->idx;
    SOCKET cfd = a->cfd, ufd = a->ufd;
    DWORD conn_id = a->conn_id;
    int init_len = a->init_len;
    char* init = a->init;
    char host[512];
    snprintf(host, sizeof(host), "%s", a->host);
    free(a);

    nl_px_tls_api api;
    void* tls_conn = NULL;
    const char* alpn = NULL;
    if (nl_px_tls_contract_get(&api) && api.conn_wrap && api.conn_read &&
        api.conn_write && api.conn_free) {
        tls_conn = api.conn_wrap((int)ufd, host, &alpn);
    }
    if (!tls_conn) {
        nl_log(NL_LOG_ERROR,
                    "proxy: TLS session unavailable for upstream %s (refusing plaintext fallback)", host);
        static const char err502[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(cfd, err502, (int)(sizeof(err502) - 1), 0);
        closesocket(cfd);
        closesocket(ufd);
    } else if (alpn && strcmp(alpn, "h2") == 0) {
        /* 上游协商出 h2：改用 H2 帧交换（函数内部负责关闭双方 fd 与释放 TLS 会话） */
        (void)nl_px_h2_upstream_run((intptr_t)cfd, (intptr_t)ufd, tls_conn, &api,
                                    init, init_len, 60000);
    } else {
        (void)nl_px_tls_tunnel_run((intptr_t)cfd, (intptr_t)ufd, tls_conn, &api,
                                   init, init_len, 60000);
    }
    free(init);

    /* 隧道结束：在引擎锁内按 conn_id 回收槽位（迟到回收不会误伤复用后的槽） */
    EnterCriticalSection(&s->pr_loop_cs);
    if (idx >= 0 && idx < s->pr_cap && s->pr_conn_id && s->pr_conn_id[idx] == conn_id)
        pr_free_slot(s, idx);
    LeaveCriticalSection(&s->pr_loop_cs);
    InterlockedDecrement(&s->pr_tls_threads);
    return 0;
}

/* 移交失败（契约缺失/线程创建失败）统一回 502 并回收槽位；返回 0。 */
static int pr_tls_start_tunnel(nl_web_server_t* s, int idx, SOCKET cfd, SOCKET ufd,
                               const char* host, const char* init, int init_len) {
    int bad_slot = (!s || idx < 0 || idx >= s->pr_cap) ? 1 : 0;
    if (bad_slot || !init || init_len <= 0) {
        static const char err502[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        if (cfd != INVALID_SOCKET) { (void)send(cfd, err502, (int)(sizeof(err502) - 1), 0); closesocket(cfd); }
        if (ufd != INVALID_SOCKET) closesocket(ufd);
        if (!bad_slot) {
            s->pr_fd_client[idx] = INVALID_SOCKET;
            s->pr_fd_upstream[idx] = 0;
            pr_free_slot(s, idx);
        }
        return 0;
    }

    pr_tls_tunnel_arg_t* a = (pr_tls_tunnel_arg_t*)calloc(1, sizeof(*a));
    if (a) {
        a->init = (char*)malloc((size_t)init_len);
        if (!a->init) { free(a); a = NULL; }
    }
    if (!a) {
        static const char err502[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(cfd, err502, (int)(sizeof(err502) - 1), 0);
        closesocket(cfd);
        closesocket(ufd);
        s->pr_fd_client[idx] = INVALID_SOCKET;
        s->pr_fd_upstream[idx] = 0;
        pr_free_slot(s, idx);
        return 0;
    }

    memcpy(a->init, init, (size_t)init_len);
    a->init_len = init_len;
    a->s = s;
    a->idx = idx;
    a->cfd = cfd;
    a->ufd = ufd;
    a->conn_id = s->pr_conn_id[idx];
    snprintf(a->host, sizeof(a->host), "%s", host ? host : "");

    /* fd 所有权交给线程；槽位仍记录 fd 以便拆卸时 shutdown 唤醒，但标记
     * PR_ST_TLS 后引擎不会对它投递/关闭任何 I/O（fd 由隧道线程最终关闭）。 */
    s->pr_state[idx] = PR_ST_ACTIVE | PR_ST_TLS;

    InterlockedIncrement(&s->pr_tls_threads);
    HANDLE th = CreateThread(NULL, 0, pr_tls_tunnel_thread, a, 0, NULL);
    if (!th) {
        InterlockedDecrement(&s->pr_tls_threads);
        free(a->init);
        free(a);
        static const char err502[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(cfd, err502, (int)(sizeof(err502) - 1), 0);
        closesocket(cfd);
        closesocket(ufd);
        pr_free_slot(s, idx);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

/* ==================================================================
 * HTTP/2 明文（h2c）桥接：把客户端连接移交公共层 nl_px_h2（平台无关）
 *
 * 移交后槽位复用 PR_ST_TLS 语义（自管 I/O）：引擎不再为该槽投递读写，
 * 拆卸时既有逻辑会 shutdown 客户端 fd 唤醒桥接线程；线程退出后按
 * conn_id 回收槽位，再递减线程计数（顺序与 TLS 隧道一致）。
 * ================================================================== */
typedef struct pr_h2_arg {
    nl_web_server_t* s;
    int idx;
    SOCKET cfd;
    DWORD conn_id;
    int port;
    int is_upgrade;
    int init_len;
    char* init;
} pr_h2_arg_t;

static DWORD WINAPI pr_h2_thread(LPVOID param) {
    pr_h2_arg_t* a = (pr_h2_arg_t*)param;
    nl_web_server_t* s = a->s;
    int idx = a->idx;
    DWORD conn_id = a->conn_id;
    (void)nl_px_h2_bridge_run((intptr_t)a->cfd, a->port, a->init,
                              (size_t)a->init_len, a->is_upgrade, 60000,
                              s, &g_pr_local_ops);
    free(a->init);
    free(a);

    EnterCriticalSection(&s->pr_loop_cs);
    if (idx >= 0 && idx < s->pr_cap && s->pr_conn_id && s->pr_conn_id[idx] == conn_id)
        pr_free_slot(s, idx);
    LeaveCriticalSection(&s->pr_loop_cs);
    InterlockedDecrement(&s->pr_tls_threads);
    return 0;
}

static int pr_h2_start_bridge(nl_web_server_t* s, int idx, SOCKET cfd,
                              const char* init, int init_len, int is_upgrade) {
    pr_h2_arg_t* a = (pr_h2_arg_t*)calloc(1, sizeof(*a));
    if (a) {
        a->init = (char*)malloc((size_t)init_len);
        if (!a->init) { free(a); a = NULL; }
    }
    if (!a) {
        static const char err502[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(cfd, err502, (int)(sizeof(err502) - 1), 0);
        closesocket(cfd);
        s->pr_fd_client[idx] = INVALID_SOCKET;
        pr_free_slot(s, idx);
        return 0;
    }
    memcpy(a->init, init, (size_t)init_len);
    a->init_len = init_len;
    a->s = s;
    a->idx = idx;
    a->cfd = cfd;
    a->conn_id = s->pr_conn_id[idx];
    a->port = s->port;
    a->is_upgrade = is_upgrade;

    s->pr_state[idx] = PR_ST_ACTIVE | PR_ST_TLS;
    InterlockedIncrement(&s->pr_tls_threads);
    HANDLE th = CreateThread(NULL, 0, pr_h2_thread, a, 0, NULL);
    if (!th) {
        InterlockedDecrement(&s->pr_tls_threads);
        free(a->init);
        free(a);
        static const char err502[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)send(cfd, err502, (int)(sizeof(err502) - 1), 0);
        closesocket(cfd);
        s->pr_fd_client[idx] = INVALID_SOCKET;
        pr_free_slot(s, idx);
        return 0;
    }
    CloseHandle(th);
    return 1;
}

// ---- ConnectEx 扩展动态获取（InitOnce 保证多引擎线程安全）----
static INIT_ONCE g_connectex_once = INIT_ONCE_STATIC_INIT;
static LPFN_CONNECTEX g_connectex_fn = NULL;
static BOOL CALLBACK nl_connectex_init(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    SOCKET tmp = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (tmp == INVALID_SOCKET) return TRUE;
    GUID guid = WSAID_CONNECTEX;
    DWORD bytes = 0;
    WSAIoctl(tmp, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid),
             &g_connectex_fn, sizeof(g_connectex_fn), &bytes, NULL, NULL);
    closesocket(tmp);
    return TRUE;
}
static LPFN_CONNECTEX nl_get_connectex(void) {
    InitOnceExecuteOnce(&g_connectex_once, nl_connectex_init, NULL, NULL);
    return g_connectex_fn;
}

/**
 * 异步发起上游连接（ConnectEx）。成功投递返回 1，等待 IOCP_EV_UPSTREAM_CONNECT；
 * 返回 0 表示扩展不可用/创建失败，调用方应回退同步 connect。
 * 前置条件：调用方已把上游请求构造进 pr_hbuf_out[idx]，长度写入 pr_up_req_len[idx]。
 */
static int nl_proxy_start_connect_async(nl_web_server_t* s, int idx, const nl_web_route_t* route) {
    LPFN_CONNECTEX pfn = nl_get_connectex();
    if (!pfn || !s->iocp) return 0;

    char host[512];
    int port = 0;
    nl_proxy_resolve_upstream(route, host, sizeof(host), &port);

    struct sockaddr_storage up_addr;
    int up_af = nl_proxy_fill_addr(host, port, &up_addr);
    if (up_af == 0) return 0;

    SOCKET ufd = WSASocket(up_af, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (ufd == INVALID_SOCKET) return 0;

    // ConnectEx 要求 socket 已绑定本地地址（按上游地址族选通配地址）
    struct sockaddr_storage local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    int local_len;
    if (up_af == AF_INET6) {
        struct sockaddr_in6* l6 = (struct sockaddr_in6*)&local_addr;
        l6->sin6_family = AF_INET6;
        l6->sin6_addr = in6addr_any;
        l6->sin6_port = 0;
        local_len = (int)sizeof(*l6);
    } else {
        struct sockaddr_in* l4 = (struct sockaddr_in*)&local_addr;
        l4->sin_family = AF_INET;
        l4->sin_addr.s_addr = INADDR_ANY;
        l4->sin_port = 0;
        local_len = (int)sizeof(*l4);
    }
    if (bind(ufd, (struct sockaddr*)&local_addr, local_len) == SOCKET_ERROR) {
        closesocket(ufd);
        return 0;
    }
    // 必须在投递 ConnectEx 之前把 socket 关联到 IOCP（completion key = idx），
    // 否则连接完成的 OVERLAPPED 不会作为完成包出现在引擎消费循环里。
    if (!CreateIoCompletionPort((HANDLE)ufd, s->iocp, (ULONG_PTR)idx, 0)) {
        closesocket(ufd);
        return 0;
    }

    pr_iocp_ctx_t* uctx = &((pr_iocp_ctx_t*)s->pr_iocp_up_ctx)[idx];
    memset(uctx, 0, sizeof(*uctx));
    uctx->idx = idx;
    uctx->conn_id = s->pr_conn_id[idx];
    uctx->ev  = IOCP_EV_UPSTREAM_CONNECT;
    uctx->sock = ufd;

    BOOL ok = pfn(ufd, (struct sockaddr*)&up_addr, (int)sizeof(up_addr),
                  NULL, 0, NULL, &uctx->ov);
    if (!ok && WSAGetLastError() != WSA_IO_PENDING) {
        closesocket(ufd);
        return 0;
    }
    // 占位：连接完成前该 fd 不参与读/写
    s->pr_fd_upstream[idx] = ufd;
    return 1;
}

// 同步连接上游（回退路径；扩展不可用或 ConnectEx 投递失败时使用）
static SOCKET nl_proxy_connect_upstream(const nl_web_route_t* route) {
    char host[512];
    int port = 0;
    nl_proxy_resolve_upstream(route, host, sizeof(host), &port);

    struct sockaddr_storage up_addr;
    int up_af = nl_proxy_fill_addr(host, port, &up_addr);
    if (up_af == 0) return INVALID_SOCKET;

    SOCKET up_sock = socket(up_af, SOCK_STREAM, 0);
    if (up_sock == INVALID_SOCKET) return INVALID_SOCKET;

    DWORD tv_ms = 5000;
    setsockopt(up_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv_ms, sizeof(tv_ms));
    setsockopt(up_sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv_ms, sizeof(tv_ms));

    int up_len = (up_af == AF_INET6) ? (int)sizeof(struct sockaddr_in6)
                                     : (int)sizeof(struct sockaddr_in);
    if (connect(up_sock, (struct sockaddr*)&up_addr, up_len) == SOCKET_ERROR) {
        closesocket(up_sock);
        return INVALID_SOCKET;
    }

    // HTTPS 的 TLS 握手由隧道线程内的会话对象完成（此处只建 TCP 连接）
    return up_sock;
}

#ifdef NETLEAF_LEGACY_WORKER_POOL
// 旧版阻塞式单连接处理（仅 legacy worker 池/单线程回退使用，默认构建不编译）
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
        /* 首行：METHOD SP PATH SP VERSION（公共层统一解析，不修改 buffer 字节） */
        if (nl_px_parse_request_line(buffer, (size_t)(line_end - buffer),
                                     method, sizeof(method),
                                     path, sizeof(path),
                                     protocol, sizeof(protocol)) != 0) {
            send_http_error(client, 400, "Bad Request");
            closesocket(client);
            return;
        }
    }

    /* 第192轮：RFC 3986 端点规范化（剥 query/fragment、percent-decode、
     * dot-segment 归一）。路由查找与本地响应用 norm_path；代理转发
     * （反代请求行 / TCP 透传）保留原始 path（含 query，字节原样）。
     * 第203轮：normalize 失败不再立即拒绝 —— 可能是目录端点的越根 ".."
     * （clamp 语义），先按字面查表；命中非 DIR 路由才按原规则 403/400。 */
    char norm_path[4096];
    int norm_ok = 1;
    int norm_uri_rc = NL_URI_OK;
    if (strcmp(path, "*") == 0) {
        norm_path[0] = '*';        /* OPTIONS *（RFC 7230 §7.1）：无路由，维持 404 */
        norm_path[1] = '\0';
    } else {
        norm_uri_rc = nl_uri_normalize(path, norm_path, sizeof(norm_path));
        if (norm_uri_rc != NL_URI_OK) {
            norm_ok = 0;
            /* 用原始 path 继续查表（DIR 字面前缀可命中；normalize 失败的
             * nl_px_path_match* 返回 0，不会误中其它路由） */
            snprintf(norm_path, sizeof(norm_path), "%s", path);
        }
    }

    EnterCriticalSection(&server->mutex);
    // 使用 hash 表 O(1) 查找路由；若 hash 未初始化则回退到线性链表
    nl_web_route_t* route = nl_route_hash_lookup_simple(server, norm_path);
    if (!route) {
        // 第195轮：先精确 strcmp（hash 同款语义），未中再按最长前缀挑目录路由
        // ——"精确 > 最长前缀"与引擎路径 pr_lookup_route_idx 一致。
        // 第203轮：DIR 路由单独按字面前缀（带边界）匹配，优先于其它前缀；
        // 其规范化前缀匹配由 nl_px_path_match_prefix 内部 normalize 失败返回 0 排除。
        // DIR 字面前缀必须试"原始 path 剥 query"—— norm 会吃掉端点前缀
        // （"/web/../p" → "/p"），仅用 norm_path 匹配将漏掉 DIR 端点。
        nl_web_route_t* best = NULL;
        size_t best_len = 0;
        nl_web_route_t* dir_best = NULL;
        size_t dir_len = 0;
        char lit[4096];
        size_t ln = 0;
        while (path[ln] && path[ln] != '?' && path[ln] != '#') ln++;
        if (ln >= sizeof(lit)) ln = sizeof(lit) - 1;
        memcpy(lit, path, ln);
        lit[ln] = '\0';
        for (nl_web_route_t* r = server->routes; r; r = r->next) {
            if (strcmp(r->path, norm_path) == 0) { route = r; break; }
            if (r->type == NL_ROUTE_TYPE_DIR) {
                size_t plen = strlen(r->path);
                if (ln >= plen && memcmp(r->path, lit, plen) == 0 &&
                    (lit[plen] == '\0' || lit[plen] == '/') &&
                    (!dir_best || plen > dir_len)) {
                    dir_best = r; dir_len = plen;
                }
                continue;
            }
            if (r->is_prefix && nl_px_path_match_prefix(r->path, norm_path)) {
                size_t plen = strlen(r->path);
                if (!best || plen > best_len) { best = r; best_len = plen; }
            }
        }
        if (!route) route = dir_best ? dir_best : best;
    }
    if (!norm_ok && (!route || route->type != NL_ROUTE_TYPE_DIR)) {
        /* 非法端点只有目录端点可 clamp 放行；未命中也按 400（与引擎 plan_build 同规则） */
        int esc = (norm_uri_rc == NL_URI_ESCAPES);
        LeaveCriticalSection(&server->mutex);
        send_http_error(client, esc ? 403 : 400,
                        esc ? "Forbidden" : "Bad Request");
        closesocket(client);
        return;
    }
    if (route) {
        if (route->type == NL_ROUTE_TYPE_REDIRECT) {
                // Send redirect (301 or 302 based on server setting)
                int redirect_code = server->redirect_type;
                LeaveCriticalSection(&server->mutex);
                char redirect_response[1024];
                int rn = nl_px_build_redirect_head(redirect_response, sizeof(redirect_response),
                                                   redirect_code, route->redirect_url, 0);
                if (rn > 0) (void)send(client, redirect_response, rn, 0);
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
            else if (route->type == NL_ROUTE_TYPE_DIR) {
                // 第203轮：目录/文件端点 —— 复用公共层 plan（端点即根、../ clamp、
                // MIME 猜测、错误码映射均与引擎路径同一份实现）；
                // keepalive 传 0：legacy 每请求一连接，响应完即关（头与行为一致）
                LeaveCriticalSection(&server->mutex);
                int is_head = (strcmp(method, "HEAD") == 0);
                nl_px_local_plan plan;
                if (nl_px_local_plan_build(server, &g_pr_local_ops, path,
                                           is_head, 0, &plan) != 1 || plan.head_len == 0) {
                    send_http_error(client, 404, "Not Found");
                    closesocket(client);
                    return;
                }
                if (send(client, plan.head, (int)plan.head_len, 0) <= 0) {
                    closesocket(client);
                    return;
                }
                if (plan.status == 200 && plan.type == NL_PX_LOCAL_FILE &&
                    !is_head && plan.file_size > 0) {
                    FILE* fp = fopen(plan.file_path, "rb");
                    if (!fp) { closesocket(client); return; }
                    char chunk[65536];
                    long long remain = plan.file_size;
                    while (remain > 0) {
                        int want = (remain > (long long)sizeof(chunk))
                                   ? (int)sizeof(chunk) : (int)remain;
                        int got = (int)fread(chunk, 1, (size_t)want, fp);
                        if (got <= 0) break;
                        if (send(client, chunk, got, 0) <= 0) break;
                        remain -= got;
                    }
                    fclose(fp);
                }
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
                    while ((n = recv(up_sock, proxy_buf, 65536, 0)) > 0) {
                        if (send(client, proxy_buf, n, 0) <= 0) break;
                    }
                    free(proxy_buf);
                    closesocket(up_sock);
                    closesocket(client);
                    return;
                }

                // HTTP/HTTPS：重组 request line + 重写 Host/XFF/Connection + 透传
                // 客户端原始头与 body（原实现硬编码 XFF 127.0.0.1，且 rest 按 C 串
                // 扫描，body 含 \0 即被截断）
                char upstream_req_full[12288];
                char peer_ip[64];
                peer_ip[0] = '\0';
                {
                    struct sockaddr_storage pa;
                    int pa_len = (int)sizeof(pa);
                    memset(&pa, 0, sizeof(pa));
                    if (getpeername(client, (struct sockaddr*)&pa, &pa_len) == 0)
                        (void)nl_px_sockaddr_ip((const struct sockaddr*)&pa, peer_ip, sizeof(peer_ip));
                }
                /* 第195轮：目录转发改写（nginx proxy_pass URI 拼接）；
                 * up_uri 为空保持原样透传，与引擎路径同一份公共逻辑。 */
                char rpath[4096];
                const char* fwd_path = path;
                if (route->up_uri[0]) {
                    if (nl_px_dir_rewrite(route->path, route->is_prefix,
                                          route->up_uri, path,
                                          rpath, sizeof(rpath)) > 0)
                        fwd_path = rpath;
                }
                int total = snprintf(upstream_req_full, sizeof(upstream_req_full),
                    "%s %s %s\r\nHost: %s\r\n", method, fwd_path, protocol, route->upstream);
                if (total < 0 || (size_t)total >= sizeof(upstream_req_full)) total = 0;
                if (peer_ip[0]) {
                    int x = snprintf(upstream_req_full + total, sizeof(upstream_req_full) - total,
                                     "X-Forwarded-For: %s\r\n", peer_ip);
                    if (x > 0 && (size_t)x < sizeof(upstream_req_full) - total) total += x;
                }
                {
                    int x = snprintf(upstream_req_full + total, sizeof(upstream_req_full) - total,
                                     "Connection: close\r\n");
                    if (x > 0 && (size_t)x < sizeof(upstream_req_full) - total) total += x;
                }
                /* buffer 首行已被改写：memchr 重新定位。客户端头区 = [首行后, 头终止符)，
                 * body = [终止符后, received)。全程按字节界处理，避免 C 串扫描截断 body。 */
                const char* line_term = memchr(buffer, '\n', (size_t)received);
                if (line_term) {
                    const char* hdrs = line_term + 1;
                    const char* hend = NULL;
                    for (int i = 0; hdrs - buffer + i + 3 < received; i++) {
                        if (hdrs[i] == '\r' && hdrs[i + 1] == '\n' &&
                            hdrs[i + 2] == '\r' && hdrs[i + 3] == '\n') {
                            hend = hdrs + i;
                            break;
                        }
                    }
                    if (hend) {
                        // 逐行透传客户端头，过滤上方已改写的 Host/XFF/Connection（防重复头）
                        const char* p = hdrs;
                        while (p < hend && total < (int)sizeof(upstream_req_full)) {
                            const char* nl = memchr(p, '\n', (size_t)(hend - p));
                            if (!nl) break;
                            const char* le = nl;
                            if (le > p && le[-1] == '\r') le--;
                            size_t llen = (size_t)(le - p);
                            size_t k = 0;
                            while (k < llen && p[k] != ':') k++;
                            int skip = 0;
                            if (k > 0 && k < llen) {
                                if ((k == 4 && nl_px_ci_starts(p, llen, "host")) ||
                                    (k == 16 && nl_px_ci_starts(p, llen, "x-forwarded-for")) ||
                                    (k == 10 && nl_px_ci_starts(p, llen, "connection")))
                                    skip = 1;
                            }
                            if (!skip && llen > 0 &&
                                (size_t)total + llen + 2 < sizeof(upstream_req_full)) {
                                memcpy(upstream_req_full + total, p, llen);
                                total += (int)llen;
                                upstream_req_full[total++] = '\r';
                                upstream_req_full[total++] = '\n';
                            }
                            p = nl + 1;
                        }
                        if ((size_t)total + 2 < sizeof(upstream_req_full)) {
                            upstream_req_full[total++] = '\r';
                            upstream_req_full[total++] = '\n';
                        }
                        const char* body = hend + 4;
                        size_t body_n = (size_t)(buffer + received - body);
                        if (body_n > 0 && (size_t)total + body_n <= sizeof(upstream_req_full)) {
                            memcpy(upstream_req_full + total, body, body_n);
                            total += (int)body_n;
                        }
                    } else {
                        // 单包内头未收齐：按字节透传首行之后的剩余数据
                        size_t rest_n = (size_t)(buffer + received - hdrs);
                        if ((size_t)total + rest_n <= sizeof(upstream_req_full)) {
                            memcpy(upstream_req_full + total, hdrs, rest_n);
                            total += (int)rest_n;
                        }
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
                while ((n = recv(up_sock, proxy_buf, 65536, 0)) > 0) {
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
                // P12: HEAD 只发头不发体（RFC 7231），Content-Length 与 GET 一致
                if (strcmp(method, "HEAD") == 0) {
                    char hdr[512];
                    int hdr_len = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        route->content_type, route->content_size);
                    send(client, hdr, hdr_len, 0);
                } else {
                    send_http_response(client, route->content_type, route->content, route->content_size);
                }
                closesocket(client);
                return;
            }
    }
    LeaveCriticalSection(&server->mutex);
    
    send_http_error(client, 404, "Not Found");
    closesocket(client);
}
#endif /* NETLEAF_LEGACY_WORKER_POOL */

#ifdef NETLEAF_LEGACY_WORKER_POOL
// worker 线程回调适配（模块不依赖 nl_web_server_t 结构体）
static void nl_web_client_dispatch(void* owner, SOCKET client) {
    nl_handle_client((nl_web_server_t*)owner, client);
}

static DWORD WINAPI web_server_thread(LPVOID arg) {
    nl_web_server_t* server = (nl_web_server_t*)arg;

    // 可选模块：仅当显式启用 legacy worker 池且配置了 worker_count 时启动线程池
    if (server->worker_count > 0 && server->workers) {
        /* 第189轮：listen 设非阻塞，防多 worker 竞争 accept 时输家永久挂起（同 Linux）。
         * worker 内 WSAPoll 等可读 + WSAEWOULDBLOCK 退避；accept 出的新 socket 恢复阻塞。 */
        u_long nb = 1;
        ioctlsocket(server->sock, FIONBIO, &nb);
        nl_worker_pool_ctx_t ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.sock          = server->sock;
        ctx.running       = &server->running;
        ctx.handles       = server->workers;
        ctx.capacity      = server->worker_capacity;
        ctx.handle_client = nl_web_client_dispatch;
        ctx.owner         = server;

        int started = nl_worker_pool_start(&ctx, server->worker_count);
        if (started > 0) {
            server->worker_active = 1;
            nl_worker_pool_join(&ctx, started);
            server->worker_active = 0;
            server->worker_count  = 0;
            return 0;
        }
    }

    // 回退：worker 未启动（启动失败或 worker_count=0），accept 线程自己承担主 accept 循环
    // 第189轮：先 WSAPoll(200ms) 再 accept，避免空闲时阻塞 accept 卡死 stop（同 worker 池修复）
    while (server->running) {
        WSAPOLLFD pfd;
        pfd.fd = server->sock;
        pfd.events = POLLRDNORM;
        pfd.revents = 0;
        int prc = WSAPoll(&pfd, 1, 200);
        if (prc <= 0) {
            if (!server->running) break;
            Sleep(10);
            continue;
        }
        SOCKET client = accept(server->sock, NULL, NULL);
        if (client == INVALID_SOCKET) {
            Sleep(10);
            continue;
        }
        nl_handle_client(server, client);
    }

    return 0;
}
#endif /* NETLEAF_LEGACY_WORKER_POOL */

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
    if (server) server->pr_pending_client = INVALID_SOCKET;   // 第190轮：非零初值（SOCKET）
    if (!server) {
        LeaveCriticalSection(&g_web_servers_mutex);
        return NULL;
    }
    
    server->port = port;
    server->routes = NULL;
    server->listen_mode = -1;   // 未显式设置：启动时读 NETLEAF_LISTEN_MODE（默认双栈）
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
    if (server->pr_accept_ctxs || server->pr_accept_addrs) {
        // 第191轮：destroy 时引擎可能已在 cleanup 关过一轮 —— 仅释放残留（NULL 安全）
        for (int k = 0; k < PR_ACCEPT_PENDING; k++) {
            if (server->pr_accept_socks[k] != INVALID_SOCKET) {
                closesocket(server->pr_accept_socks[k]);
                server->pr_accept_socks[k] = INVALID_SOCKET;
            }
        }
        free(server->pr_accept_ctxs);  server->pr_accept_ctxs  = NULL;
        free(server->pr_accept_addrs); server->pr_accept_addrs = NULL;
    }
    // 第190轮：关闭 ring 满背压暂存的已 accept 连接
    if (server->pr_pending_client != INVALID_SOCKET) {
        closesocket(server->pr_pending_client);
        server->pr_pending_client = INVALID_SOCKET;
    }
    free(server->pr_accept_addr); server->pr_accept_addr = NULL;
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
    free(server->pr_up);          server->pr_up          = NULL;
    free(server->pr_up_key);      server->pr_up_key      = NULL;
    free(server->pr_up_pend);     server->pr_up_pend     = NULL;
    free(server->pr_up_pend_len); server->pr_up_pend_len = NULL;
    free(server->pr_up_req_len);  server->pr_up_req_len  = NULL;
    free(server->pr_last_active_ms); server->pr_last_active_ms = NULL;  // P7 兜底（引擎正常退出时已 free+置 NULL）
    free(server->pr_conn_id);        server->pr_conn_id        = NULL;  // slot 复用竞态防护 兜底
    if (server->up_pool) { nl_px_pool_destroy(server->up_pool); free(server->up_pool); server->up_pool = NULL; }
    free(server);
}

int nl_web_start(nl_web_server_t* server) {
    if (!server || server->running) return NL_EINVAL;
    
    if (init_winsock() != 0) return NL_ERROR;
    
    // 监听地址族由 nl_px_listen_mode_resolve 决定（显式 API > 环境变量 > 双栈）：
    //   DUAL = AF_INET6 + IPV6_V6ONLY=0，IPv4 仍可访问；失败自动回退 IPv4
    //   V4   = AF_INET
    //   V6   = AF_INET6 + IPV6_V6ONLY=1（不回退）
    server->af = AF_INET;
    server->sock = INVALID_SOCKET;
    {
        int mode = nl_px_listen_mode_resolve(server->listen_mode);
        int try6 = (mode == NL_PX_LISTEN_DUAL || mode == NL_PX_LISTEN_V6);
        if (try6) {
            SOCKET s6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
            if (s6 != INVALID_SOCKET) {
                int opt6 = 1;
                int v6only = (mode == NL_PX_LISTEN_V6) ? 1 : 0;
                setsockopt(s6, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt6, sizeof(opt6));
                setsockopt(s6, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
                struct sockaddr_in6 a6;
                memset(&a6, 0, sizeof(a6));
                a6.sin6_family = AF_INET6;
                a6.sin6_addr = in6addr_any;
                a6.sin6_port = htons((u_short)server->port);
                if (bind(s6, (struct sockaddr*)&a6, sizeof(a6)) == 0 &&
                    listen(s6, SOMAXCONN) != SOCKET_ERROR) {
                    server->sock = s6;
                    server->af = AF_INET6;
                } else {
                    closesocket(s6);
                }
            }
        }
    }
    if (server->sock == INVALID_SOCKET) {
        if (nl_px_listen_mode_resolve(server->listen_mode) == NL_PX_LISTEN_V6)
            return NL_ERROR;   // 显式要求 V6 时不静默回退
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
    }
    
    server->running = 1;
    // 数据驱动检测：是否存在反向代理路由。
#ifdef NETLEAF_LEGACY_WORKER_POOL
    // 旧版阻塞 worker 池（可选模块）：仅代理路由走 IOCP 引擎，其余走 worker 池；
    // force_legacy_pool=1 时强制整站走 worker 池（含代理）。
    int has_proxy = 0;
    EnterCriticalSection(&server->mutex);
    for (nl_web_route_t* r = server->routes; r; r = r->next)
        if (r->type == NL_ROUTE_TYPE_PROXY) { has_proxy = 1; break; }
    LeaveCriticalSection(&server->mutex);
    if (server->force_legacy_pool) has_proxy = 0;
    if (has_proxy) {
        server->thread = CreateThread(NULL, 0, nl_web_proxy_engine_thread, server, 0, NULL);
        if (!server->thread) {
            server->running = 0;
            closesocket(server->sock);
            return NL_ERROR;
        }
        nl_log(NL_LOG_INFO, "Web server (proxy engine) started on port %d", server->port);
        return NL_OK;
    }
    // worker 池容量按需分配。未显式设置时用 WEB_WORKER_DEFAULT（上限 64）。
    // worker_count 为 0 时由 web_server_thread 回退到 accept 线程自处理连接。
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

    nl_log(NL_LOG_INFO, "Web server started on port %d", server->port);
    return NL_OK;
#else
    // 默认构建：不含 legacy worker 池，本地路由 + 代理路由统一由 IOCP 引擎处理
    // （引擎内置 PR_ROLE_LOCAL，见 pr_iocp_handle_completion 的 CONTENT/FILE/REDIRECT 分支）。
    server->thread = CreateThread(NULL, 0, nl_web_proxy_engine_thread, server, 0, NULL);
    if (!server->thread) {
        server->running = 0;
        closesocket(server->sock);
        return NL_ERROR;
    }
    nl_log(NL_LOG_INFO, "Web server (IOCP engine) started on port %d", server->port);
    return NL_OK;
#endif
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

// P6: 引擎线程数。Windows 上=IOCP 消费线程数（主线程+额外消费者，共 n 个跑同一完成循环）；
// 完成处理经 pr_loop_cs 临界区串行化，多线程主要提升 GQCS 取包并发。
int nl_web_set_engine_threads(nl_web_server_t* server, int n) {
    if (!server) return NL_ERROR;
    if (n < 0) n = 0;
    if (n > 128) n = 128;
    server->engine_threads_cfg = n;
    return NL_OK;
}

// P6: 总连接预算。Windows IOCP 引擎暂不支持，仅记字段不生效。
int nl_web_set_max_conns(nl_web_server_t* server, int total) {
    if (!server) return NL_ERROR;
    if (total < 0) total = 0;
    server->max_conns_cfg = total;
    return NL_OK;
}

// P7: 空闲超时。Windows IOCP 引擎生效：消费线程 ~500ms 节流扫描，超过 idle_ms
// 未活跃的连接强关回收（<=0 视为默认 30000ms）。
int nl_web_set_idle_timeout(nl_web_server_t* server, int ms) {
    if (!server) return NL_ERROR;
    if (ms < 0) ms = 0;
    server->idle_timeout_ms = ms;
    return NL_OK;
}

// P5: 强制 legacy worker 池。
int nl_web_force_legacy_pool(nl_web_server_t* server, int enable) {
    if (!server) return NL_ERROR;
    server->force_legacy_pool = enable ? 1 : 0;
    return NL_OK;
}

// 监听地址族模式（须在 nl_web_start 之前设置）。
int nl_web_set_listen_mode(nl_web_server_t* server, nl_web_listen_mode_t mode) {
    if (!server) return NL_EINVAL;
    if (mode != NL_WEB_LISTEN_DUAL && mode != NL_WEB_LISTEN_V4 &&
        mode != NL_WEB_LISTEN_V6)
        return NL_EINVAL;
    server->listen_mode = (int)mode;   /* 与 NL_PX_LISTEN_* 数值一致 */
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
        // 先唤醒并等待 TLS 隧道线程退出，避免其访问即将释放的数组
        if (InterlockedCompareExchange(&server->pr_tls_threads, 0, 0) > 0) {
            for (int i = 0; i < p_cap; i++) {
                if (!(server->pr_state[i] & PR_ST_ACTIVE) || !(server->pr_state[i] & PR_ST_TLS)) continue;
                if (server->pr_fd_client && server->pr_fd_client[i] != INVALID_SOCKET)
                    shutdown(server->pr_fd_client[i], SD_BOTH);
                if (server->pr_fd_upstream && server->pr_fd_upstream[i] > 0)
                    shutdown(server->pr_fd_upstream[i], SD_BOTH);
            }
            for (int w = 0; w < 1000 &&
                 InterlockedCompareExchange(&server->pr_tls_threads, 0, 0) > 0; w++)
                Sleep(10);
        }
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
        free(server->pr_up);          server->pr_up          = NULL;
        free(server->pr_up_key);      server->pr_up_key      = NULL;
        free(server->pr_up_pend);     server->pr_up_pend     = NULL;
        free(server->pr_up_pend_len); server->pr_up_pend_len = NULL;
        free(server->pr_last_active_ms); server->pr_last_active_ms = NULL;  // P7 兜底
        free(server->pr_conn_id);        server->pr_conn_id        = NULL;  // slot 复用竞态防护 兜底
        free(server->pr_iocp_cw_ctx); server->pr_iocp_cw_ctx = NULL;
        server->pr_cap = server->pr_count = 0;
    }
    // 销毁上游 keep-alive 连接池（兜底：正常退出时引擎已自清理）
    if (server->up_pool) { nl_px_pool_destroy(server->up_pool); free(server->up_pool); server->up_pool = NULL; }
    
    // 清理 worker 线程（兜底：正常情况下 web_server_thread 内已 join）
    for (int i = 0; i < server->worker_count; i++) {
        if (server->workers && server->workers[i]) {
            CloseHandle(server->workers[i]);
            server->workers[i] = NULL;
        }
    }
    
    nl_log(NL_LOG_INFO, "Web server stopped");
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
        nl_log(NL_LOG_INFO, "Added redirect: %s -> %s", path, content);
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
        nl_log(NL_LOG_INFO, "Added file route: %s -> %s (hot reload)", path, abs_path);
        return;
    }
    
    // Priority 3: Static content
    add_web_route(server, path, content, content_type);
}

// 解析 upstream 字符串为 host + port + protocol（支持 scheme 前缀或默认端口）
// 第195轮：新增 uri_out —— authority 之后的第一个 '/' 起为上游 URI 部分
// （如 http://backend/v1/ 的 "/v1/"），供目录转发的 proxy_pass URI 拼接使用。
// 返回 0 成功，-1 失败。host/port/uri 由调用方提供足够缓冲。
static int nl_parse_upstream(const char* upstream, char* host, size_t host_size,
                             int* port_out, nl_proxy_protocol_t* proto_out,
                             char* uri_out, size_t uri_cap) {
    if (!upstream || upstream[0] == '\0') return -1;
    if (uri_out && uri_cap > 0) uri_out[0] = '\0';

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

    // 分离 authority 与 URI（第一个 '/' 起为 URI；IPv6 [::1]:8080 不含 '/' 不受影响）
    char authority[512];
    const char* slash = strchr(p, '/');
    size_t alen = slash ? (size_t)(slash - p) : strlen(p);
    if (alen >= sizeof(authority)) alen = sizeof(authority) - 1;
    memcpy(authority, p, alen);
    authority[alen] = '\0';
    if (slash && uri_out && uri_cap > 0) snprintf(uri_out, uri_cap, "%s", slash);

    // 分离 host 与 port（支持 [::1]:8080 形式）
    int port = (proto == NL_PROXY_HTTPS) ? 443 : 80;
    if (!nl_px_split_hostport(authority, host, host_size, &port,
                              (proto == NL_PROXY_HTTPS) ? 443 : 80)) {
        snprintf(host, host_size, "%s", authority);
        port = (proto == NL_PROXY_HTTPS) ? 443 : 80;
    }

    *port_out = port;
    *proto_out = proto;
    return 0;
}

// 数据驱动反向代理：将 path 路径的请求转发至上游（http/https/tcp）
// 共享的路由创建实现（第195轮抽出，供精确/目录两个 API 复用）
static int nl_web_add_proxy_common(nl_web_server_t* server, const char* path,
                                   const char* upstream, int is_prefix) {
    if (!server || !path || !upstream) return NL_EINVAL;

    char host[512];
    int port = 0;
    nl_proxy_protocol_t proto = NL_PROXY_HTTP;
    char up_uri[256];
    if (nl_parse_upstream(upstream, host, sizeof(host), &port, &proto,
                          up_uri, sizeof(up_uri)) != 0) {
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
        route->is_prefix = (unsigned char)(is_prefix ? 1 : 0);
        snprintf(route->up_uri, sizeof(route->up_uri), "%s", up_uri);
        route->content = NULL;
        route->content_size = 0;
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
        LeaveCriticalSection(&server->mutex);
        nl_log(NL_LOG_INFO, "Added proxy route: %s -> %s%s%s (%d)", path, route->upstream,
               up_uri[0] ? " " : "", up_uri[0] ? up_uri : "", port);
        return NL_OK;
    }
    LeaveCriticalSection(&server->mutex);
    return NL_ENOMEM;
}

int nl_web_add_proxy(nl_web_server_t* server, const char* path, const char* upstream) {
    return nl_web_add_proxy_common(server, path, upstream, 0);
}

/* 第205轮 CONNECT：注册允许的隧道目标白名单（host 精确匹配不区分大小写，
 * port=0 任意端口；空名单拒绝所有 CONNECT）。最多 16 条，超出返回 -1。 */
int nl_web_connect_allow(nl_web_server_t* server, const char* host, int port) {
    if (!server || !host || !host[0]) return -1;
    int rc = 0;
    EnterCriticalSection(&server->mutex);
    if (server->connect_allow_cnt >= 16) {
        rc = -1;   /* 固定上限：不扩容、不 malloc */
    } else {
        int i = server->connect_allow_cnt;
        snprintf(server->connect_allow_host[i], sizeof(server->connect_allow_host[i]),
                 "%s", host);
        server->connect_allow_port[i] = port;
        server->connect_allow_cnt = i + 1;
    }
    LeaveCriticalSection(&server->mutex);
    return rc;
}

// 第195轮：目录（前缀）转发 —— nginx `location <prefix> { proxy_pass ... }` 语义。
// 命中规则：精确匹配优先，其余按最长前缀选择；upstream 可带 URI
// （http://backend/v1/）命中后剥离前缀拼接，不带则完整路径原样透传。
int nl_web_add_proxy_dir(nl_web_server_t* server, const char* prefix, const char* upstream) {
    if (!prefix || prefix[0] != '/') return NL_EINVAL;   // 目录前缀必须是绝对路径
    return nl_web_add_proxy_common(server, prefix, upstream, 1);
}

// 第203轮：目录/文件端点 —— 端点即根。
//   target 为目录：端点下任意路径映射到目录内（端点内 "../" 被 clamp 保持
//     在该目录中，既不逃逸出端点目录，也不允许跳出端点前缀到达全局路径）；
//   target 为文件：端点下任意请求都服务该文件（文件即根）。
// 与 nl_web_add_html_file（精确 FILE 路由）的区别：后者按整条 URL 精确匹配，
// 本 API 按前缀接管端点下所有子路径。
int nl_web_add_dir(nl_web_server_t* server, const char* prefix, const char* target) {
    if (!server || !prefix || !target || !target[0]) return NL_EINVAL;
    if (prefix[0] != '/') return NL_EINVAL;               // 端点前缀必须是绝对路径

    EnterCriticalSection(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (!route) {
        LeaveCriticalSection(&server->mutex);
        return NL_ENOMEM;
    }
    // 前缀规范化：去尾部 '/'（"/web/" → "/web"），保证字面前缀匹配与
    // pl_dir_resolve 的字面剥前缀一致；规范化后拒绝根 "/"（会吞掉所有路由）
    snprintf(route->path, sizeof(route->path), "%s", prefix);
    size_t pl = strlen(route->path);
    while (pl > 1 && route->path[pl - 1] == '/') route->path[--pl] = '\0';
    if (route->path[1] == '\0') {
        free(route);
        LeaveCriticalSection(&server->mutex);
        return NL_EINVAL;
    }
    snprintf(route->file_path, sizeof(route->file_path), "%s", target);
    route->type = NL_ROUTE_TYPE_DIR;
    route->is_prefix = 1;
    route->next = server->routes;
    server->routes = route;
    nl_route_hash_insert_simple(server, route);
    LeaveCriticalSection(&server->mutex);
    nl_log(NL_LOG_INFO, "Added dir endpoint: %s -> %s", route->path, route->file_path);
    return NL_OK;
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
        nl_log(NL_LOG_INFO, "Added route: %s", path);
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
            nl_log(NL_LOG_INFO, "Removed route: %s", path);
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
            nl_log(NL_LOG_INFO, "Updated route: %s", path);
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
    nl_log(NL_LOG_INFO, "Redirect type set to %d", type);
}

nl_redirect_type_t nl_web_get_redirect_type(nl_web_server_t* server) {
    if (!server) return NL_REDIRECT_TEMPORARY;
    return server->redirect_type;
}

// Add HTTP redirect (alias kept in sync with Linux/macOS 实现)
void nl_web_add_redirect(nl_web_server_t* server, const char* path, const char* target_url) {
    nl_web_add_redirect_302(server, path, target_url);
}

void nl_web_add_redirect_302(nl_web_server_t* server, const char* path, const char* target_url) {
    if (!server || !path || !target_url) return;

    EnterCriticalSection(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (route) {
        strncpy(route->path, path, sizeof(route->path) - 1);
        route->path[sizeof(route->path) - 1] = '\0';
        strncpy(route->redirect_url, target_url, sizeof(route->redirect_url) - 1);
        route->redirect_url[sizeof(route->redirect_url) - 1] = '\0';
        strncpy(route->content_type, "text/html", sizeof(route->content_type) - 1);
        route->type = NL_ROUTE_TYPE_REDIRECT;
        route->content = NULL;
        route->content_size = 0;
        route->file_path[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
    }
    LeaveCriticalSection(&server->mutex);

    nl_log(NL_LOG_INFO, "Added redirect: %s -> %s", path, target_url);
}

// Add HTML from external file (hot reload support，与 Linux/macOS 实现对齐)
int nl_web_add_html_file(nl_web_server_t* server, const char* path, const char* file_path) {
    if (!server || !path || !file_path) return NL_EINVAL;

    // 绝对路径：Windows 用 _fullpath（等价 POSIX realpath）
    char abs_path[512];
    if (!_fullpath(abs_path, file_path, sizeof(abs_path))) {
        size_t slen = strlen(file_path);
        if (slen >= sizeof(abs_path)) slen = sizeof(abs_path) - 1;
        memcpy(abs_path, file_path, slen);
        abs_path[slen] = '\0';
    }

    // Verify file exists
    FILE* fp = fopen(abs_path, "rb");
    if (!fp) {
        nl_log(NL_LOG_ERROR, "File not found: %s", abs_path);
        return NL_EFILE;
    }
    fclose(fp);

    EnterCriticalSection(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (!route) {
        LeaveCriticalSection(&server->mutex);
        return NL_ENOMEM;
    }

    strncpy(route->path, path, sizeof(route->path) - 1);
    route->path[sizeof(route->path) - 1] = '\0';
    strncpy(route->file_path, abs_path, sizeof(route->file_path) - 1);
    route->file_path[sizeof(route->file_path) - 1] = '\0';
    strncpy(route->content_type, "text/html", sizeof(route->content_type) - 1);
    route->type = NL_ROUTE_TYPE_FILE;
    route->content = NULL;
    route->content_size = 0;
    route->redirect_url[0] = '\0';
    route->next = server->routes;
    server->routes = route;
    nl_route_hash_insert_simple(server, route);
    LeaveCriticalSection(&server->mutex);

    nl_log(NL_LOG_INFO, "Added HTML file route: %s -> %s (hot reload)", path, abs_path);
    return NL_OK;
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

