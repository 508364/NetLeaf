/*
 * nl_px_h2.c - HTTP/2 反代桥接（平台无关，一份实现）
 *
 * 设计要点见 nl_px_h2.h。这里补齐实现约定：
 *   - h2c 下游：read 线程独占客户端 fd，顺序解码 HPACK（会话级状态必须串行），
 *     每个 stream 在 END_STREAM 后起一个工作线程，用 HTTP/1.1 + Connection:
 *     close 回放给 127.0.0.1:local_port，再把响应切成 HEADERS/DATA。
 *   - 发送侧：帧写入用独立互斥锁串行化；流控窗口用状态锁 + 条件变量等待，
 *     两个锁的获取顺序固定为"先状态锁、释放后再写锁"，避免与 WINDOW_UPDATE
 *     的发送互相死锁。
 *   - HPACK 编码表固定为"不使用动态索引"（不插入条目），因此无需跟踪对端
 *     SETTINGS_HEADER_TABLE_SIZE，也不会因表容量收缩而错位；首个头块前发一次
 *     动态表容量更新（0），与新连接的解码器认知保持一致。
 *   - 生命周期：会话与 stream 都用引用计数；工作线程持有一份引用，因此
 *     会话主循环退出时可以立即关闭 fd 并返回，无需 join。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nl_px_h2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../http/netleaf_http_internal.h"

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <process.h>
    #include <windows.h>
    typedef SOCKET nl_h2fd;
    #define NL_H2_INVALID   INVALID_SOCKET
    #define nl_h2_closefd   closesocket
    typedef int nl_h2_socklen_t;
    #define nl_h2_errno()   ((int)WSAGetLastError())
    #define NL_H2_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
    typedef CRITICAL_SECTION nl_h2_mu_t;
    typedef CONDITION_VARIABLE nl_h2_cv_t;
    typedef unsigned (__stdcall *nl_h2_thread_fn)(void*);
#else
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <pthread.h>
    #include <time.h>
    typedef int nl_h2fd;
    #define NL_H2_INVALID   (-1)
    #define nl_h2_closefd   close
    typedef socklen_t nl_h2_socklen_t;
    #define nl_h2_errno()   (errno)
    #define NL_H2_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)
    typedef pthread_mutex_t nl_h2_mu_t;
    typedef pthread_cond_t  nl_h2_cv_t;
    typedef void* (*nl_h2_thread_fn)(void*);
#endif

#define NL_H2_MAX_FRAME_RECV (1u << 20)   /* 单帧长度上限（防御恶意长度） */
#define NL_H2_MAX_BODY       (64u << 20)  /* 单请求/响应体上限 */
#define NL_H2_MAX_HDR        (64u << 10)  /* 请求/响应头行上限 */
#define NL_H2_MAX_BLOCK      (128u << 10) /* 头块（含 CONTINUATION）上限 */
#define NL_H2_SEND_TIMEOUT   30000
#define NL_H2_DEFAULT_IDLE   60000
/* 单会话并发的本机回放连接上限：一条 stream 对应一条本机 HTTP/1.1 连接，
 * 不设上限时恶意客户端可用大量并行 stream 把本机 fd / 线程数打爆。
 * 超出后进入排队，由完成的工作线程按序接棒。 */
#define NL_H2_REPLAY_MAX     32

#define H2_TYPE_DATA          0x0
#define H2_TYPE_HEADERS       0x1
#define H2_TYPE_PRIORITY      0x2
#define H2_TYPE_RST_STREAM    0x3
#define H2_TYPE_SETTINGS      0x4
#define H2_TYPE_PUSH_PROMISE  0x5
#define H2_TYPE_PING          0x6
#define H2_TYPE_GOAWAY        0x7
#define H2_TYPE_WINDOW_UPDATE 0x8
#define H2_TYPE_CONTINUATION  0x9

#define H2_FLAG_END_STREAM    0x1
#define H2_FLAG_ACK           0x1
#define H2_FLAG_END_HEADERS   0x4
#define H2_FLAG_PADDED        0x8
#define H2_FLAG_PRIORITY      0x20

#define H2_SETTINGS_HEADER_TABLE_SIZE      0x1
#define H2_SETTINGS_INITIAL_WINDOW_SIZE    0x4
#define H2_SETTINGS_MAX_FRAME_SIZE         0x5

#define H2_ERR_PROTOCOL        0x1
#define H2_ERR_FLOW_CONTROL    0x3
#define H2_ERR_FRAME_SIZE      0x6
#define H2_ERR_REFUSED_STREAM  0x7
#define H2_ERR_CANCEL          0x8
#define H2_ERR_INTERNAL        0x2

/* ============================================================
 * 锁 / 条件变量 / 线程 / 网络等待
 * ============================================================ */
static void nl_h2_mu_init(nl_h2_mu_t* m) {
#if defined(_WIN32)
    InitializeCriticalSection(m);
#else
    pthread_mutex_init(m, NULL);
#endif
}
static void nl_h2_mu_destroy(nl_h2_mu_t* m) {
#if defined(_WIN32)
    DeleteCriticalSection(m);
#else
    pthread_mutex_destroy(m);
#endif
}
static void nl_h2_mu_lock(nl_h2_mu_t* m) {
#if defined(_WIN32)
    EnterCriticalSection(m);
#else
    pthread_mutex_lock(m);
#endif
}
static void nl_h2_mu_unlock(nl_h2_mu_t* m) {
#if defined(_WIN32)
    LeaveCriticalSection(m);
#else
    pthread_mutex_unlock(m);
#endif
}
static void nl_h2_cv_init(nl_h2_cv_t* c) {
#if defined(_WIN32)
    InitializeConditionVariable(c);
#else
    pthread_cond_init(c, NULL);
#endif
}
static void nl_h2_cv_destroy(nl_h2_cv_t* c) {
#if defined(_WIN32)
    (void)c;
#else
    pthread_cond_destroy(c);
#endif
}
/* 等待（毫秒），返回时已重新持有 m */
static void nl_h2_cv_wait(nl_h2_cv_t* c, nl_h2_mu_t* m, int ms) {
#if defined(_WIN32)
    SleepConditionVariableCS(c, m, (DWORD)(ms > 0 ? ms : 1));
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    pthread_cond_timedwait(c, m, &ts);
#endif
}
static void nl_h2_cv_broadcast(nl_h2_cv_t* c) {
#if defined(_WIN32)
    WakeAllConditionVariable(c);
#else
    pthread_cond_broadcast(c);
#endif
}
static int nl_h2_thread_start(nl_h2_thread_fn fn, void* arg) {
#if defined(_WIN32)
    uintptr_t h = _beginthreadex(NULL, 0, fn, arg, 0, NULL);
    if (!h) return -1;
    CloseHandle((HANDLE)h);
    return 0;
#else
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return -1;
    pthread_detach(t);
    return 0;
#endif
}

/* 等待 fd 就绪：1 = 就绪，0 = 超时/错误 */
static int nl_h2_wait_fd(intptr_t fd, int want_read, int want_write, int timeout_ms) {
#if defined(_WIN32)
    nl_h2fd f = (nl_h2fd)fd;
    fd_set rs, ws;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    if (want_read)  FD_SET(f, &rs);
    if (want_write) FD_SET(f, &ws);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(0, want_read ? &rs : NULL, want_write ? &ws : NULL, NULL, &tv);
    return r > 0 ? 1 : 0;
#else
    struct pollfd p;
    p.fd = (int)fd;
    p.events = (short)((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0));
    p.revents = 0;
    int r = poll(&p, 1, timeout_ms);
    return r > 0 ? 1 : 0;
#endif
}

