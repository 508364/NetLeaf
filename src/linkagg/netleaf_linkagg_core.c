/**
 * @file netleaf_linkagg_core.c
 * @brief LinkAgg 共享核心实现（三平台共用一份代码）
 *
 * 反向链路聚合模型：
 *   - 监听端口收到的请求不再由 NetLeaf 主动派发给后端，而是按后端端点放入
 *     数据池（每后端一个 FIFO 队列）。
 *   - 服务端宿主程序通过 nl_lagg_pull()（进程内）或数据池 IPC 服务
 *     （跨进程，FETCH/RESP 文本协议）主动拉取属于自己的请求并回送响应。
 *   - 客户端等待宿主响应：超时后重试一次，仍超时则把该端点标记为异常并
 *     返回 503；宿主成功拉取/响应后自动恢复正常。
 *
 * 平台差异被收敛到本文件顶部的 socket / thread 适配层，不再按平台复制实现。
 */
#define _GNU_SOURCE

#include "netleaf_linkagg.h"
#include "netleaf_linkagg_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <process.h>
    typedef SOCKET nl_lagg_sock_t;
    #define NL_LAGG_SOCK_INVALID INVALID_SOCKET
    #define NL_LAGG_SOCK_CLOSE(s)  closesocket(s)
#else
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    typedef int nl_lagg_sock_t;
    #define NL_LAGG_SOCK_INVALID (-1)
    #define NL_LAGG_SOCK_CLOSE(s)  close(s)
#endif

#define NL_LAGG_MAX_EVENTS 64
#define NL_LAGG_RECV_TIMEOUT_MS 3000
#define NL_LAGG_BACKEND_TIMEOUT_MS 5000
#define NL_LAGG_MAX_HEADER_BYTES 16384
#define NL_LAGG_MAX_REQUEST_LIMIT (8u * 1024u * 1024u)
#define NL_LAGG_MAX_RESPONSE_BYTES (8u * 1024u * 1024u)
#define NL_LAGG_IO_CHUNK 8192

// ------------------------------------------------------------------
// 线程适配
// ------------------------------------------------------------------
#ifdef _WIN32
    typedef LPTHREAD_START_ROUTINE nl_lagg_thread_fn;
    #define NL_LAGG_THREAD_FN(name) static DWORD WINAPI name(LPVOID arg)
    #define NL_LAGG_THREAD_RET return 0
    #define NL_LAGG_THREAD_CALL(name, arg) name(arg)
#else
    typedef void* (*nl_lagg_thread_fn)(void*);
    #define NL_LAGG_THREAD_FN(name) static void* name(void* arg)
    #define NL_LAGG_THREAD_RET return NULL
    #define NL_LAGG_THREAD_CALL(name, arg) name(arg)
#endif

static nl_lagg_thread_t nl_lagg_thread_start(nl_lagg_thread_fn fn, void* arg) {
#ifdef _WIN32
    return CreateThread(NULL, 0, fn, arg, 0, NULL);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return NL_LAGG_THREAD_INVALID;
    return t;
#endif
}

static void nl_lagg_thread_detach(nl_lagg_thread_t t) {
    if (t == NL_LAGG_THREAD_INVALID) return;
#ifdef _WIN32
    CloseHandle(t);
#else
    pthread_detach(t);
#endif
}

static void nl_lagg_thread_join(nl_lagg_thread_t t) {
    if (t == NL_LAGG_THREAD_INVALID) return;
#ifdef _WIN32
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
#else
    pthread_join(t, NULL);
#endif
}

static long long nl_lagg_now_ms(void) {
#ifdef _WIN32
    return (long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

static void nl_lagg_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

// 条件变量等待：最多 timeout_ms（WIN 返回 0 成功，POSIX 0 成功）
static int nl_lagg_cond_timedwait(nl_lagg_cond_t* c, nl_lagg_mutex_t* m, int timeout_ms) {
#ifdef _WIN32
    return SleepConditionVariableCS(c, m, (DWORD)timeout_ms) ? 0 : -1;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts);
#endif
}

// ------------------------------------------------------------------
// Socket 适配
// ------------------------------------------------------------------
static int nl_lagg_net_init(void) {
#ifdef _WIN32
    static int started = 0;
    if (!started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        started = 1;
    }
#endif
    return 0;
}

static void nl_lagg_set_nonblocking(nl_lagg_sock_t s, int enable) {
#ifdef _WIN32
    u_long nb = enable ? 1 : 0;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    if (enable) flags |= O_NONBLOCK;
    else flags &= ~O_NONBLOCK;
    fcntl(s, F_SETFL, flags);
#endif
}

static void nl_lagg_set_timeouts(nl_lagg_sock_t s, int recv_ms, int send_ms) {
#ifdef _WIN32
    DWORD v = (DWORD)(recv_ms > 0 ? recv_ms : 0);
    if (recv_ms > 0) setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&v, sizeof(v));
    v = (DWORD)(send_ms > 0 ? send_ms : 0);
    if (send_ms > 0) setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&v, sizeof(v));
#else
    struct timeval tv;
    if (recv_ms > 0) {
        tv.tv_sec = recv_ms / 1000;
        tv.tv_usec = (recv_ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    if (send_ms > 0) {
        tv.tv_sec = send_ms / 1000;
        tv.tv_usec = (send_ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
#endif
}

static int nl_lagg_send_all(nl_lagg_sock_t s, const void* data, size_t len) {
    const char* p = (const char*)data;
    size_t sent = 0;
    while (sent < len) {
        int n = (int)send(s, p + sent, (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

static intptr_t nl_lagg_listen_socket(int port) {
    if (nl_lagg_net_init() != 0) return -1;
    nl_lagg_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == NL_LAGG_SOCK_INVALID) return -1;

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(s, 128) != 0) {
        NL_LAGG_SOCK_CLOSE(s);
        return -1;
    }
    nl_lagg_set_nonblocking(s, 1);
    return (intptr_t)s;
}

// 带 1 秒超时的 accept；返回 -1 表示无连接/停止中
static intptr_t nl_lagg_accept_socket(intptr_t listen_fd, volatile int* running) {
    nl_lagg_sock_t ls = (nl_lagg_sock_t)listen_fd;
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(ls, &rs);
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    int rc = select((int)ls + 1, &rs, NULL, NULL, &tv);
    if (rc <= 0) {
        (void)running;
        return -1;
    }
    nl_lagg_sock_t c = accept(ls, NULL, NULL);
    if (c == NL_LAGG_SOCK_INVALID) return -1;
    return (intptr_t)c;
}

static intptr_t nl_lagg_connect_to(const char* host, int port, int timeout_ms) {
    if (!host || port <= 0) return -1;
    if (nl_lagg_net_init() != 0) return -1;

    nl_lagg_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == NL_LAGG_SOCK_INVALID) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        NL_LAGG_SOCK_CLOSE(s);
        return -1;
    }

    nl_lagg_set_nonblocking(s, 1);
    int rc = connect(s, (struct sockaddr*)&addr, sizeof(addr));
    if (rc != 0) {
#ifdef _WIN32
        int in_progress = (WSAGetLastError() == WSAEWOULDBLOCK);
#else
        int in_progress = (errno == EINPROGRESS);
#endif
        if (!in_progress) {
            NL_LAGG_SOCK_CLOSE(s);
            return -1;
        }
        fd_set ws;
        FD_ZERO(&ws);
        FD_SET(s, &ws);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select((int)s + 1, NULL, &ws, NULL, &tv) <= 0) {
            NL_LAGG_SOCK_CLOSE(s);
            return -1;
        }
        int err = 0;
#ifdef _WIN32
        int err_len = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &err_len) != 0 || err != 0) {
#else
        socklen_t err_len = sizeof(err);
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &err_len) != 0 || err != 0) {
#endif
            NL_LAGG_SOCK_CLOSE(s);
            return -1;
        }
    }
    nl_lagg_set_nonblocking(s, 0);
    nl_lagg_set_timeouts(s, NL_LAGG_BACKEND_TIMEOUT_MS, NL_LAGG_BACKEND_TIMEOUT_MS);
    return (intptr_t)s;
}