/* 完整写出（阻塞语义，写慢时等可写） */
static int nl_h2_send_fd(intptr_t fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        int w = (int)send((nl_h2fd)fd, p, (int)(len > 1u << 20 ? 1u << 20 : len), 0);
        if (w > 0) { p += w; len -= (size_t)w; continue; }
        if (w < 0 && NL_H2_WOULDBLOCK(nl_h2_errno())) {
            if (!nl_h2_wait_fd(fd, 0, 1, NL_H2_SEND_TIMEOUT)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* 动态缓冲追加（NUL 结尾，便于按行解析） */
static int nl_h2_buf_append(char** buf, size_t* len, size_t* cap,
                            const void* p, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap : 256;
        while (nc < *len + n + 1) nc *= 2;
        char* nb = (char*)realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb;
        *cap = nc;
    }
    if (n) memcpy(*buf + *len, p, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 0;
}

static int nl_h2_ci_eq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* 逐行遍历 "name: value\r\n"：返回 1 = 取到一行，0 = 结束 */
static int nl_h2_next_header(const char* p, size_t len, size_t* off,
                             char* name, size_t ncap, char* value, size_t vcap) {
    size_t i = *off;
    while (i + 1 < len && !(p[i] == '\r' && p[i + 1] == '\n')) i++;
    if (i + 1 >= len) return 0;
    size_t start = *off, end = i;
    *off = i + 2;
    size_t colon = start;
    while (colon < end && p[colon] != ':') colon++;
    if (colon >= end) return -1;
    size_t n = colon - start;
    if (n >= ncap) n = ncap - 1;
    memcpy(name, p + start, n);
    name[n] = '\0';
    size_t v = colon + 1;
    while (v < end && (p[v] == ' ' || p[v] == '\t')) v++;
    size_t vn = end - v;
    if (vn >= vcap) vn = vcap - 1;
    memcpy(value, p + v, vn);
    value[vn] = '\0';
    return 1;
}

/* 请求侧需要丢弃的逐跳头（Host 由 :authority 重新生成） */
static int nl_h2_skip_req_header(const char* name) {
    static const char* const list[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding",
        "upgrade", "http2-settings", "te", "host", "expect", NULL
    };
    for (int i = 0; list[i]; i++) if (nl_h2_ci_eq(name, list[i])) return 1;
    return 0;
}

/* 响应侧需要丢弃的逐跳头（H2 由帧边界表达长度） */
static int nl_h2_skip_resp_header(const char* name) {
    static const char* const list[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding",
        "upgrade", "content-length", "trailer", NULL
    };
    for (int i = 0; list[i]; i++) if (nl_h2_ci_eq(name, list[i])) return 1;
    return 0;
}

/* ============================================================
 * 首包判定
 * ============================================================ */
/* 尺寸 +1 容纳字符串终止符：字面量共 25B，若声明为 24 会截掉 '\0'
 * 触发 -Wunterminated-string-initialization；所有比较仍按前 24B 进行 */
static const char nl_h2_magic[NL_PX_H2_PREFACE_LEN + 1] =
    "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

int nl_px_h2_preface_check(const char* data, size_t len) {
    if (!data || len == 0) return 1;
    size_t k = len < NL_PX_H2_PREFACE_LEN ? len : NL_PX_H2_PREFACE_LEN;
    if (memcmp(data, nl_h2_magic, k) != 0) return 0;
    return len >= NL_PX_H2_PREFACE_LEN ? 2 : 1;
}

int nl_px_h2_upgrade_check(const char* headers, size_t len) {
    if (!headers || len == 0) return 0;
    size_t off = 0;
    char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
    while (off < len) {
        int r = nl_h2_next_header(headers, len, &off, name, sizeof(name),
                                  value, sizeof(value));
        if (r <= 0) break;
        if (nl_h2_ci_eq(name, "upgrade")) {
            /* 值可能是逗号分隔列表，逐项忽略大小写比较 */
            const char* p = value;
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == ',') p++;
                const char* q = p;
                while (*q && *q != ',' && *q != ' ' && *q != '\t') q++;
                size_t n = (size_t)(q - p);
                if (n == 3 && (p[0] == 'h' || p[0] == 'H') &&
                    (p[1] == '2' || p[1] == '2') &&
                    (p[2] == 'c' || p[2] == 'C'))
                    return 1;
                p = q;
            }
        }
    }
    return 0;
}

/* ============================================================
 * 会话 / 流结构
 * ============================================================ */
typedef struct nl_px_h2sess nl_px_h2sess;

typedef struct nl_px_h2st {
    nl_px_h2sess* sess;
    uint32_t id;
    int refs;
    int send_window;       /* 本端发送窗口（受对端 WINDOW_UPDATE 控制） */
    int started;           /* 工作线程已启动 */
    int reset;             /* 收到 RST_STREAM */
    int is_head;
    char method[24];
    char path[4096];
    char authority[512];
    char* hdr;             /* "name: value\r\n" 串 */
    size_t hdr_len, hdr_cap;
    char* body;
    size_t body_len, body_cap;
    struct nl_px_h2st* next;
    struct nl_px_h2st* qnext;   /* 回放排队链表 */
} nl_px_h2st;

struct nl_px_h2sess {
    intptr_t fd;
    int local_port;
    struct sockaddr_storage laddr;  /* 客户端连接的本地地址（回放目标） */
    char peer_ip[64];               /* 客户端 IP 文本（X-Forwarded-For） */
    void* local_ctx;                /* 进程内路由分发上下文 */
    const nl_px_local_ops* local_ops;
    int idle_ms;
    int conn_window;
    int stream_window_init;
    int max_frame;         /* 对端允许的最大帧（本端发送上限） */
    int closed;
    int refs;
    int workers;
    int goaway;
    uint32_t last_sid;
    nl_h2_mu_t mu;         /* 状态锁 */
    nl_h2_cv_t cv;         /* 窗口/关闭通知 */
    nl_h2_mu_t wmu;        /* 帧写入串行化 */
    struct hpack_context dec;
    struct hpack_context enc;
    nl_px_h2st* streams;
    /* 回放排队（workers 达到 NL_H2_REPLAY_MAX 时暂存待处理 stream） */
    nl_px_h2st* ready;
    nl_px_h2st* ready_tail;
    /* CONTINUATION 累积 */
    char* pend;
    size_t pend_len, pend_cap;
    uint32_t pend_sid;
    int pend_end_stream;
    /* 首包余量 + 复用帧缓冲 */
    char* in;
    size_t in_len, in_off;
    uint8_t* fr;
    size_t fr_cap;
};

static void nl_h2sess_unref(nl_px_h2sess* s);
static void nl_h2st_unref(nl_px_h2st* st);

static void nl_h2st_unref(nl_px_h2st* st) {
    nl_px_h2sess* s = st->sess;
    int dead = 0;
    nl_h2_mu_lock(&s->mu);
    if (--st->refs <= 0) dead = 1;
    nl_h2_mu_unlock(&s->mu);
    if (dead) {
        free(st->hdr);
        free(st->body);
        free(st);
    }
}

static void nl_h2sess_unref(nl_px_h2sess* s) {
    int dead = 0;
    nl_h2_mu_lock(&s->mu);
    if (--s->refs <= 0) dead = 1;
    nl_h2_mu_unlock(&s->mu);
    if (dead) {
        hpack_free(&s->dec);
        hpack_free(&s->enc);
        free(s->pend);
        free(s->in);
        free(s->fr);
        nl_h2_cv_destroy(&s->cv);
        nl_h2_mu_destroy(&s->mu);
        nl_h2_mu_destroy(&s->wmu);
        free(s);
    }
}

/* 查找/创建 stream（调用方需持有状态锁；创建时 refs=1 归属会话链表） */
static nl_px_h2st* nl_h2s_get_stream(nl_px_h2sess* s, uint32_t sid, int create) {
    for (nl_px_h2st* it = s->streams; it; it = it->next)
        if (it->id == sid) return it;
    if (!create) return NULL;
    nl_px_h2st* st = (nl_px_h2st*)calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->sess = s;
    st->id = sid;
    st->refs = 1;
    st->send_window = s->stream_window_init;
    st->next = s->streams;
    s->streams = st;
    return st;
}

static int nl_h2s_write_frame(nl_px_h2sess* s, uint8_t type, uint8_t flags,
                              uint32_t sid, const void* payload, size_t len) {
    uint8_t hdr[9];
    struct nl_h2_frame_header h;
    h.length = (uint32_t)len;
    h.type = type;
    h.flags = flags;
    h.stream_id = sid & 0x7FFFFFFFu;
    h2_frame_header_write(hdr, &h);

    nl_h2_mu_lock(&s->wmu);
    int rc = nl_h2_send_fd(s->fd, hdr, 9);
    if (rc == 0 && len > 0) rc = nl_h2_send_fd(s->fd, payload, len);
    nl_h2_mu_unlock(&s->wmu);
    if (rc != 0) {
        nl_h2_mu_lock(&s->mu);
        s->closed = 1;
        nl_h2_mu_unlock(&s->mu);
        nl_h2_cv_broadcast(&s->cv);
    }
    return rc;
}

static void nl_h2s_send_window_update(nl_px_h2sess* s, uint32_t sid, uint32_t inc) {
    if (inc == 0) return;
    uint8_t p[4] = { (uint8_t)(inc >> 24), (uint8_t)(inc >> 16),
                     (uint8_t)(inc >> 8), (uint8_t)inc };
    nl_h2s_write_frame(s, H2_TYPE_WINDOW_UPDATE, 0, sid, p, 4);
}

static void nl_h2s_send_rst(nl_px_h2sess* s, uint32_t sid, uint32_t code) {
    uint8_t p[4] = { (uint8_t)(code >> 24), (uint8_t)(code >> 16),
                     (uint8_t)(code >> 8), (uint8_t)code };
    nl_h2s_write_frame(s, H2_TYPE_RST_STREAM, 0, sid, p, 4);
}

/* ============================================================
 * HTTP/1.1 回放（每 stream 一条本机连接）
 * ============================================================ */
/* 回放目标：优先用监听套接字的真实本地地址（getsockname 得到），
 * 未指定地址（0.0.0.0 / ::）时回退 127.0.0.1 —— 服务绑定到具体网卡时
 * 也能自连成功（之前硬编码回环会让非回环监听场景全部失败）。 */
static intptr_t nl_h2_connect_local(int port, int idle_ms,
                                    const struct sockaddr_storage* laddr) {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    int use = 0;
    if (laddr) {
        if (laddr->ss_family == AF_INET) {
            const struct sockaddr_in* v4 = (const struct sockaddr_in*)laddr;
            if (v4->sin_addr.s_addr != htonl(INADDR_ANY)) {
                ss = *laddr;
                use = 1;
            }
        } else if (laddr->ss_family == AF_INET6) {
            const struct sockaddr_in6* v6 = (const struct sockaddr_in6*)laddr;
            if (!IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr)) {
                ss = *laddr;
                use = 1;
            }
        }
    }
    if (!use) {
        struct sockaddr_in* v4 = (struct sockaddr_in*)&ss;
        v4->sin_family = AF_INET;
        v4->sin_addr.s_addr = htonl(0x7F000001u);
    }
    if (ss.ss_family == AF_INET) {
        ((struct sockaddr_in*)&ss)->sin_port = htons((unsigned short)port);
    } else {
        ((struct sockaddr_in6*)&ss)->sin6_port = htons((unsigned short)port);
    }

    nl_h2fd fd = socket(ss.ss_family, SOCK_STREAM, 0);
    if (fd == NL_H2_INVALID) return -1;
    if (connect(fd, (struct sockaddr*)&ss, (int)sizeof(ss)) != 0) {
        nl_h2_closefd(fd);
        return -1;
    }
#if defined(_WIN32)
    DWORD tv = (DWORD)(idle_ms > 0 ? idle_ms : NL_H2_DEFAULT_IDLE);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = (idle_ms > 0 ? idle_ms : NL_H2_DEFAULT_IDLE) / 1000;
    tv.tv_usec = ((idle_ms > 0 ? idle_ms : NL_H2_DEFAULT_IDLE) % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    return (intptr_t)fd;
}

/* 构造回放请求（HTTP/1.1 + Connection: close） */
static int nl_h2_build_replay(const nl_px_h2st* st, int port,
                              char** out, size_t* out_len) {
    size_t cap = st->hdr_len + st->body_len + 1024;
    char* b = (char*)malloc(cap);
    if (!b) return -1;
    const char* method = st->method[0] ? st->method : "GET";
    const char* path = st->path[0] ? st->path : "/";
    char host[560];
    if (st->authority[0]) snprintf(host, sizeof(host), "%s", st->authority);
    else snprintf(host, sizeof(host), "127.0.0.1:%d", port);

    int n = snprintf(b, cap, "%s %s HTTP/1.1\r\nHost: %s\r\n", method, path, host);
    if (n <= 0 || (size_t)n >= cap) { free(b); return -1; }
    if (st->hdr_len) {
        memcpy(b + n, st->hdr, st->hdr_len);
        n += (int)st->hdr_len;
    }
    if (st->body_len > 0 && !strstr(st->hdr ? st->hdr : "", "content-length:")) {
        char cl[64];
        int m = snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", st->body_len);
        if (m <= 0 || (size_t)(n + m) >= cap) { free(b); return -1; }
        memcpy(b + n, cl, (size_t)m);
        n += m;
    }
    const char tail[] = "Connection: close\r\n\r\n";
    if ((size_t)(n + (int)sizeof(tail)) >= cap) { free(b); return -1; }
    memcpy(b + n, tail, sizeof(tail) - 1);
    n += (int)(sizeof(tail) - 1);
    if (st->body_len) {
        memcpy(b + n, st->body, st->body_len);
        n += (int)st->body_len;
    }
    *out = b;
    *out_len = (size_t)n;
    return 0;
}

/* chunked 完整性判定：1 = 完整，0 = 还需数据，-1 = 格式错误 */
static int nl_h2_chunk_complete(const char* d, size_t n) {
    size_t off = 0;
    for (;;) {
        size_t i = off;
        while (i + 1 < n && !(d[i] == '\r' && d[i + 1] == '\n')) i++;
        if (i + 1 >= n) return 0;
        unsigned long long sz = 0;
        int any = 0;
        for (size_t k = off; k < i; k++) {
            char c = d[k];
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            sz = sz * 16 + (unsigned)v;
            any = 1;
            if (sz > (1ull << 40)) return -1;
        }
        if (!any) return -1;
        off = i + 2;
        if (sz == 0) {
            /* trailer 段直到空行 */
            size_t j = off;
            for (;;) {
                if (j + 1 >= n) return 0;
                if (d[j] == '\r' && d[j + 1] == '\n') return 1;
                while (j + 1 < n && !(d[j] == '\r' && d[j + 1] == '\n')) j++;
                if (j + 1 >= n) return 0;
                j += 2;
            }
        }
        if (off + (size_t)sz > n) return 0;
        off += (size_t)sz;
        if (off + 2 > n) return 0;
        if (!(d[off] == '\r' && d[off + 1] == '\n')) return -1;
        off += 2;
    }
}

static char* nl_h2_dechunk(const char* d, size_t n, size_t* out_len) {
    char* out = (char*)malloc(n ? n : 1);
    if (!out) return NULL;
    size_t off = 0, w = 0;
    for (;;) {
        size_t i = off;
        while (i + 1 < n && !(d[i] == '\r' && d[i + 1] == '\n')) i++;
        if (i + 1 >= n) break;
        unsigned long long sz = 0;
        int any = 0;
        for (size_t k = off; k < i; k++) {
            char c = d[k];
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else break;
            sz = sz * 16 + (unsigned)v;
            any = 1;
        }
        if (!any) break;
        off = i + 2;
        if (sz == 0) break;
        if (off + (size_t)sz > n) break;
        memcpy(out + w, d + off, (size_t)sz);
        w += (size_t)sz;
        off += (size_t)sz;
        if (off + 2 <= n) off += 2;
    }
    *out_len = w;
    return out;
}

/*
 * 读一个 HTTP/1.1 响应（含 framing），响应头逐行过滤并写入 out_hdrs。
 * 返回 0 成功，-1 失败。
 */
static int nl_h2_read_http1(intptr_t fd, int idle_ms, int is_head,
                            int* out_status, char** out_hdrs, size_t* out_hlen,
                            char** out_body, size_t* out_blen) {
    size_t cap = 8192, len = 0;
    char* buf = (char*)malloc(cap + 1);
    if (!buf) return -1;

    int status = 0;
    char* hdrs = NULL;
    size_t hlen = 0, hcap = 0;
    long long cl = -1;
    int chunked = 0, parsed = 0, eof = 0;
    size_t body_off = 0;
    int rc = -1;

    for (;;) {
        if (!parsed) {
            /* 跳过 1xx 临时响应（101 不会出现在本机回放里） */
            size_t i = 0;
            int found = -1;
            for (; i + 3 < len; i++) {
                if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    found = (int)i;
                    break;
                }
            }
            if (found >= 0) {
                size_t hdr_end = (size_t)found;
                /* 状态行 */
                char* sp = (char*)memchr(buf, ' ', hdr_end);
                status = (sp && sp + 1 < buf + hdr_end) ? atoi(sp + 1) : 0;
                if (status >= 100 && status < 200 && status != 101) {
                    size_t skip = hdr_end + 4;
                    memmove(buf, buf + skip, len - skip);
                    len -= skip;
                    buf[len] = '\0';
                    continue;
                }
                /* 解析头行 */
                char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
                size_t off = 0;
                char* nl = (char*)memchr(buf, '\n', hdr_end);
                if (!nl) goto done;
                off = (size_t)(nl - buf) + 1;
                while (off < hdr_end) {
                    int r = nl_h2_next_header(buf, hdr_end + 2, &off,
                                              name, sizeof(name), value, sizeof(value));
                    if (r <= 0) break;
                    if (nl_h2_ci_eq(name, "content-length")) cl = atoll(value);
                    else if (nl_h2_ci_eq(name, "transfer-encoding") &&
                             strstr(value, "chunked")) chunked = 1;
                    if (nl_h2_skip_resp_header(name)) continue;
                    char line[MAX_HEADER_NAME + MAX_HEADER_VALUE + 4];
                    int ln = snprintf(line, sizeof(line), "%s: %s\r\n", name, value);
                    if (ln > 0 && nl_h2_buf_append(&hdrs, &hlen, &hcap, line, (size_t)ln) != 0)
                        goto done;
                }
                if (chunked) cl = -1;
                body_off = hdr_end + 4;
                parsed = 1;
            }
        }

        if (parsed) {
            if (is_head || status == 204 || status == 304) break;
            if (chunked) {
                int c = nl_h2_chunk_complete(buf + body_off, len - body_off);
                if (c == 1) break;
                if (c < 0) break;
            } else if (cl >= 0) {
                if ((long long)(len - body_off) >= cl) break;
            } else if (eof) {
                break;
            }
        }
        if (eof) break;

        if (len + 8192 + 1 > cap) {
            if (cap >= NL_H2_MAX_BODY) goto done;
            size_t nc = cap * 2;
            char* nb = (char*)realloc(buf, nc + 1);
            if (!nb) goto done;
            buf = nb;
            cap = nc;
        }
        if (!nl_h2_wait_fd(fd, 1, 0, idle_ms)) goto done;
        int r = (int)recv((nl_h2fd)fd, buf + len, (int)(cap - len), 0);
        if (r > 0) { len += (size_t)r; buf[len] = '\0'; continue; }
        if (r == 0) { eof = 1; continue; }
        if (NL_H2_WOULDBLOCK(nl_h2_errno())) continue;
        goto done;
    }

    /* 提取 body */
    {
        size_t bl = len > body_off ? len - body_off : 0;
        char* body = NULL;
        size_t blen = 0;
        if (is_head || status == 204 || status == 304) {
            body = (char*)malloc(1);
            blen = 0;
        } else if (chunked) {
            body = nl_h2_dechunk(buf + body_off, bl, &blen);
        } else {
            size_t take = (cl >= 0 && (size_t)cl < bl) ? (size_t)cl : bl;
            body = (char*)malloc(take ? take : 1);
            if (body && take) memcpy(body, buf + body_off, take);
            blen = take;
        }
        if (!body) goto done;
        *out_status = status;
        *out_hdrs = hdrs;
        *out_hlen = hlen;
        *out_body = body;
        *out_blen = blen;
        rc = 0;
    }

done:
    if (rc != 0) free(hdrs);
    free(buf);
    return rc;
}