// 查找一个空闲 TCP 端口（macOS 的 IPC 实现基于 TCP 回环，需要真实端口）
#ifdef __APPLE__
static int nl_lagg_find_free_port(void) {
    if (nl_lagg_net_init() != 0) return 0;
    nl_lagg_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == NL_LAGG_SOCK_INVALID) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        NL_LAGG_SOCK_CLOSE(s);
        return 0;
    }
    int port = 0;
    struct sockaddr_in bound;
#ifdef _WIN32
    int blen = (int)sizeof(bound);
#else
    socklen_t blen = (socklen_t)sizeof(bound);
#endif
    if (getsockname(s, (struct sockaddr*)&bound, &blen) == 0) {
        port = (int)ntohs(bound.sin_port);
    }
    NL_LAGG_SOCK_CLOSE(s);
    return port;
}
#endif // __APPLE__

// ------------------------------------------------------------------
// 小工具
// ------------------------------------------------------------------
static void nl_lagg_copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char* nl_lagg_ci_find(const char* hay, size_t hay_len, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hay_len < nlen) return NULL;
    for (size_t i = 0; i + nlen <= hay_len; i++) {
        size_t j = 0;
        while (j < nlen) {
            char a = hay[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (j == nlen) return hay + i;
    }
    return NULL;
}

// 返回 Content-Length；无该头返回 -1
static long long nl_lagg_parse_content_length(const char* headers, size_t len) {
    const char* p = nl_lagg_ci_find(headers, len, "\r\ncontent-length:");
    if (!p) {
        if (len >= 15 && nl_lagg_ci_find(headers, len, "content-length:") == headers) {
            p = headers; // 极少见：首行即 Content-Length
        } else {
            return -1;
        }
    }
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    long long v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > (long long)NL_LAGG_MAX_REQUEST_LIMIT) return -1;
        digits++;
        p++;
    }
    return digits ? v : -1;
}

// ------------------------------------------------------------------
// 进程级 ID 注册表互斥量（原先分散在各平台文件中，现共享一份）
// ------------------------------------------------------------------
static nl_lagg_mutex_t g_id_mutex;
static int g_id_mutex_initialized = 0;

void nl_lagg_init_global_mutex(void) {
    if (!g_id_mutex_initialized) {
        NL_LAGG_MUTEX_INIT(&g_id_mutex);
        g_id_mutex_initialized = 1;
    }
}

void nl_lagg_mutex_lock(nl_lagg_mutex_t* m) {
    nl_lagg_init_global_mutex();
    NL_LAGG_MUTEX_LOCK(m);
}

void nl_lagg_mutex_unlock(nl_lagg_mutex_t* m) {
    NL_LAGG_MUTEX_UNLOCK(m);
}

// ------------------------------------------------------------------
// 后端选择 / 健康
// ------------------------------------------------------------------
static int nl_lagg_backend_in_list(nl_lagg_server_t* server, nl_lagg_backend_t* b) {
    nl_lagg_backend_t* it = server->backends;
    while (it) {
        if (it == b) return 1;
        it = it->next;
    }
    return 0;
}