/* ============================================================
 * stream 工作线程：回放 + 回写 H2 响应
 * ============================================================ */
static void nl_h2s_spawn(nl_px_h2sess* s, nl_px_h2st* st);
#if defined(_WIN32)
static unsigned __stdcall nl_h2s_worker(void* arg);
#else
static void* nl_h2s_worker(void* arg);
#endif

static int nl_h2s_send_response(nl_px_h2sess* s, nl_px_h2st* st,
                                int status, const char* hdrs, size_t hlen,
                                const char* body, size_t blen) {
    uint8_t* block = (uint8_t*)malloc(NL_H2_MAX_HDR);
    if (!block) return -1;

    size_t off = 0;
    char code[16];
    snprintf(code, sizeof(code), "%d", status > 0 ? status : 502);
    int n;
    /* 编码端不使用动态索引（enc.max_dynamic_table_size = 0），无需发送
     * HPACK 动态表容量更新（0x20）；部分解码器不识别该表示，会整块错位。 */
    n = hpack_encode_header(&s->enc, block + off, NL_H2_MAX_HDR - off, ":status", code);
    if (n <= 0) { free(block); return -1; }
    off += (size_t)n;

    size_t p = 0;
    char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
    while (p < hlen) {
        int r = nl_h2_next_header(hdrs, hlen, &p, name, sizeof(name), value, sizeof(value));
        if (r <= 0) break;
        if (nl_h2_skip_resp_header(name)) continue;
        n = hpack_encode_header(&s->enc, block + off, NL_H2_MAX_HDR - off, name, value);
        if (n <= 0) continue;
        off += (size_t)n;
    }

    int end = (blen == 0 || st->is_head) ? H2_FLAG_END_STREAM : 0;
    if (nl_h2s_write_frame(s, H2_TYPE_HEADERS,
                           (uint8_t)(H2_FLAG_END_HEADERS | end), st->id,
                           block, off) != 0) {
        free(block);
        return -1;
    }
    free(block);
    if (end) return 0;

    size_t sent = 0;
    while (sent < blen) {
        int want = (int)(blen - sent);
        int cap = (int)(s->max_frame > 0 ? s->max_frame : 16384);
        if (want > cap) want = cap;

        int take = 0;
        nl_h2_mu_lock(&s->mu);
        for (;;) {
            if (s->closed || st->reset) { nl_h2_mu_unlock(&s->mu); return -1; }
            int avail = s->conn_window < st->send_window ? s->conn_window : st->send_window;
            if (avail > 0) {
                take = want < avail ? want : avail;
                s->conn_window -= take;
                st->send_window -= take;
                break;
            }
            nl_h2_cv_wait(&s->cv, &s->mu, 500);
        }
        nl_h2_mu_unlock(&s->mu);

        int last = (sent + (size_t)take >= blen) ? H2_FLAG_END_STREAM : 0;
        if (nl_h2s_write_frame(s, H2_TYPE_DATA, (uint8_t)last, st->id,
                               body + sent, (size_t)take) != 0)
            return -1;
        sent += (size_t)take;
    }
    return 0;
}