void nl_lagg_backend_mark_healthy(nl_lagg_server_t* server, nl_lagg_backend_t* backend) {
    if (!server || !backend) return;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    if (nl_lagg_backend_in_list(server, backend)) {
        backend->healthy = 1;
        backend->consecutive_failures = 0;
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
}

void nl_lagg_backend_mark_failed(nl_lagg_server_t* server, nl_lagg_backend_t* backend) {
    if (!server || !backend) return;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    if (nl_lagg_backend_in_list(server, backend)) {
        backend->healthy = 0;
        backend->consecutive_failures++;
        backend->last_failure_ms = (uint64_t)nl_lagg_now_ms();
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
}

static nl_lagg_backend_t* nl_lagg_select_backend(nl_lagg_server_t* server) {
    if (!server || !server->backends) return NULL;

    NL_LAGG_MUTEX_LOCK(&server->mutex);

    nl_lagg_backend_t* list[NL_LINKAGG_MAX_BACKENDS];
    int n = 0;
    int healthy_count = 0;
    nl_lagg_backend_t* it = server->backends;
    while (it && n < NL_LINKAGG_MAX_BACKENDS) {
        if (it->healthy) healthy_count++;
        list[n++] = it;
        it = it->next;
    }

    // 优先在健康节点中选择；全部异常时仍尝试（否则无法通过流量自愈）
    nl_lagg_backend_t* eligible[NL_LINKAGG_MAX_BACKENDS];
    int m = 0;
    if (healthy_count > 0) {
        for (int i = 0; i < n; i++) {
            if (list[i]->healthy) eligible[m++] = list[i];
        }
    } else {
        for (int i = 0; i < n; i++) eligible[m++] = list[i];
    }

    nl_lagg_backend_t* result = NULL;
    switch (server->policy) {
        case NL_POLICY_ROUND_ROBIN: {
            if (m > 0) {
                int idx = server->current_index % m;
                result = eligible[idx];
                server->current_index = (idx + 1) % m;
            }
            break;
        }
        case NL_POLICY_RANDOM: {
            if (m > 0) result = eligible[(int)(rand() % m)];
            break;
        }
        case NL_POLICY_LEAST_CONNECTIONS: {
            int best = 0x7fffffff;
            for (int i = 0; i < m; i++) {
                int w = eligible[i]->weight > 0 ? eligible[i]->weight : 1;
                int score = eligible[i]->current_connections / w;
                if (score < best) {
                    best = score;
                    result = eligible[i];
                }
            }
            break;
        }
        case NL_POLICY_WEIGHTED_ROUND_ROBIN: {
            long total = 0;
            long best = -1;
            for (int i = 0; i < m; i++) {
                if (eligible[i]->weight > 0) {
                    eligible[i]->current_weight += eligible[i]->weight;
                    total += eligible[i]->weight;
                    if (eligible[i]->current_weight > best) {
                        best = eligible[i]->current_weight;
                        result = eligible[i];
                    }
                }
            }
            if (result) result->current_weight -= total;
            break;
        }
        default:
            if (m > 0) result = eligible[0];
            break;
    }

    if (result) result->current_connections++;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return result;
}

static void nl_lagg_release_backend(nl_lagg_server_t* server, nl_lagg_backend_t* backend) {
    if (!server || !backend) return;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    if (nl_lagg_backend_in_list(server, backend) && backend->current_connections > 0) {
        backend->current_connections--;
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
}

// ------------------------------------------------------------------
// 请求生命周期（引用计数）
// ------------------------------------------------------------------
static void nl_lagg_request_destroy(nl_lagg_request_t* req) {
    if (!req) return;
    NL_LAGG_COND_DESTROY(&req->cond);
    NL_LAGG_MUTEX_DESTROY(&req->mutex);
    free(req->data);
    free(req->resp);
    free(req);
}

// 调用方不得持有 req->mutex / server->mutex
static void nl_lagg_request_unref(nl_lagg_request_t* req) {
    if (!req) return;
    int destroy = 0;
    NL_LAGG_MUTEX_LOCK(&req->mutex);
    if (--req->refs <= 0) destroy = 1;
    NL_LAGG_MUTEX_UNLOCK(&req->mutex);
    if (destroy) nl_lagg_request_destroy(req);
}

// ------------------------------------------------------------------
// 数据池：入池 / 拉取
// ------------------------------------------------------------------
static void nl_lagg_pool_push(nl_lagg_server_t* server, nl_lagg_backend_t* backend,
                              nl_lagg_request_t* req) {
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    req->queued = 1;
    req->backend = backend;
    if (backend->pool_tail) backend->pool_tail->next = req;
    else backend->pool_head = req;
    backend->pool_tail = req;
    backend->pool_count++;
    // 队列引用在入池前已由调用方计入（req->refs = 客户端 + 队列）
    NL_LAGG_COND_SIGNAL(&server->pool_cond);
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
}

static int nl_lagg_backend_matches(const nl_lagg_server_t* server,
                                   const nl_lagg_backend_t* backend,
                                   const char* endpoint) {
    if (!endpoint || endpoint[0] == '\0' || strcmp(endpoint, "*") == 0) return 1;
    if (strcmp(backend->endpoint, endpoint) == 0) return 1;
    if (server->port_id[0] && strcmp(server->port_id, endpoint) == 0) return 1;
    return 0;
}

NL_LINKAGG_API nl_lagg_request_t* nl_lagg_pull(nl_lagg_server_t* server,
                                               const char* endpoint,
                                               int timeout_ms) {
    if (!server) return NULL;
    if (timeout_ms < 0) timeout_ms = 0;

    long long start = nl_lagg_now_ms();
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    while (server->running) {
        nl_lagg_backend_t* b = server->backends;
        while (b) {
            if (b->type == NL_BACKEND_IPC && b->pool_head &&
                nl_lagg_backend_matches(server, b, endpoint)) {
                nl_lagg_request_t* req = b->pool_head;
                b->pool_head = req->next;
                if (!b->pool_head) b->pool_tail = NULL;
                b->pool_count--;
                req->queued = 0;

                // 已被客户端放弃的条目直接丢弃
                NL_LAGG_MUTEX_LOCK(&req->mutex);
                int aborted = req->aborted;
                NL_LAGG_MUTEX_UNLOCK(&req->mutex);
                if (!aborted) {
                    b->healthy = 1;
                    b->consecutive_failures = 0;
                    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
                    return req;
                }
                // aborted：释放队列引用
                NL_LAGG_MUTEX_UNLOCK(&server->mutex);
                nl_lagg_request_unref(req);
                NL_LAGG_MUTEX_LOCK(&server->mutex);
                break; // 重新扫描
            }
            b = b->next;
        }
        if (b) continue; // 丢弃了一个条目，立即重扫

        long long elapsed = nl_lagg_now_ms() - start;
        if (elapsed >= timeout_ms) break;
        int slice = 100;
        if (timeout_ms - elapsed < slice) slice = (int)(timeout_ms - elapsed);
        nl_lagg_cond_timedwait(&server->pool_cond, &server->mutex, slice > 0 ? slice : 1);
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return NULL;
}

NL_LINKAGG_API int nl_lagg_request_get_id(nl_lagg_request_t* req, uint64_t* out_id) {
    if (!req || !out_id) return NL_LINKAGG_ERROR_INVALID_PARAM;
    *out_id = req->id;
    return NL_LINKAGG_OK;
}

NL_LINKAGG_API const char* nl_lagg_request_get_endpoint(nl_lagg_request_t* req) {
    return req ? req->endpoint : NULL;
}

NL_LINKAGG_API const void* nl_lagg_request_get_data(nl_lagg_request_t* req, size_t* out_len) {
    if (!req) return NULL;
    if (out_len) *out_len = req->len;
    return req->data;
}

NL_LINKAGG_API int nl_lagg_request_respond(nl_lagg_request_t* req,
                                           const void* data, size_t len) {
    if (!req) return NL_LINKAGG_ERROR_INVALID_PARAM;
    if (len > NL_LAGG_MAX_RESPONSE_BYTES) return NL_LINKAGG_ERROR_INVALID_PARAM;
    if (len > 0 && !data) return NL_LINKAGG_ERROR_INVALID_PARAM;

    char* copy = NULL;
    if (len > 0) {
        copy = (char*)malloc(len);
        if (!copy) return NL_LINKAGG_ERROR_MEMORY;
        memcpy(copy, data, len);
    }

    int rc = NL_LINKAGG_OK;
    int free_after = 0;

    // 先更新健康状态（不持有 req->mutex，避免与拉取路径的加锁顺序冲突）
    if (req->server && req->backend) {
        nl_lagg_backend_mark_healthy(req->server, req->backend);
    }

    NL_LAGG_MUTEX_LOCK(&req->mutex);
    if (req->responded || req->aborted) {
        rc = req->aborted ? NL_LINKAGG_ERROR_ABORTED : NL_LINKAGG_ERROR;
        free(copy);
        copy = NULL;
    } else {
        free(req->resp);
        req->resp = copy;
        req->resp_len = len;
        req->responded = 1;
        NL_LAGG_COND_SIGNAL(&req->cond);
        if (--req->refs <= 0) free_after = 1;
    }
    NL_LAGG_MUTEX_UNLOCK(&req->mutex);
    if (free_after) nl_lagg_request_destroy(req);
    return rc;
}

NL_LINKAGG_API void nl_lagg_request_free(nl_lagg_request_t* req) {
    if (!req) return;
    int free_after = 0;
    NL_LAGG_MUTEX_LOCK(&req->mutex);
    if (!req->responded && !req->aborted) {
        req->aborted = 1;
        NL_LAGG_COND_SIGNAL(&req->cond);
    }
    if (--req->refs <= 0) free_after = 1;
    NL_LAGG_MUTEX_UNLOCK(&req->mutex);
    if (free_after) nl_lagg_request_destroy(req);
}

// 客户端等待宿主响应（超时重试 retries 次）
static int nl_lagg_wait_for_response(nl_lagg_request_t* req, int timeout_ms, int retries) {
    if (timeout_ms <= 0) timeout_ms = NL_LINKAGG_PULL_DEFAULT_TIMEOUT_MS;
    if (retries < 0) retries = 0;

    int responded = 0;
    NL_LAGG_MUTEX_LOCK(&req->mutex);
    for (int attempt = 0; attempt <= retries && !req->responded && !req->aborted; attempt++) {
        long long start = nl_lagg_now_ms();
        while (!req->responded && !req->aborted) {
            long long elapsed = nl_lagg_now_ms() - start;
            if (elapsed >= timeout_ms) break;
            int slice = timeout_ms - (int)elapsed;
            if (slice > 100) slice = 100;
            nl_lagg_cond_timedwait(&req->cond, &req->mutex, slice > 0 ? slice : 1);
        }
    }
    responded = req->responded;
    NL_LAGG_MUTEX_UNLOCK(&req->mutex);
    return responded;
}

// 客户端放弃等待：从池中摘除（若仍在队列），设置 aborted 并释放客户端引用
static void nl_lagg_abort_request(nl_lagg_server_t* server, nl_lagg_request_t* req) {
    int pool_ref_released = 0;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    if (req->queued && req->backend) {
        nl_lagg_backend_t* b = req->backend;
        if (nl_lagg_backend_in_list(server, b) && b->pool_head == req) {
            b->pool_head = req->next;
            if (!b->pool_head) b->pool_tail = NULL;
            b->pool_count--;
            req->queued = 0;
            pool_ref_released = 1;
        } else if (nl_lagg_backend_in_list(server, b)) {
            // 非队首：遍历摘链
            nl_lagg_request_t* prev = b->pool_head;
            while (prev && prev->next != req) prev = prev->next;
            if (prev) {
                prev->next = req->next;
                if (b->pool_tail == req) b->pool_tail = prev;
                b->pool_count--;
                req->queued = 0;
                pool_ref_released = 1;
            }
        } else {
            req->queued = 0; // 后端已被移除，队列随之失效
            pool_ref_released = 1;
        }
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);

    int free_after = 0;
    NL_LAGG_MUTEX_LOCK(&req->mutex);
    if (!req->responded) req->aborted = 1;
    if (pool_ref_released) req->refs--;   // 队列引用
    req->refs--;                          // 客户端引用
    NL_LAGG_COND_BROADCAST(&req->cond);
    if (req->refs <= 0) free_after = 1;
    NL_LAGG_MUTEX_UNLOCK(&req->mutex);
    if (free_after) nl_lagg_request_destroy(req);
}

// ------------------------------------------------------------------
// 前端 HTTP：读取请求 / 转发到 HTTP 后端 / 拉取型后端入池
// ------------------------------------------------------------------
static int nl_lagg_read_request(nl_lagg_sock_t s, char** out_buf, size_t* out_len) {
    size_t cap = NL_LAGG_IO_CHUNK * 2;
    char* buf = (char*)malloc(cap);
    if (!buf) return -1;
    size_t total = 0;
    size_t header_end = 0; // 指向 \r\n\r\n 之后的偏移
    long long body_len = -1;

    while (1) {
        if (total + 1 >= cap) {
            if (cap >= NL_LAGG_MAX_REQUEST_LIMIT) break;
            size_t ncap = cap * 2;
            if (ncap > NL_LAGG_MAX_REQUEST_LIMIT) ncap = NL_LAGG_MAX_REQUEST_LIMIT;
            char* nb = (char*)realloc(buf, ncap);
            if (!nb) break;
            buf = nb;
            cap = ncap;
        }
        int n = (int)recv(s, buf + total, (int)(cap - total - 1), 0);
        if (n <= 0) break;
        total += (size_t)n;

        if (header_end == 0) {
            buf[total] = '\0';
            const char* term = nl_lagg_ci_find(buf, total, "\r\n\r\n");
            if (term) {
                header_end = (size_t)(term - buf) + 4;
                if (header_end > NL_LAGG_MAX_HEADER_BYTES) {
                    free(buf);
                    return -1;
                }
                body_len = nl_lagg_parse_content_length(buf, header_end);
            } else if (total > NL_LAGG_MAX_HEADER_BYTES) {
                free(buf);
                return -1;
            }
        }

        if (header_end > 0) {
            if (body_len < 0) break; // 无 Content-Length，视为无 body
            size_t expected = header_end + (size_t)body_len;
            if (total >= expected) break;
        }
    }

    if (total == 0 || header_end == 0) {
        free(buf);
        return -1;
    }
    buf[total] = '\0';
    *out_buf = buf;
    *out_len = total;
    return 0;
}

// 转发请求到 HTTP 后端的监听端口，并把响应流式回传给客户端
static int nl_lagg_forward_http(nl_lagg_backend_t* backend, const char* data, size_t len,
                                nl_lagg_sock_t client) {
    intptr_t fd = nl_lagg_connect_to(backend->host, backend->port, 3000);
    if (fd == -1) return -1;
    nl_lagg_sock_t bs = (nl_lagg_sock_t)fd;

    if (nl_lagg_send_all(bs, data, len) != 0) {
        NL_LAGG_SOCK_CLOSE(bs);
        return -1;
    }

    size_t header_scan = 0;
    size_t relayed = 0;
    long long content_len = -1;
    size_t header_bytes = 0;
    int header_done = 0;

    char* hdr = (char*)malloc(NL_LAGG_MAX_HEADER_BYTES);
    if (!hdr) {
        NL_LAGG_SOCK_CLOSE(bs);
        return -1;
    }

    char chunk[NL_LAGG_IO_CHUNK];
    while (1) {
        int n = (int)recv(bs, chunk, sizeof(chunk), 0);
        if (n <= 0) break;

        if (!header_done) {
            size_t copy = (size_t)n;
            if (header_scan + copy > NL_LAGG_MAX_HEADER_BYTES) {
                copy = NL_LAGG_MAX_HEADER_BYTES - header_scan;
            }
            memcpy(hdr + header_scan, chunk, copy);
            header_scan += copy;
            const char* term = nl_lagg_ci_find(hdr, header_scan, "\r\n\r\n");
            if (term) {
                header_done = 1;
                header_bytes = (size_t)(term - hdr) + 4;
                content_len = nl_lagg_parse_content_length(hdr, header_bytes);
            }
        }

        if (nl_lagg_send_all(client, chunk, (size_t)n) != 0) break;
        relayed += (size_t)n;
        if (header_done && content_len >= 0 &&
            relayed >= header_bytes + (size_t)content_len) break;
    }
    free(hdr);
    NL_LAGG_SOCK_CLOSE(bs);
    return relayed > 0 ? 0 : -1;
}

static void nl_lagg_send_status(nl_lagg_sock_t s, int code, const char* reason) {
    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                     code, reason ? reason : "");
    if (n > 0) nl_lagg_send_all(s, buf, (size_t)n);
}

// 拉取型后端：入池并等待宿主响应
static void nl_lagg_dispatch_pull(nl_lagg_server_t* server, nl_lagg_backend_t* backend,
                                  char* data, size_t len, nl_lagg_sock_t client) {
    nl_lagg_request_t* req = (nl_lagg_request_t*)calloc(1, sizeof(nl_lagg_request_t));
    if (!req) {
        free(data);
        nl_lagg_send_status(client, 503, "Service Unavailable");
        return;
    }
    NL_LAGG_MUTEX_INIT(&req->mutex);
    NL_LAGG_COND_INIT(&req->cond);
    req->server = server;
    req->backend = backend;
    req->data = data;
    req->len = len;
    req->refs = 2; // 客户端引用 + 队列引用

    NL_LAGG_MUTEX_LOCK(&server->mutex);
    req->id = ++server->next_request_id;
    nl_lagg_copy_str(req->port_id, sizeof(req->port_id), server->port_id);
    nl_lagg_copy_str(req->endpoint, sizeof(req->endpoint), backend->endpoint);
    server->total_requests++;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);

    nl_lagg_pool_push(server, backend, req);

    int responded = nl_lagg_wait_for_response(req, server->pull_timeout_ms, server->pull_retries);
    if (!responded) {
        nl_lagg_abort_request(server, req);
        nl_lagg_backend_mark_failed(server, backend);
        NL_LAGG_MUTEX_LOCK(&server->mutex);
        server->failed_requests++;
        NL_LAGG_MUTEX_UNLOCK(&server->mutex);
        nl_lagg_send_status(client, 503, "Service Unavailable");
        return;
    }

    // 取走响应并释放客户端引用
    char* resp = NULL;
    size_t resp_len = 0;
    int free_after = 0;
    NL_LAGG_MUTEX_LOCK(&req->mutex);
    resp = req->resp;
    resp_len = req->resp_len;
    req->resp = NULL;
    if (--req->refs <= 0) free_after = 1;
    NL_LAGG_MUTEX_UNLOCK(&req->mutex);
    if (free_after) nl_lagg_request_destroy(req);

    if (resp && resp_len > 0) {
        nl_lagg_send_all(client, resp, resp_len);
    } else {
        nl_lagg_send_status(client, 502, "Bad Gateway");
    }
    free(resp);
    nl_lagg_backend_mark_healthy(server, backend);
}

typedef struct {
    nl_lagg_server_t* server;
    nl_lagg_sock_t client;
} nl_lagg_conn_arg_t;

NL_LAGG_THREAD_FN(nl_lagg_conn_thread) {
    nl_lagg_conn_arg_t* a = (nl_lagg_conn_arg_t*)arg;
    nl_lagg_server_t* server = a->server;
    nl_lagg_sock_t client = a->client;
    free(a);

    char* request = NULL;
    size_t request_len = 0;
    if (nl_lagg_read_request(client, &request, &request_len) == 0) {
        nl_lagg_backend_t* backend = nl_lagg_select_backend(server);
        if (!backend) {
            free(request);
            nl_lagg_send_status(client, 503, "Service Unavailable");
        } else if (backend->type == NL_BACKEND_HTTP) {
            if (nl_lagg_forward_http(backend, request, request_len, client) != 0) {
                nl_lagg_backend_mark_failed(server, backend);
                nl_lagg_send_status(client, 502, "Bad Gateway");
            } else {
                nl_lagg_backend_mark_healthy(server, backend);
            }
            free(request);
            nl_lagg_release_backend(server, backend);
        } else {
            nl_lagg_release_backend(server, backend);
            nl_lagg_dispatch_pull(server, backend, request, request_len, client);
            // request 的所有权已转移给数据池条目
        }
    }

    if (server->on_disconnect) {
        server->on_disconnect((int)client, server->user_data);
    }
    NL_LAGG_SOCK_CLOSE(client);

    NL_LAGG_MUTEX_LOCK(&server->mutex);
    if (server->active_conns > 0) server->active_conns--;
    NL_LAGG_COND_BROADCAST(&server->pool_cond);
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    NL_LAGG_THREAD_RET;
}

// 监听线程：accept 后交给独立的连接线程处理，避免慢请求阻塞接入
NL_LAGG_THREAD_FN(nl_lagg_conn_accept_entry) {
    nl_lagg_server_t* server = (nl_lagg_server_t*)arg;
    while (server->running) {
        intptr_t client = nl_lagg_accept_socket(server->listen_fd, &server->running);
        if (client == -1) continue;

        nl_lagg_sock_t cs = (nl_lagg_sock_t)client;
        nl_lagg_set_timeouts(cs, NL_LAGG_RECV_TIMEOUT_MS, NL_LAGG_RECV_TIMEOUT_MS);

        if (server->on_connect) server->on_connect((int)cs, server->user_data);

        NL_LAGG_MUTEX_LOCK(&server->mutex);
        server->active_conns++;
        NL_LAGG_MUTEX_UNLOCK(&server->mutex);

        nl_lagg_conn_arg_t* a = (nl_lagg_conn_arg_t*)calloc(1, sizeof(nl_lagg_conn_arg_t));
        if (!a) {
            NL_LAGG_SOCK_CLOSE(cs);
            NL_LAGG_MUTEX_LOCK(&server->mutex);
            if (server->active_conns > 0) server->active_conns--;
            NL_LAGG_MUTEX_UNLOCK(&server->mutex);
            continue;
        }
        a->server = server;
        a->client = cs;

        nl_lagg_thread_t t = nl_lagg_thread_start(nl_lagg_conn_thread, a);
        if (t == NL_LAGG_THREAD_INVALID) {
            free(a);
            NL_LAGG_SOCK_CLOSE(cs);
            NL_LAGG_MUTEX_LOCK(&server->mutex);
            if (server->active_conns > 0) server->active_conns--;
            NL_LAGG_MUTEX_UNLOCK(&server->mutex);
        } else {
            nl_lagg_thread_detach(t);
        }
    }
    NL_LAGG_THREAD_RET;
}

// ------------------------------------------------------------------
// 跨进程数据池服务（FETCH/RESP 协议）
// ------------------------------------------------------------------
typedef struct {
    void* conn;
    char buf[NL_LAGG_IO_CHUNK];
    size_t len;
    size_t pos;
} nl_lagg_reader_t;

static int nl_lagg_reader_fill(nl_lagg_reader_t* r) {
    if (r->pos > 0 && r->pos >= r->len) {
        r->len = 0;
        r->pos = 0;
    }
    if (r->len >= sizeof(r->buf)) return -1;
    size_t got = 0;
    if (nl_ipc_recv(r->conn, r->buf + r->len, sizeof(r->buf) - r->len, &got) != 0) return -1;
    if (got == 0) return -1;
    r->len += got;
    return 0;
}

static int nl_lagg_reader_line(nl_lagg_reader_t* r, char* out, size_t cap) {
    size_t n = 0;
    while (1) {
        while (r->pos < r->len) {
            char c = r->buf[r->pos++];
            if (c == '\n') {
                while (n > 0 && (out[n - 1] == '\r' || out[n - 1] == ' ')) n--;
                out[n] = '\0';
                return 0;
            }
            if (n + 1 < cap) out[n++] = c;
        }
        if (nl_lagg_reader_fill(r) != 0) return -1;
    }
}

static int nl_lagg_reader_exact(nl_lagg_reader_t* r, void* dst, size_t n) {
    char* out = (char*)dst;
    size_t done = 0;
    while (done < n) {
        if (r->pos >= r->len) {
            if (nl_lagg_reader_fill(r) != 0) return -1;
        }
        size_t avail = r->len - r->pos;
        size_t take = n - done;
        if (take > avail) take = avail;
        memcpy(out + done, r->buf + r->pos, take);
        r->pos += take;
        done += take;
    }
    return 0;
}

static int nl_lagg_ipc_send_str(void* conn, const char* s) {
    return nl_ipc_send(conn, s, strlen(s));
}

static void nl_lagg_pool_conn_handle(nl_lagg_server_t* server, void* conn) {
    nl_lagg_reader_t* r = (nl_lagg_reader_t*)calloc(1, sizeof(nl_lagg_reader_t));
    if (!r) return;
    r->conn = conn;

    char line[NL_LAGG_IPC_LINE_MAX];
    while (server->running) {
        if (nl_lagg_reader_line(r, line, sizeof(line)) != 0) break;
        if (strcmp(line, "BYE") == 0) break;

        char cmd[32], endpoint[512];
        int timeout_ms = 0;
        if (sscanf(line, "%31s %511s %d", cmd, endpoint, &timeout_ms) < 2) {
            nl_lagg_ipc_send_str(conn, "ERR BADLINE\n");
            continue;
        }
        if (strcmp(cmd, "FETCH") != 0) {
            nl_lagg_ipc_send_str(conn, "ERR BADCMD\n");
            continue;
        }
        if (timeout_ms < 0) timeout_ms = 0;

        nl_lagg_request_t* req = nl_lagg_pull(server, endpoint, timeout_ms);
        if (!req) {
            nl_lagg_ipc_send_str(conn, "NONE\n");
            continue;
        }

        uint64_t id = 0;
        size_t len = 0;
        const void* data = nl_lagg_request_get_data(req, &len);
        nl_lagg_request_get_id(req, &id);
        char head[128];
        snprintf(head, sizeof(head), "ITEM %llu %zu\n", (unsigned long long)id, len);
        if (nl_lagg_ipc_send_str(conn, head) != 0 || (len > 0 && nl_ipc_send(conn, data, len) != 0)) {
            nl_lagg_request_free(req);
            break;
        }

        if (nl_lagg_reader_line(r, line, sizeof(line)) != 0) {
            nl_lagg_request_free(req);
            break;
        }
        if (strcmp(line, "BYE") == 0) {
            nl_lagg_request_free(req);
            break;
        }

        char cmd2[32];
        uint64_t rid = 0;
        size_t rlen = 0;
        if (sscanf(line, "%31s %llu %zu", cmd2, (unsigned long long*)&rid, &rlen) < 3) {
            nl_lagg_request_free(req);
            break;
        }
        if (rid != id || rlen > NL_LAGG_MAX_RESPONSE_BYTES) {
            nl_lagg_request_free(req);
            break;
        }
        if (strcmp(cmd2, "DROP") == 0) {
            nl_lagg_request_free(req);
            continue;
        }
        if (strcmp(cmd2, "RESP") != 0) {
            nl_lagg_request_free(req);
            break;
        }

        char* payload = NULL;
        if (rlen > 0) {
            payload = (char*)malloc(rlen);
            if (!payload || nl_lagg_reader_exact(r, payload, rlen) != 0) {
                free(payload);
                nl_lagg_request_free(req);
                break;
            }
        }
        nl_lagg_request_respond(req, payload, rlen);
        free(payload);
    }
    free(r);
}

// 宿主连接线程上下文（同时携带 server 与节点）
typedef struct {
    nl_lagg_server_t* server;
    nl_lagg_pool_conn_t* node;
} nl_lagg_pool_ctx_t;

NL_LAGG_THREAD_FN(nl_lagg_pool_host_thread) {
    nl_lagg_pool_ctx_t* ctx = (nl_lagg_pool_ctx_t*)arg;
    nl_lagg_server_t* server = ctx->server;
    nl_lagg_pool_conn_t* node = ctx->node;
    free(ctx);

    nl_lagg_pool_conn_handle(server, node->conn);

    NL_LAGG_MUTEX_LOCK(&server->pool_mutex);
    int owned_by_stop = node->owned_by_stop;
    if (!owned_by_stop) {
        nl_lagg_pool_conn_t* prev = NULL;
        nl_lagg_pool_conn_t* it = server->pool_conns;
        while (it) {
            if (it == node) {
                if (prev) prev->next = it->next;
                else server->pool_conns = it->next;
                break;
            }
            prev = it;
            it = it->next;
        }
    }
    NL_LAGG_MUTEX_UNLOCK(&server->pool_mutex);

    if (!owned_by_stop) {
        nl_ipc_close(node->conn);
        node->conn = NULL;
        free(node);
    }
    NL_LAGG_THREAD_RET;
}

NL_LAGG_THREAD_FN(nl_lagg_pool_accept_thread) {
    nl_lagg_server_t* server = (nl_lagg_server_t*)arg;
    while (server->running) {
        void* conn = NULL;
        if (nl_ipc_accept(server->pool_ipc, &conn) != 0) {
            if (!server->running) break;
            nl_lagg_sleep_ms(20);
            continue;
        }
        if (!server->running || !conn) {
            if (conn) nl_ipc_close(conn);
            break;
        }

        nl_lagg_pool_conn_t* node = (nl_lagg_pool_conn_t*)calloc(1, sizeof(nl_lagg_pool_conn_t));
        nl_lagg_pool_ctx_t* ctx = (nl_lagg_pool_ctx_t*)calloc(1, sizeof(nl_lagg_pool_ctx_t));
        if (!node || !ctx) {
            free(node);
            free(ctx);
            nl_ipc_close(conn);
            continue;
        }
        node->conn = conn;
        ctx->server = server;
        ctx->node = node;

        // 在持有 pool_mutex 的情况下创建线程并挂链：
        // 保证 stop 拿到的节点一定带有有效的线程句柄（避免 join 空句柄的竞态）
        NL_LAGG_MUTEX_LOCK(&server->pool_mutex);
        node->thread = nl_lagg_thread_start(nl_lagg_pool_host_thread, ctx);
        if (node->thread != NL_LAGG_THREAD_INVALID) {
            node->next = server->pool_conns;
            server->pool_conns = node;
        }
        NL_LAGG_MUTEX_UNLOCK(&server->pool_mutex);

        if (node->thread == NL_LAGG_THREAD_INVALID) {
            nl_ipc_close(conn);
            free(ctx);
            free(node);
        }
    }
    NL_LAGG_THREAD_RET;
}

// ------------------------------------------------------------------
// 公共 API：创建 / 启停 / 后端管理
// ------------------------------------------------------------------
static void nl_lagg_make_pool_endpoint(nl_lagg_server_t* server) {
#if defined(_WIN32)
    snprintf(server->pool_endpoint, sizeof(server->pool_endpoint),
             "\\\\.\\pipe\\netleaf_lagg_%d", server->port);
#elif defined(__APPLE__)
    if (server->pool_port <= 0) server->pool_port = nl_lagg_find_free_port();
    snprintf(server->pool_endpoint, sizeof(server->pool_endpoint),
             "127.0.0.1:%d", server->pool_port);
#else
    snprintf(server->pool_endpoint, sizeof(server->pool_endpoint),
             "/tmp/netleaf_lagg_%d.sock", server->port);
#endif
}

NL_LINKAGG_API nl_lagg_server_t* nl_lagg_create(int port) {
    if (port <= 0 || port > 65535) return NULL;
    if (nl_lagg_net_init() != 0) return NULL;
    nl_lagg_init_global_mutex();

    nl_lagg_server_t* server = (nl_lagg_server_t*)calloc(1, sizeof(nl_lagg_server_t));
    if (!server) return NULL;

    server->port = port;
    server->policy = NL_POLICY_ROUND_ROBIN;
    server->running = 0;
    server->listen_fd = -1;
    server->pull_timeout_ms = NL_LINKAGG_PULL_DEFAULT_TIMEOUT_MS;
    server->pull_retries = NL_LINKAGG_PULL_DEFAULT_RETRIES;
    server->pool_service_enabled = NL_LAGG_POOL_SERVICE_DEFAULT;
    NL_LAGG_MUTEX_INIT(&server->mutex);
    NL_LAGG_MUTEX_INIT(&server->pool_mutex);
    NL_LAGG_COND_INIT(&server->pool_cond);
    nl_lagg_make_pool_endpoint(server);
    return server;
}

NL_LINKAGG_API void nl_lagg_destroy(nl_lagg_server_t* server) {
    if (!server) return;
    nl_lagg_stop(server);

    nl_lagg_backend_t* b = server->backends;
    while (b) {
        nl_lagg_backend_t* next = b->next;
        free(b);
        b = next;
    }
    NL_LAGG_COND_DESTROY(&server->pool_cond);
    NL_LAGG_MUTEX_DESTROY(&server->pool_mutex);
    NL_LAGG_MUTEX_DESTROY(&server->mutex);
    free(server);
}

NL_LINKAGG_API int nl_lagg_start(nl_lagg_server_t* server) {
    if (!server || server->running) return NL_LINKAGG_ERROR;

    intptr_t fd = nl_lagg_listen_socket(server->port);
    if (fd == -1) return NL_LINKAGG_ERROR_PORT_BIND_FAILED;
    server->listen_fd = fd;
    server->running = 1;
    server->active_conns = 0;

    // 跨进程数据池服务（失败时降级为仅进程内拉取）
    if (server->pool_service_enabled) {
        nl_ipc_t* ipc = nl_ipc_create(server->pool_endpoint);
        if (ipc && nl_ipc_listen(ipc) == 0) {
            server->pool_ipc = ipc;
            server->pool_thread = nl_lagg_thread_start(nl_lagg_pool_accept_thread, server);
            if (server->pool_thread == NL_LAGG_THREAD_INVALID) {
                nl_ipc_destroy(ipc);
                server->pool_ipc = NULL;
            } else {
                server->pool_thread_valid = 1;
            }
        } else if (ipc) {
            nl_ipc_destroy(ipc);
        }
    }

    server->accept_thread = nl_lagg_thread_start(nl_lagg_conn_accept_entry, server);
    if (server->accept_thread == NL_LAGG_THREAD_INVALID) {
        server->running = 0;
        NL_LAGG_SOCK_CLOSE((nl_lagg_sock_t)server->listen_fd);
        server->listen_fd = -1;
        return NL_LINKAGG_ERROR;
    }
    server->accept_thread_valid = 1;
    return NL_LINKAGG_OK;
}

NL_LINKAGG_API void nl_lagg_stop(nl_lagg_server_t* server) {
    if (!server) return;
    if (!server->running && !server->accept_thread_valid) return;

    server->running = 0;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    NL_LAGG_COND_BROADCAST(&server->pool_cond);
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);

    if (server->accept_thread_valid) {
        nl_lagg_thread_join(server->accept_thread);
        server->accept_thread_valid = 0;
    }
    if (server->listen_fd != -1) {
        NL_LAGG_SOCK_CLOSE((nl_lagg_sock_t)server->listen_fd);
        server->listen_fd = -1;
    }

    // 唤醒阻塞在 accept 的池服务线程
    if (server->pool_thread_valid && server->pool_ipc) {
        void* wake = NULL;
        nl_ipc_t* tmp = nl_ipc_create(server->pool_endpoint);
        if (tmp && nl_ipc_connect(tmp, &wake) == 0 && wake) {
            nl_ipc_close(wake);
        }
        if (tmp) nl_ipc_destroy(tmp);
        nl_lagg_thread_join(server->pool_thread);
        server->pool_thread_valid = 0;
    }

    // 通知并回收所有宿主连接线程
    while (1) {
        NL_LAGG_MUTEX_LOCK(&server->pool_mutex);
        nl_lagg_pool_conn_t* node = server->pool_conns;
        if (node) {
            server->pool_conns = node->next;
            node->owned_by_stop = 1;
        }
        NL_LAGG_MUTEX_UNLOCK(&server->pool_mutex);
        if (!node) break;
        nl_lagg_ipc_send_str(node->conn, "BYE\n");
        nl_lagg_thread_join(node->thread);
        if (node->conn) nl_ipc_close(node->conn);
        free(node);
    }

    if (server->pool_ipc) {
        nl_ipc_destroy(server->pool_ipc);
        server->pool_ipc = NULL;
    }

    // 等待在途客户端连接处理完毕（读取/等待均有上限）
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    while (server->active_conns > 0) {
        nl_lagg_cond_timedwait(&server->pool_cond, &server->mutex, 100);
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
}

NL_LINKAGG_API int nl_lagg_add_http_backend(nl_lagg_server_t* server, const char* host,
                                            int port, int weight) {
    if (!server || !host || port <= 0) return NL_LINKAGG_ERROR_INVALID_PARAM;
    if (server->backend_count >= NL_LINKAGG_MAX_BACKENDS) return NL_LINKAGG_ERROR_TOO_MANY_BACKENDS;

    nl_lagg_backend_t* b = (nl_lagg_backend_t*)calloc(1, sizeof(nl_lagg_backend_t));
    if (!b) return NL_LINKAGG_ERROR_MEMORY;

    b->type = NL_BACKEND_HTTP;
    nl_lagg_copy_str(b->host, sizeof(b->host), host);
    b->port = port;
    snprintf(b->endpoint, sizeof(b->endpoint), "%s:%d", host, port);
    b->weight = weight > 0 ? weight : 1;
    b->healthy = 1;

    NL_LAGG_MUTEX_LOCK(&server->mutex);
    b->next = server->backends;
    server->backends = b;
    server->backend_count++;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return NL_LINKAGG_OK;
}

NL_LINKAGG_API int nl_lagg_add_ipc_backend(nl_lagg_server_t* server, const char* endpoint,
                                           int weight) {
    if (!server || !endpoint) return NL_LINKAGG_ERROR_INVALID_PARAM;
    if (server->backend_count >= NL_LINKAGG_MAX_BACKENDS) return NL_LINKAGG_ERROR_TOO_MANY_BACKENDS;

    nl_lagg_backend_t* b = (nl_lagg_backend_t*)calloc(1, sizeof(nl_lagg_backend_t));
    if (!b) return NL_LINKAGG_ERROR_MEMORY;

    b->type = NL_BACKEND_IPC;
    nl_lagg_copy_str(b->endpoint, sizeof(b->endpoint), endpoint);
    b->weight = weight > 0 ? weight : 1;
    b->healthy = 1;

    NL_LAGG_MUTEX_LOCK(&server->mutex);
    b->next = server->backends;
    server->backends = b;
    server->backend_count++;
    NL_LAGG_COND_BROADCAST(&server->pool_cond);
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return NL_LINKAGG_OK;
}

NL_LINKAGG_API int nl_lagg_remove_backend(nl_lagg_server_t* server, const char* endpoint) {
    if (!server || !endpoint) return NL_LINKAGG_ERROR_INVALID_PARAM;
    int found = 0;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    nl_lagg_backend_t* prev = NULL;
    nl_lagg_backend_t* b = server->backends;
    while (b) {
        if (strcmp(b->endpoint, endpoint) == 0) {
            if (prev) prev->next = b->next;
            else server->backends = b->next;
            server->backend_count--;
            free(b);
            found = 1;
            break;
        }
        prev = b;
        b = b->next;
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return found ? NL_LINKAGG_OK : NL_LINKAGG_ERROR;
}

NL_LINKAGG_API void nl_lagg_set_policy(nl_lagg_server_t* server, nl_lagg_policy_t policy) {
    if (!server) return;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    server->policy = (int)policy;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
}

NL_LINKAGG_API void nl_lagg_set_on_connect(nl_lagg_server_t* server,
                                           nl_lagg_on_connect_cb cb, void* user_data) {
    if (!server) return;
    server->on_connect = cb;
    server->user_data = user_data;
}

NL_LINKAGG_API void nl_lagg_set_on_disconnect(nl_lagg_server_t* server,
                                              nl_lagg_on_disconnect_cb cb, void* user_data) {
    if (!server) return;
    server->on_disconnect = cb;
    server->user_data = user_data;
}

// ------------------------------------------------------------------
// 拉取参数 / 状态查询 / 跨进程端点
// ------------------------------------------------------------------
NL_LINKAGG_API void nl_lagg_set_pull_timeout(nl_lagg_server_t* server,
                                             int timeout_ms, int retries) {
    if (!server) return;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    server->pull_timeout_ms = timeout_ms > 0 ? timeout_ms : NL_LINKAGG_PULL_DEFAULT_TIMEOUT_MS;
    server->pull_retries = retries >= 0 ? retries : NL_LINKAGG_PULL_DEFAULT_RETRIES;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
}

NL_LINKAGG_API int nl_lagg_get_pull_timeout(nl_lagg_server_t* server) {
    if (!server) return 0;
    return server->pull_timeout_ms;
}

NL_LINKAGG_API int nl_lagg_get_backend_health(nl_lagg_server_t* server, const char* endpoint) {
    if (!server || !endpoint) return -1;
    int health = -1;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    nl_lagg_backend_t* b = server->backends;
    while (b) {
        if (strcmp(b->endpoint, endpoint) == 0) {
            health = b->healthy ? 1 : 0;
            break;
        }
        b = b->next;
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return health;
}

NL_LINKAGG_API const char* nl_lagg_get_pool_endpoint(nl_lagg_server_t* server,
                                                     char* buf, size_t buf_len) {
    if (!server) return NULL;
    if (buf && buf_len > 0) {
        nl_lagg_copy_str(buf, buf_len, server->pool_endpoint);
        return buf;
    }
    return server->pool_endpoint;
}

NL_LINKAGG_API int nl_lagg_set_pool_service(nl_lagg_server_t* server, int enable) {
    if (!server) return NL_LINKAGG_ERROR_INVALID_PARAM;
    if (server->running) return NL_LINKAGG_ERROR;
    server->pool_service_enabled = enable ? 1 : 0;
    return NL_LINKAGG_OK;
}