static void nl_h2s_worker_finish(nl_px_h2st* st) {
    nl_px_h2sess* s = st->sess;
    nl_px_h2st* nxt = NULL;
    nl_h2_mu_lock(&s->mu);
    s->workers--;
    if (s->ready && !s->closed) {
        nxt = s->ready;
        s->ready = nxt->qnext;
        if (!s->ready) s->ready_tail = NULL;
        nxt->qnext = NULL;
        nxt->refs++;
        s->refs++;
        s->workers++;
    }
    nl_h2_mu_unlock(&s->mu);
    nl_h2st_unref(st);
    nl_h2sess_unref(s);

    if (nxt && nl_h2_thread_start(nl_h2s_worker, nxt) != 0) {
        nl_h2_mu_lock(&s->mu);
        s->workers--;
        s->refs--;
        nl_h2_mu_unlock(&s->mu);
        nl_h2st_unref(nxt);
        nl_h2s_send_rst(s, nxt->id, H2_ERR_INTERNAL);
        nl_h2sess_unref(s);
    }
}

static void* nl_h2s_worker_body(nl_px_h2st* st) {
    nl_px_h2sess* s = st->sess;
    nl_h2_mu_lock(&s->mu);
    int dead = s->closed || st->reset;
    nl_h2_mu_unlock(&s->mu);
    if (dead) return NULL;   /* 会话/流已废弃：不必再占用本机连接 */

    /* 进程内分发优先：静态/重定向/HTTP 反代直接产出响应，免去本机回放一跳。
     * HTTPS/TCP 上游返回 FALLBACK，继续走下面的回放路径。 */
    if (s->local_ops) {
        int status = 0;
        char* hdrs = NULL;
        size_t hlen = 0;
        char* body = NULL;
        size_t blen = 0;
        int dr = nl_px_local_dispatch(s->local_ctx, s->local_ops,
                                      st->method[0] ? st->method : "GET",
                                      st->path[0] ? st->path : "/",
                                      st->authority,
                                      st->hdr ? st->hdr : "", st->hdr_len,
                                      st->body, st->body_len,
                                      s->peer_ip[0] ? s->peer_ip : NULL,
                                      st->is_head, s->idle_ms,
                                      &status, &hdrs, &hlen, &body, &blen);
        if (dr == NL_PX_LOCAL_OK) {
            (void)nl_h2s_send_response(s, st, status, hdrs ? hdrs : "", hlen,
                                       body ? body : "", blen);
            free(hdrs);
            free(body);
            return NULL;
        }
        free(hdrs);
        free(body);
        /* dr == NL_PX_LOCAL_FALLBACK / ERROR：回退本机回放 */
    }

    intptr_t fd = nl_h2_connect_local(s->local_port, s->idle_ms, &s->laddr);
    if (fd < 0) {
        nl_h2s_send_rst(s, st->id, H2_ERR_INTERNAL);
        return NULL;
    }
    char* req = NULL;
    size_t req_len = 0;
    if (nl_h2_build_replay(st, s->local_port, &req, &req_len) != 0) {
        nl_h2_closefd((nl_h2fd)fd);
        nl_h2s_send_rst(s, st->id, H2_ERR_INTERNAL);
        return NULL;
    }
    if (nl_h2_send_fd(fd, req, req_len) != 0) {
        free(req);
        nl_h2_closefd((nl_h2fd)fd);
        nl_h2s_send_rst(s, st->id, H2_ERR_INTERNAL);
        return NULL;
    }
    free(req);

    int status = 0;
    char* hdrs = NULL;
    size_t hlen = 0;
    char* body = NULL;
    size_t blen = 0;
    int ok = nl_h2_read_http1(fd, s->idle_ms, st->is_head,
                              &status, &hdrs, &hlen, &body, &blen);
    nl_h2_closefd((nl_h2fd)fd);

    if (ok == 0) {
        /* 客户端已断开时不必再写；send_response 内部会检测 closed/reset */
        (void)nl_h2s_send_response(s, st, status, hdrs ? hdrs : "", hlen, body, blen);
        free(hdrs);
        free(body);
    } else {
        nl_h2s_send_rst(s, st->id, H2_ERR_INTERNAL);
    }
    return NULL;
}

#if defined(_WIN32)
static unsigned __stdcall nl_h2s_worker(void* arg)
#else
static void* nl_h2s_worker(void* arg)
#endif
{
    nl_px_h2st* st = (nl_px_h2st*)arg;
    nl_h2s_worker_body(st);
    nl_h2s_worker_finish(st);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void nl_h2s_spawn(nl_px_h2sess* s, nl_px_h2st* st) {
    nl_h2_mu_lock(&s->mu);
    if (st->started || s->closed) {
        nl_h2_mu_unlock(&s->mu);
        return;
    }
    st->started = 1;
    if (s->workers >= NL_H2_REPLAY_MAX) {
        /* 并发已达上限：排队，等某个工作线程结束时接棒 */
        st->qnext = NULL;
        if (s->ready_tail) s->ready_tail->qnext = st;
        else s->ready = st;
        s->ready_tail = st;
        nl_h2_mu_unlock(&s->mu);
        return;
    }
    st->refs++;
    s->refs++;
    s->workers++;
    nl_h2_mu_unlock(&s->mu);

    if (nl_h2_thread_start(nl_h2s_worker, st) != 0) {
        nl_h2_mu_lock(&s->mu);
        s->workers--;
        s->refs--;
        st->started = 0;
        nl_h2_mu_unlock(&s->mu);
        nl_h2st_unref(st);
        nl_h2s_send_rst(s, st->id, H2_ERR_INTERNAL);
    }
}

/* ============================================================
 * HPACK 头块解码（请求侧）
 * ============================================================ */
static int nl_h2s_decode_request(nl_px_h2sess* s, uint32_t sid,
                                 const void* block_v, size_t len,
                                 int end_stream) {
    const uint8_t* block = (const uint8_t*)block_v;
    nl_h2_mu_lock(&s->mu);
    nl_px_h2st* st = nl_h2s_get_stream(s, sid, 1);
    nl_h2_mu_unlock(&s->mu);
    if (!st) return -1;

    /* 已启动的流再收到 HEADERS 视为 trailer：仍需解码以同步 HPACK 表 */
    int disc = st->started ? 1 : 0;

    size_t off = 0;
    char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
    while (off < len) {
        size_t consumed = 0;
        if (hpack_decode_header(&s->dec, block + off, len - off,
                                name, value, &consumed) != 0)
            return -1;
        if (consumed == 0) return -1;
        off += consumed;
        if (name[0] == '\0') continue;   /* 动态表容量更新，无头字段 */
        if (disc) continue;
        if (name[0] == ':') {
            /* 有界拷贝（非 snprintf）：value 源 4096B 可能长于目标，截断语义
             * 不变，且不触发 -Wformat-truncation */
            #define NL_H2_COPY(dst) do { size_t _n = strnlen(value, sizeof(dst) - 1); \
                memcpy(dst, value, _n); dst[_n] = '\0'; } while (0)
            if (nl_h2_ci_eq(name, ":method")) NL_H2_COPY(st->method);
            else if (nl_h2_ci_eq(name, ":path")) NL_H2_COPY(st->path);
            else if (nl_h2_ci_eq(name, ":authority")) NL_H2_COPY(st->authority);
            #undef NL_H2_COPY
            continue;
        }
        if (nl_h2_skip_req_header(name)) continue;
        char line[MAX_HEADER_NAME + MAX_HEADER_VALUE + 4];
        int ln = snprintf(line, sizeof(line), "%s: %s\r\n", name, value);
        if (ln > 0 && nl_h2_buf_append(&st->hdr, &st->hdr_len, &st->hdr_cap,
                                       line, (size_t)ln) != 0)
            return -1;
        if (st->hdr_len > NL_H2_MAX_HDR) return -1;
    }
    if (st->method[0] && nl_h2_ci_eq(st->method, "HEAD")) st->is_head = 1;

    if (end_stream) nl_h2s_spawn(s, st);
    return 0;
}

/* ============================================================
 * 帧分发（h2c 下游）
 * ============================================================ */
static int nl_h2s_apply_settings(nl_px_h2sess* s, const uint8_t* p, size_t n) {
    if (n % 6 != 0) return -1;
    nl_h2_mu_lock(&s->mu);
    for (size_t i = 0; i + 6 <= n; i += 6) {
        uint16_t id = (uint16_t)(((uint16_t)p[i] << 8) | p[i + 1]);
        uint32_t v = ((uint32_t)p[i + 2] << 24) | ((uint32_t)p[i + 3] << 16) |
                     ((uint32_t)p[i + 4] << 8) | (uint32_t)p[i + 5];
        if (id == H2_SETTINGS_INITIAL_WINDOW_SIZE) {
            int nv = (int)(v & 0x7FFFFFFFu);
            int delta = nv - s->stream_window_init;
            s->stream_window_init = nv;
            for (nl_px_h2st* it = s->streams; it; it = it->next) it->send_window += delta;
        } else if (id == H2_SETTINGS_MAX_FRAME_SIZE) {
            if (v >= 16384 && v <= 16777215) s->max_frame = (int)v;
        }
        /* HEADER_TABLE_SIZE 等：本端编码不使用动态索引，无需处理 */
    }
    nl_h2_mu_unlock(&s->mu);
    nl_h2_cv_broadcast(&s->cv);
    return 0;
}

static int nl_h2s_handle_frame(nl_px_h2sess* s, const struct nl_h2_frame_header* h,
                               const uint8_t* p, size_t n) {
    uint32_t sid = h->stream_id & 0x7FFFFFFFu;

    switch (h->type) {
    case H2_TYPE_SETTINGS:
        if (sid != 0) return -1;
        if (h->flags & H2_FLAG_ACK) return 0;
        if (nl_h2s_apply_settings(s, p, n) != 0) return -1;
        nl_h2s_write_frame(s, H2_TYPE_SETTINGS, H2_FLAG_ACK, 0, NULL, 0);
        return 0;

    case H2_TYPE_PING:
        if (sid != 0 || n != 8) return -1;
        if (!(h->flags & H2_FLAG_ACK))
            nl_h2s_write_frame(s, H2_TYPE_PING, H2_FLAG_ACK, 0, p, 8);
        return 0;

    case H2_TYPE_WINDOW_UPDATE: {
        if (n != 4) return -1;
        uint32_t inc = (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                        ((uint32_t)p[2] << 8) | (uint32_t)p[3]) & 0x7FFFFFFFu;
        if (inc == 0) return 0;
        nl_h2_mu_lock(&s->mu);
        if (sid == 0) {
            s->conn_window += (int)inc;
            if (s->conn_window < 0 || s->conn_window > 0x7FFFFFFF) s->conn_window = 0x7FFFFFFF;
        } else {
            nl_px_h2st* st = nl_h2s_get_stream(s, sid, 0);
            if (st) {
                st->send_window += (int)inc;
                if (st->send_window < 0 || st->send_window > 0x7FFFFFFF) st->send_window = 0x7FFFFFFF;
            }
        }
        nl_h2_mu_unlock(&s->mu);
        nl_h2_cv_broadcast(&s->cv);
        return 0;
    }

    case H2_TYPE_RST_STREAM: {
        nl_h2_mu_lock(&s->mu);
        nl_px_h2st* st = nl_h2s_get_stream(s, sid, 0);
        if (st) st->reset = 1;
        nl_h2_mu_unlock(&s->mu);
        nl_h2_cv_broadcast(&s->cv);
        return 0;
    }

    case H2_TYPE_GOAWAY:
        nl_h2_mu_lock(&s->mu);
        s->goaway = 1;
        nl_h2_mu_unlock(&s->mu);
        return 0;

    case H2_TYPE_PRIORITY:
        return 0;

    case H2_TYPE_DATA: {
        if (sid == 0) return -1;
        size_t off = 0, dlen = n;
        if (h->flags & H2_FLAG_PADDED) {
            if (n < 1) return -1;
            size_t pad = p[0];
            off = 1;
            if (pad + 1 > n) return -1;
            dlen = n - 1 - pad;
        }
        nl_h2_mu_lock(&s->mu);
        nl_px_h2st* st = nl_h2s_get_stream(s, sid, 1);
        if (st && !st->reset) {
            if (st->body_len + dlen > NL_H2_MAX_BODY) {
                nl_h2_mu_unlock(&s->mu);
                nl_h2s_send_rst(s, sid, H2_ERR_CANCEL);
                return 0;
            }
            if (nl_h2_buf_append(&st->body, &st->body_len, &st->body_cap,
                                 p + off, dlen) != 0) {
                nl_h2_mu_unlock(&s->mu);
                return -1;
            }
        }
        nl_h2_mu_unlock(&s->mu);

        /* 及时放大接收窗口，避免大 body 被 65535 卡住 */
        nl_h2s_send_window_update(s, sid, (uint32_t)dlen);
        nl_h2s_send_window_update(s, 0, (uint32_t)dlen);

        if (st && (h->flags & H2_FLAG_END_STREAM)) nl_h2s_spawn(s, st);
        return 0;
    }

    case H2_TYPE_HEADERS: {
        if (sid == 0) return -1;
        if (s->pend_sid != 0) return -1;   /* 上一个头块未以 CONTINUATION 收尾 */
        size_t start = 0, end = n;
        if (h->flags & H2_FLAG_PADDED) {
            if (n < 1) return -1;
            size_t pad = (size_t)p[0];
            start = 1;
            if (start + pad > end) return -1;
            end -= pad;
        }
        if (h->flags & H2_FLAG_PRIORITY) {
            if (end < start + 5) return -1;
            start += 5;
        }
        if (end < start) return -1;
        int end_stream = (h->flags & H2_FLAG_END_STREAM) ? 1 : 0;
        if (h->flags & H2_FLAG_END_HEADERS)
            return nl_h2s_decode_request(s, sid, p + start, end - start, end_stream);
        s->pend_len = 0;
        s->pend_sid = sid;
        s->pend_end_stream = end_stream;
        if (nl_h2_buf_append(&s->pend, &s->pend_len, &s->pend_cap,
                             p + start, end - start) != 0)
            return -1;
        return 0;
    }

    case H2_TYPE_CONTINUATION: {
        if (s->pend_sid == 0 || sid != s->pend_sid) return -1;
        if (nl_h2_buf_append(&s->pend, &s->pend_len, &s->pend_cap, p, n) != 0)
            return -1;
        if (s->pend_len > NL_H2_MAX_BLOCK) return -1;
        if (h->flags & H2_FLAG_END_HEADERS) {
            uint32_t psid = s->pend_sid;
            int es = s->pend_end_stream;
            s->pend_sid = 0;
            int r = nl_h2s_decode_request(s, psid, s->pend, s->pend_len, es);
            s->pend_len = 0;
            return r;
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* ============================================================
 * 会话主循环
 * ============================================================ */
static int nl_h2s_read(nl_px_h2sess* s, void* dst, size_t n) {
    char* p = (char*)dst;
    size_t got = 0;
    while (got < n) {
        if (s->in_off < s->in_len) {
            size_t avail = s->in_len - s->in_off;
            size_t take = avail < n - got ? avail : n - got;
            memcpy(p + got, s->in + s->in_off, take);
            s->in_off += take;
            got += take;
            continue;
        }
        if (!nl_h2_wait_fd(s->fd, 1, 0, s->idle_ms)) return -1;
        int r = (int)recv((nl_h2fd)s->fd, p + got, (int)(n - got), 0);
        if (r > 0) { got += (size_t)r; continue; }
        if (r == 0) return -1;
        if (NL_H2_WOULDBLOCK(nl_h2_errno())) continue;
        return -1;
    }
    return 0;
}

static int nl_h2s_read_frame(nl_px_h2sess* s, struct nl_h2_frame_header* h) {
    uint8_t hdr[9];
    if (nl_h2s_read(s, hdr, 9) != 0) return -1;
    h2_frame_header_read(hdr, h);
    if (h->length > NL_H2_MAX_FRAME_RECV) return -1;
    if (h->length > 0) {
        if (h->length > s->fr_cap) {
            uint8_t* nb = (uint8_t*)realloc(s->fr, h->length);
            if (!nb) return -1;
            s->fr = nb;
            s->fr_cap = h->length;
        }
        if (nl_h2s_read(s, s->fr, h->length) != 0) return -1;
    }
    return 0;
}

static void nl_h2sess_shutdown(nl_px_h2sess* s) {
    nl_px_h2st* list = NULL;
    nl_h2_mu_lock(&s->mu);
    s->closed = 1;
    list = s->streams;
    s->streams = NULL;
    s->ready = NULL;        /* 排队项已在 streams 链表中，随链表统一回收 */
    s->ready_tail = NULL;
    nl_h2_mu_unlock(&s->mu);
    nl_h2_cv_broadcast(&s->cv);

#if defined(_WIN32)
    if (s->fd != (intptr_t)INVALID_SOCKET) shutdown((SOCKET)s->fd, SD_BOTH);
#else
    if (s->fd >= 0) shutdown((int)s->fd, SHUT_RDWR);
#endif
    if (s->fd != (intptr_t)NL_H2_INVALID) nl_h2_closefd((nl_h2fd)s->fd);
    s->fd = (intptr_t)NL_H2_INVALID;

    while (list) {
        nl_px_h2st* nx = list->next;
        list->next = NULL;
        nl_h2st_unref(list);
        list = nx;
    }
}

/* base64url 解码（RFC 4648 §5，容忍无填充；返回字节数，-1 非法） */
static int nl_h2_b64url_decode(const char* in, size_t n, uint8_t* out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }
    return (int)o;
}

/* 升级请求流 1 注入（HTTP/1.1 请求头 → stream 1） */
static int nl_h2s_seed_upgrade(nl_px_h2sess* s, const char* initial, size_t len) {
    const char* hend = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (initial[i] == '\r' && initial[i + 1] == '\n' &&
            initial[i + 2] == '\r' && initial[i + 3] == '\n') {
            hend = initial + i;
            break;
        }
    }
    if (!hend) return -1;
    const char* nl = (const char*)memchr(initial, '\n', len);
    if (!nl) return -1;

    nl_h2_mu_lock(&s->mu);
    nl_px_h2st* st = nl_h2s_get_stream(s, 1, 1);
    nl_h2_mu_unlock(&s->mu);
    if (!st) return -1;

    char method[24], path[4096], proto[32];
    if (nl_px_parse_request_line(initial, (size_t)(nl - initial),
                                 method, sizeof(method), path, sizeof(path),
                                 proto, sizeof(proto)) != 0)
        return -1;
    snprintf(st->method, sizeof(st->method), "%s", method);
    snprintf(st->path, sizeof(st->path), "%s", path);
    st->is_head = nl_h2_ci_eq(method, "HEAD");

    const char* p = nl + 1;
    size_t hdr_len = (size_t)(hend - p);
    char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
    size_t ho = 0;
    while (ho < hdr_len) {
        int r = nl_h2_next_header(p, hdr_len, &ho, name, sizeof(name),
                                  value, sizeof(value));
        if (r <= 0) break;
        if (nl_h2_ci_eq(name, "host")) {
            size_t an = strnlen(value, sizeof(st->authority) - 1);   /* 有界拷贝，防截断告警 */
            memcpy(st->authority, value, an); st->authority[an] = '\0';
            continue;
        }
        /* 升级请求携带的 HTTP2-Settings（base64url 的 SETTINGS 载荷）：
         * 必须应用，否则对端自定义的 INITIAL_WINDOW_SIZE / MAX_FRAME_SIZE
         * 会被忽略（发送窗口按默认 65535 走，大响应可能卡住或超帧长）。
         * 对端随后发出的 SETTINGS 帧与本值一致，重复应用是幂等的。 */
        if (nl_h2_ci_eq(name, "http2-settings")) {
            uint8_t sb[512];
            int sn = nl_h2_b64url_decode(value, strlen(value), sb, sizeof(sb));
            if (sn > 0) (void)nl_h2s_apply_settings(s, sb, (size_t)sn);
            continue;
        }
        if (nl_h2_skip_req_header(name)) continue;
        char line[MAX_HEADER_NAME + MAX_HEADER_VALUE + 4];
        int ln = snprintf(line, sizeof(line), "%s: %s\r\n", name, value);
        if (ln > 0)
            (void)nl_h2_buf_append(&st->hdr, &st->hdr_len, &st->hdr_cap,
                                   line, (size_t)ln);
    }

    /* 请求头之后可能已带 body（升级请求一般没有，有则原样回放） */
    size_t body_off = (size_t)(hend - initial) + 4;
    if (body_off < len &&
        nl_h2_buf_append(&st->body, &st->body_len, &st->body_cap,
                         initial + body_off, len - body_off) != 0)
        return -1;
    nl_h2s_spawn(s, st);
    return 0;
}

int nl_px_h2_bridge_run(intptr_t client_fd, int local_port,
                        const char* initial, size_t initial_len,
                        int is_upgrade, int idle_timeout_ms,
                        void* local_ctx, const nl_px_local_ops* local_ops) {
    if (client_fd == (intptr_t)NL_H2_INVALID || local_port <= 0) return -1;

    nl_px_h2sess* s = (nl_px_h2sess*)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->fd = client_fd;
    s->local_port = local_port;
    s->local_ctx = local_ctx;
    s->local_ops = local_ops;
    s->idle_ms = idle_timeout_ms > 0 ? idle_timeout_ms : NL_H2_DEFAULT_IDLE;
    s->conn_window = 65535;
    s->stream_window_init = 65535;
    s->max_frame = 16384;
    s->refs = 1;
    {   /* 记录监听地址：工作线程回放时自连真实网卡而非硬编码回环 */
        nl_h2_socklen_t alen = (nl_h2_socklen_t)sizeof(s->laddr);
        if (getsockname((nl_h2fd)client_fd, (struct sockaddr*)&s->laddr, &alen) != 0)
            memset(&s->laddr, 0, sizeof(s->laddr));
    }
    {   /* 对端 IP 文本：X-Forwarded-For 用（IPv4/IPv6 统一走公共 helper） */
        struct sockaddr_storage peer;
        nl_h2_socklen_t plen = (nl_h2_socklen_t)sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        if (getpeername((nl_h2fd)client_fd, (struct sockaddr*)&peer, &plen) == 0)
            (void)nl_px_sockaddr_ip((const struct sockaddr*)&peer,
                                    s->peer_ip, sizeof(s->peer_ip));
        else
            s->peer_ip[0] = '\0';
    }
    nl_h2_mu_init(&s->mu);
    nl_h2_mu_init(&s->wmu);
    nl_h2_cv_init(&s->cv);
    hpack_init(&s->dec);
    hpack_init(&s->enc);
    /* 编码端不使用动态索引：等价于表容量 0，且无需跟随对端 SETTINGS 变化 */
    s->enc.max_dynamic_table_size = 0;

    if (initial && initial_len) {
        s->in = (char*)malloc(initial_len);
        if (!s->in) { nl_h2sess_unref(s); return -1; }
        memcpy(s->in, initial, initial_len);
        s->in_len = initial_len;
    }

#if defined(_WIN32)
    u_long nb = 1;
    ioctlsocket((SOCKET)client_fd, FIONBIO, &nb);
#else
    {
        int fl = fcntl((int)client_fd, F_GETFL, 0);
        if (fl >= 0) fcntl((int)client_fd, F_SETFL, fl | O_NONBLOCK);
    }
#endif

    if (is_upgrade) {
        static const char sw101[] =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: h2c\r\n\r\n";
        if (nl_h2_send_fd(client_fd, sw101, sizeof(sw101) - 1) != 0) {
            nl_h2sess_shutdown(s);
            nl_h2sess_unref(s);
            return -1;
        }
    }

    /* 客户端连接预流（升级：101 之后；直接 h2c：首包已含） */
    {
        char magic[NL_PX_H2_PREFACE_LEN];
        if (nl_h2s_read(s, magic, sizeof(magic)) != 0 ||
            memcmp(magic, nl_h2_magic, sizeof(magic)) != 0) {
            nl_h2sess_shutdown(s);
            nl_h2sess_unref(s);
            return -1;
        }
    }

    /* 服务端 SETTINGS（空参数 = 默认值）作为连接首帧 */
    nl_h2s_write_frame(s, H2_TYPE_SETTINGS, 0, 0, NULL, 0);

    if (is_upgrade && nl_h2s_seed_upgrade(s, initial ? initial : "", initial ? initial_len : 0) != 0) {
        nl_h2sess_shutdown(s);
        nl_h2sess_unref(s);
        return -1;
    }

    while (!s->closed) {
        struct nl_h2_frame_header h;
        if (nl_h2s_read_frame(s, &h) != 0) break;
        if (nl_h2s_handle_frame(s, &h, s->fr, h.length) != 0) break;
    }

    nl_h2sess_shutdown(s);
    nl_h2sess_unref(s);
    return 0;
}

/* ============================================================
 * 上游 H2（ALPN=h2）客户端交换
 * ============================================================ */
typedef struct nl_h2u {
    intptr_t up_fd;
    void* conn;
    const nl_px_tls_api* api;
    int idle_ms;
    struct hpack_context dec;
    struct hpack_context enc;
    int conn_window;
    int stream_window;
    int max_frame;
    int status;
    char* hdrs;
    size_t hlen, hcap;
    char* body;
    size_t blen, bcap;
    int resp_done;
    char* pend;
    size_t pend_len, pend_cap;
    uint32_t pend_sid;
    uint8_t* fbuf;
} nl_h2u;

static int nl_h2u_write(nl_h2u* u, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        int w = u->api->conn_write(u->conn, p, (int)(len > 1u << 20 ? 1u << 20 : len));
        if (w > 0) { p += w; len -= (size_t)w; continue; }
        if (w == 0) {
            if (!nl_h2_wait_fd(u->up_fd, 1, 1, u->idle_ms)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int nl_h2u_read_exact(nl_h2u* u, void* dst, size_t len) {
    char* p = (char*)dst;
    while (len > 0) {
        int r = u->api->conn_read(u->conn, p, (int)len);
        if (r > 0) { p += r; len -= (size_t)r; continue; }
        if (r == 0) {
            if (!nl_h2_wait_fd(u->up_fd, 1, 0, u->idle_ms)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int nl_h2u_write_frame(nl_h2u* u, uint8_t type, uint8_t flags,
                              uint32_t sid, const void* payload, size_t len) {
    uint8_t hdr[9];
    struct nl_h2_frame_header h;
    h.length = (uint32_t)len;
    h.type = type;
    h.flags = flags;
    h.stream_id = sid & 0x7FFFFFFFu;
    h2_frame_header_write(hdr, &h);
    if (nl_h2u_write(u, hdr, 9) != 0) return -1;
    if (len > 0 && nl_h2u_write(u, payload, len) != 0) return -1;
    return 0;
}

static int nl_h2u_append_hdr(nl_h2u* u, const char* name, const char* value) {
    char line[MAX_HEADER_NAME + MAX_HEADER_VALUE + 4];
    int n = snprintf(line, sizeof(line), "%s: %s\r\n", name, value);
    if (n <= 0) return 0;
    return nl_h2_buf_append(&u->hdrs, &u->hlen, &u->hcap, line, (size_t)n);
}

static int nl_h2u_decode(nl_h2u* u, const void* block_v, size_t len) {
    const uint8_t* block = (const uint8_t*)block_v;
    size_t off = 0;
    char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
    while (off < len) {
        size_t consumed = 0;
        if (hpack_decode_header(&u->dec, block + off, len - off,
                                name, value, &consumed) != 0)
            return -1;
        if (consumed == 0) return -1;
        off += consumed;
        if (name[0] == '\0') continue;   /* 动态表容量更新，无头字段 */
        if (name[0] == ':') {
            if (nl_h2_ci_eq(name, ":status")) u->status = atoi(value);
            continue;
        }
        if (nl_h2_skip_resp_header(name)) continue;
        if (nl_h2u_append_hdr(u, name, value) != 0) return -1;
        if (u->hlen > NL_H2_MAX_HDR) return -1;
    }
    return 0;
}

static int nl_h2u_apply_settings(nl_h2u* u, const uint8_t* p, size_t n) {
    if (n % 6 != 0) return -1;
    for (size_t i = 0; i + 6 <= n; i += 6) {
        uint16_t id = (uint16_t)(((uint16_t)p[i] << 8) | p[i + 1]);
        uint32_t v = ((uint32_t)p[i + 2] << 24) | ((uint32_t)p[i + 3] << 16) |
                     ((uint32_t)p[i + 4] << 8) | (uint32_t)p[i + 5];
        if (id == H2_SETTINGS_INITIAL_WINDOW_SIZE) {
            int nv = (int)(v & 0x7FFFFFFFu);
            u->stream_window += nv - 65535;
        } else if (id == H2_SETTINGS_MAX_FRAME_SIZE) {
            if (v >= 16384 && v <= 16777215) u->max_frame = (int)v;
        }
    }
    return 0;
}

static int nl_h2u_dispatch(nl_h2u* u, const struct nl_h2_frame_header* h,
                           const uint8_t* p, size_t n) {
    uint32_t sid = h->stream_id & 0x7FFFFFFFu;
    switch (h->type) {
    case H2_TYPE_SETTINGS:
        if (!(h->flags & H2_FLAG_ACK)) {
            if (nl_h2u_apply_settings(u, p, n) != 0) return -1;
            if (nl_h2u_write_frame(u, H2_TYPE_SETTINGS, H2_FLAG_ACK, 0, NULL, 0) != 0)
                return -1;
        }
        return 0;

    case H2_TYPE_PING:
        if (n == 8 && !(h->flags & H2_FLAG_ACK))
            if (nl_h2u_write_frame(u, H2_TYPE_PING, H2_FLAG_ACK, 0, p, 8) != 0)
                return -1;
        return 0;

    case H2_TYPE_WINDOW_UPDATE: {
        if (n != 4) return -1;
        uint32_t inc = (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                        ((uint32_t)p[2] << 8) | (uint32_t)p[3]) & 0x7FFFFFFFu;
        if (sid == 0) u->conn_window += (int)inc;
        else u->stream_window += (int)inc;
        return 0;
    }

    case H2_TYPE_RST_STREAM:
        return -1;

    case H2_TYPE_GOAWAY:
        if (!u->resp_done) return -1;
        return 0;

    case H2_TYPE_DATA: {
        size_t off = 0, dlen = n;
        if (h->flags & H2_FLAG_PADDED) {
            if (n < 1) return -1;
            size_t pad = p[0];
            off = 1;
            if (pad + 1 > n) return -1;
            dlen = n - 1 - pad;
        }
        if (u->blen + dlen > NL_H2_MAX_BODY) return -1;
        if (nl_h2_buf_append(&u->body, &u->blen, &u->bcap, p + off, dlen) != 0)
            return -1;
        uint8_t inc[4] = { 0, 0, (uint8_t)((dlen >> 8) & 0xFF), (uint8_t)(dlen & 0xFF) };
        if (dlen > 0) {
            if (nl_h2u_write_frame(u, H2_TYPE_WINDOW_UPDATE, 0, 0, inc, 4) != 0) return -1;
            if (nl_h2u_write_frame(u, H2_TYPE_WINDOW_UPDATE, 0, sid, inc, 4) != 0) return -1;
        }
        if (h->flags & H2_FLAG_END_STREAM) u->resp_done = 1;
        return 0;
    }

    case H2_TYPE_HEADERS: {
        if (u->pend_sid != 0) return -1;
        size_t start = 0, end = n;
        if (h->flags & H2_FLAG_PADDED) {
            if (n < 1) return -1;
            size_t pad = (size_t)p[0];
            start = 1;
            if (start + pad > end) return -1;
            end -= pad;
        }
        if (h->flags & H2_FLAG_PRIORITY) {
            if (end < start + 5) return -1;
            start += 5;
        }
        if (end < start) return -1;
        if (h->flags & H2_FLAG_END_STREAM) u->resp_done = 1;
        if (h->flags & H2_FLAG_END_HEADERS)
            return nl_h2u_decode(u, p + start, end - start);
        u->pend_len = 0;
        u->pend_sid = sid;
        return nl_h2_buf_append(&u->pend, &u->pend_len, &u->pend_cap,
                                p + start, end - start);
    }

    case H2_TYPE_CONTINUATION:
        if (u->pend_sid == 0 || sid != u->pend_sid) return -1;
        if (nl_h2_buf_append(&u->pend, &u->pend_len, &u->pend_cap, p, n) != 0)
            return -1;
        if (u->pend_len > NL_H2_MAX_BLOCK) return -1;
        if (h->flags & H2_FLAG_END_HEADERS) {
            u->pend_sid = 0;
            int r = nl_h2u_decode(u, u->pend, u->pend_len);
            u->pend_len = 0;
            return r;
        }
        return 0;

    default:
        return 0;
    }
}

static int nl_h2u_read_frame(nl_h2u* u, struct nl_h2_frame_header* h) {
    uint8_t hdr[9];
    if (nl_h2u_read_exact(u, hdr, 9) != 0) return -1;
    h2_frame_header_read(hdr, h);
    if (h->length > 16384) return -1;   /* 本端 SETTINGS 未放大帧上限 */
    if (h->length > 0 && nl_h2u_read_exact(u, u->fbuf, h->length) != 0) return -1;
    return 0;
}

/* 解析 initial 里的 HTTP/1.1 请求 */
typedef struct nl_h2u_req {
    char method[24];
    char path[4096];
    char host[512];
    char* hdrs;
    size_t hlen, hcap;
    const char* body;
    size_t blen;
} nl_h2u_req;

static int nl_h2u_parse_req(const char* init, size_t len, nl_h2u_req* r) {
    memset(r, 0, sizeof(*r));
    const char* hend = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (init[i] == '\r' && init[i + 1] == '\n' &&
            init[i + 2] == '\r' && init[i + 3] == '\n') {
            hend = init + i;
            break;
        }
    }
    if (!hend) return -1;
    const char* nl = (const char*)memchr(init, '\n', len);
    if (!nl) return -1;
    char proto[32];
    if (nl_px_parse_request_line(init, (size_t)(nl - init),
                                 r->method, sizeof(r->method), r->path, sizeof(r->path),
                                 proto, sizeof(proto)) != 0)
        return -1;

    const char* p = nl + 1;
    size_t hdr_len = (size_t)(hend - p), off = 0;
    char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
    while (off < hdr_len) {
        int rr = nl_h2_next_header(p, hdr_len, &off, name, sizeof(name),
                                   value, sizeof(value));
        if (rr <= 0) break;
        if (nl_h2_ci_eq(name, "host")) {
            size_t hn = strnlen(value, sizeof(r->host) - 1);   /* 有界拷贝，防截断告警 */
            memcpy(r->host, value, hn); r->host[hn] = '\0';
            continue;
        }
        if (nl_h2_skip_req_header(name)) continue;
        char line[MAX_HEADER_NAME + MAX_HEADER_VALUE + 4];
        int ln = snprintf(line, sizeof(line), "%s: %s\r\n", name, value);
        if (ln > 0 && nl_h2_buf_append(&r->hdrs, &r->hlen, &r->hcap, line, (size_t)ln) != 0)
            return -1;
    }
    size_t boff = (size_t)(hend - init) + 4;
    if (boff < len) {
        r->body = init + boff;
        r->blen = len - boff;
    }
    return 0;
}

static const char* nl_h2_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default:  return "OK";
    }
}

int nl_px_h2_upstream_run(intptr_t client_fd, intptr_t upstream_fd,
                          void* tls_conn, const nl_px_tls_api* api,
                          const char* initial, int initial_len,
                          int idle_timeout_ms) {
    int rc = -1;
    nl_h2u u;
    nl_h2u_req req;
    memset(&u, 0, sizeof(u));
    memset(&req, 0, sizeof(req));
    u.up_fd = upstream_fd;
    u.conn = tls_conn;
    u.api = api;
    u.idle_ms = idle_timeout_ms > 0 ? idle_timeout_ms : NL_H2_DEFAULT_IDLE;
    u.conn_window = 65535;
    u.stream_window = 65535;
    u.max_frame = 16384;

    if (!api || !tls_conn || !api->conn_read || !api->conn_write || !api->conn_free)
        goto done;
    if (!initial || initial_len <= 0) goto done;
    if (nl_h2u_parse_req(initial, (size_t)initial_len, &req) != 0) goto done;

    hpack_init(&u.dec);
    hpack_init(&u.enc);
    u.enc.max_dynamic_table_size = 0;
    u.fbuf = (uint8_t*)malloc(16384);
    if (!u.fbuf) goto done;

    /* 连接预流 + 本端 SETTINGS */
    {
        static const char pre[] =
            "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
        if (nl_h2u_write(&u, pre, sizeof(pre) - 1) != 0) goto done;
        if (nl_h2u_write_frame(&u, H2_TYPE_SETTINGS, 0, 0, NULL, 0) != 0) goto done;
    }

    /* 读对端 SETTINGS（其间可能有其它帧） */
    {
        int got_settings = 0;
        while (!got_settings) {
            struct nl_h2_frame_header h;
            if (nl_h2u_read_frame(&u, &h) != 0) goto done;
            if (nl_h2u_dispatch(&u, &h, u.fbuf, h.length) != 0) goto done;
            if (h.type == H2_TYPE_SETTINGS && !(h.flags & H2_FLAG_ACK)) got_settings = 1;
        }
    }

    /* 请求 HEADERS（必要时 CONTINUATION） */
    {
        size_t cap = NL_H2_MAX_HDR;
        uint8_t* block = (uint8_t*)malloc(cap);
        if (!block) goto done;
        size_t off = 0;
        int n = hpack_encode_header(&u.enc, block + off, cap - off, ":method",
                                    req.method[0] ? req.method : "GET");
        if (n <= 0) { free(block); goto done; }
        off += (size_t)n;
        n = hpack_encode_header(&u.enc, block + off, cap - off, ":scheme", "https");
        if (n <= 0) { free(block); goto done; }
        off += (size_t)n;
        n = hpack_encode_header(&u.enc, block + off, cap - off, ":authority",
                                req.host[0] ? req.host : "localhost");
        if (n <= 0) { free(block); goto done; }
        off += (size_t)n;
        n = hpack_encode_header(&u.enc, block + off, cap - off, ":path",
                                req.path[0] ? req.path : "/");
        if (n <= 0) { free(block); goto done; }
        off += (size_t)n;

        size_t p = 0;
        char name[MAX_HEADER_NAME], value[MAX_HEADER_VALUE];
        while (p < req.hlen) {
            int rr = nl_h2_next_header(req.hdrs, req.hlen, &p, name, sizeof(name),
                                       value, sizeof(value));
            if (rr <= 0) break;
            if (nl_h2_skip_req_header(name)) continue;
            n = hpack_encode_header(&u.enc, block + off, cap - off, name, value);
            if (n > 0) off += (size_t)n;
        }

        int end_stream = (req.blen == 0) ? H2_FLAG_END_STREAM : 0;
        size_t first = off < (size_t)u.max_frame ? off : (size_t)u.max_frame;
        if (nl_h2u_write_frame(&u, H2_TYPE_HEADERS,
                               (uint8_t)((first < off ? 0 : H2_FLAG_END_HEADERS) | end_stream),
                               1, block, first) != 0) {
            free(block);
            goto done;
        }
        size_t sent_h = first;
        while (sent_h < off) {
            size_t take = off - sent_h;
            if (take > (size_t)16384) take = 16384;
            int last = (sent_h + take >= off);
            if (nl_h2u_write_frame(&u, H2_TYPE_CONTINUATION,
                                   last ? H2_FLAG_END_HEADERS : 0, 1,
                                   block + sent_h, take) != 0) {
                free(block);
                goto done;
            }
            sent_h += take;
        }
        free(block);
    }

    /* 请求 body：受发送窗口约束，窗口耗尽时读帧推进（可能拿到响应/窗口更新） */
    {
        size_t sent = 0;
        while (sent < req.blen && !u.resp_done) {
            int avail = u.conn_window < u.stream_window ? u.conn_window : u.stream_window;
            if (avail <= 0) {
                struct nl_h2_frame_header h;
                if (nl_h2u_read_frame(&u, &h) != 0) goto done;
                if (nl_h2u_dispatch(&u, &h, u.fbuf, h.length) != 0) goto done;
                continue;
            }
            size_t take = req.blen - sent;
            if (take > (size_t)avail) take = (size_t)avail;
            if (take > (size_t)u.max_frame) take = (size_t)u.max_frame;
            int last = (sent + take >= req.blen);
            if (nl_h2u_write_frame(&u, H2_TYPE_DATA, last ? H2_FLAG_END_STREAM : 0,
                                   1, req.body + sent, take) != 0)
                goto done;
            u.conn_window -= (int)take;
            u.stream_window -= (int)take;
            sent += take;
        }
    }

    /* 读响应直到 END_STREAM */
    while (!u.resp_done) {
        struct nl_h2_frame_header h;
        if (nl_h2u_read_frame(&u, &h) != 0) goto done;
        if (nl_h2u_dispatch(&u, &h, u.fbuf, h.length) != 0) goto done;
    }

    /* 还原成 HTTP/1.1 写回客户端 */
    {
        int status = u.status > 0 ? u.status : 502;
        char head[512];
        int hn = snprintf(head, sizeof(head),
                          "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\n",
                          status, nl_h2_reason(status), u.blen);
        if (hn > 0 && nl_h2_send_fd(client_fd, head, (size_t)hn) != 0) goto done;
        if (u.hlen && nl_h2_send_fd(client_fd, u.hdrs, u.hlen) != 0) goto done;
        if (nl_h2_send_fd(client_fd, "\r\n", 2) != 0) goto done;
        if (u.blen && nl_h2_send_fd(client_fd, u.body, u.blen) != 0) goto done;
        rc = 0;
    }

done:
    if (rc != 0) {
        static const char err502[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        (void)nl_h2_send_fd(client_fd, err502, sizeof(err502) - 1);
    }
    if (u.fbuf) free(u.fbuf);
    if (u.hdrs) free(u.hdrs);
    if (u.body) free(u.body);
    if (u.pend) free(u.pend);
    hpack_free(&u.dec);
    hpack_free(&u.enc);
    free(req.hdrs);

    if (api && tls_conn && api->conn_free) api->conn_free(tls_conn);
    if (upstream_fd != (intptr_t)NL_H2_INVALID) nl_h2_closefd((nl_h2fd)upstream_fd);
    if (client_fd != (intptr_t)NL_H2_INVALID) nl_h2_closefd((nl_h2fd)client_fd);
    return rc;
}
