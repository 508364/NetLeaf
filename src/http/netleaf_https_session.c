#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * HTTPS 扩展：TLS 会话对象（客户端角色，平台无关）
 *
 * 反代到 HTTPS 上游需要"在既有 TCP 连接上完成 TLS 握手并持续收发"，仅提供
 * 一次性握手钩子不够：握手后的所有读写都必须走同一个 TLS 会话，否则会把
 * 密文当明文转发。本文件即为公共层契约（src/common/nl_proxy_core.h）所需的
 * 会话实现，三平台同源（后端选择见 netleaf_https_backend.h）。
 *
 * 导出符号（非 static，扩展 DLL 默认导出）：
 *   nlh_tls_conn_wrap / nlh_tls_conn_read / nlh_tls_conn_write / nlh_tls_conn_free
 *
 * 读写语义（事件驱动友好）：
 *   >0 实际字节数；0 暂时不可读/写（WANT_*）；<0 EOF/错误。
 *   wrap 成功后底层 socket 被置为非阻塞，调用方用 select/epoll 驱动。
 *
 * 线程安全：会话对象由单一所有者线程使用，不做内部加锁（热点路径零锁）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netleaf_module.h"
#include "netleaf_https_session.h"
#include "netleaf_https_backend.h"

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define NLH_FD SOCKET
#else
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <fcntl.h>
    #include <unistd.h>
    #define NLH_FD int
#endif

/* 会话对象：tls 上下文 + 归属 socket + 协商出的 ALPN（自持，供 out_alpn 返回） */
typedef struct nlh_tls_conn {
    NL_HTTPS_TLS_CTX* tls;
    int  sock;
    char alpn[32];
} nlh_tls_conn;

/* 上游 ALPN：默认同时宣告 h2 与 http/1.1，协商结果由 wrap 的 out_alpn 返回；
 * NETLEAF_DISABLE_UPSTREAM_H2=1 可退回纯 HTTP/1.1（排查上游 H2 兼容问题）。 */
static const char* const nlh_alpn_both[]   = { "h2", "http/1.1", NULL };
static const char* const nlh_alpn_http11[] = { "http/1.1", NULL };

static int nlh_env_on(const char* name) {
    const char* v = getenv(name);
    if (!v || !*v) return 0;
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y';
}

static int nlh_env_int(const char* name, int def, int min_v) {
    const char* v = getenv(name);
    if (!v || !*v) return def;
    int n = atoi(v);
    return n >= min_v ? n : def;
}

/* socket 切非阻塞（TLS 后端在非阻塞模式下返回 WANT_*，由事件循环驱动） */
static int nlh_set_nonblocking(int sock) {
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket((SOCKET)sock, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int fl = fcntl(sock, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(sock, F_SETFL, fl | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

/* 等待 fd 可读/可写，返回 1=就绪 0=超时/错误 */
static int nlh_wait_io(int sock, int for_write, int timeout_ms) {
    NLH_FD fd = (NLH_FD)sock;
    fd_set rs, ws;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    if (for_write) FD_SET(fd, &ws);
    else           FD_SET(fd, &rs);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    int r = select(0, for_write ? NULL : &rs, for_write ? &ws : NULL, NULL, &tv);
#else
    int r = select(sock + 1, for_write ? NULL : &rs, for_write ? &ws : NULL, NULL, &tv);
#endif
    return r > 0 ? 1 : 0;
}

/*
 * 建立客户端 TLS 会话。
 *   host     上游主机名（SNI / 证书主机名校验；NULL 表示不带 SNI）
 *   out_alpn 可选，返回协商出的 ALPN（指向会话内部存储，随会话释放失效）
 * 返回会话句柄；失败返回 NULL（绝不降级明文）。
 */
void* nlh_tls_conn_wrap(int sock, const char* host, const char** out_alpn) {
    if (out_alpn) *out_alpn = NULL;
    if (sock <= 0 || !NL_HTTPS_TLS_is_available()) return NULL;

    int timeout_ms = nlh_env_int("NETLEAF_TLS_HANDSHAKE_TIMEOUT_MS", 10000, 100);

    NL_HTTPS_TLS_CTX* tls = NL_HTTPS_TLS_create();
    if (!tls) return NULL;

    NL_HTTPS_TLS_CONFIG cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.is_server = 0;
    cfg.handshake_timeout_ms = timeout_ms;
    cfg.alpn = nlh_env_on("NETLEAF_DISABLE_UPSTREAM_H2") ? nlh_alpn_http11 : nlh_alpn_both;
    /* 默认仅加密不校验证书链（兼容内网自签上游）；显式开启时才加载 CA。 */
    cfg.verify_peer = nlh_env_on("NETLEAF_TLS_VERIFY") ? 1 : 0;
    cfg.ca_file = getenv("NETLEAF_TLS_CA_FILE");

    if (NL_HTTPS_TLS_configure(tls, &cfg) != NL_HTTPS_TLS_OK) {
        NL_HTTPS_TLS_destroy(tls);
        return NULL;
    }
    if (host && *host && NL_HTTPS_TLS_set_hostname(tls, host) != NL_HTTPS_TLS_OK) {
        NL_HTTPS_TLS_destroy(tls);
        return NULL;
    }
    if (nlh_set_nonblocking(sock) != 0) {
        NL_HTTPS_TLS_destroy(tls);
        return NULL;
    }
    if (NL_HTTPS_TLS_set_nonblocking(tls, 1) != NL_HTTPS_TLS_OK) {
        NL_HTTPS_TLS_destroy(tls);
        return NULL;
    }

    /* 非阻塞握手：WANT_* 时等 fd 就绪再推进，整体受 timeout_ms 约束 */
    int steps = timeout_ms / 50;
    if (steps < 1) steps = 1;
    for (int i = 0; i < steps; i++) {
        int r = NL_HTTPS_TLS_handshake_ex(tls, sock, 0);
        if (r == NL_HTTPS_TLS_OK) break;
        int wr;
        if (r == NL_HTTPS_TLS_WANT_READ)       wr = 0;
        else if (r == NL_HTTPS_TLS_WANT_WRITE) wr = 1;
        else {
            NL_HTTPS_TLS_destroy(tls);
            return NULL;
        }
        if (!nlh_wait_io(sock, wr, 50)) {
            NL_HTTPS_TLS_destroy(tls);
            return NULL;
        }
        if (i == steps - 1) {
            NL_HTTPS_TLS_destroy(tls);
            return NULL;
        }
    }

    nlh_tls_conn* conn = (nlh_tls_conn*)calloc(1, sizeof(*conn));
    if (!conn) {
        NL_HTTPS_TLS_destroy(tls);
        return NULL;
    }
    conn->tls  = tls;
    conn->sock = sock;

    const char* proto = NL_HTTPS_TLS_get_alpn(tls);
    if (proto && *proto) {
        /* 只保留短协议名，避免未来协议串变长溢出 */
        size_t n = strlen(proto);
        if (n >= sizeof(conn->alpn)) n = sizeof(conn->alpn) - 1;
        memcpy(conn->alpn, proto, n);
        conn->alpn[n] = '\0';
    }
    if (out_alpn) *out_alpn = conn->alpn[0] ? conn->alpn : NULL;
    return conn;
}

int nlh_tls_conn_read(void* handle, void* buf, int len) {
    nlh_tls_conn* conn = (nlh_tls_conn*)handle;
    if (!conn || !buf || len <= 0) return -1;
    int r = NL_HTTPS_TLS_recv(conn->tls, buf, (size_t)len);
    if (r >= 0) return r;
    if (r == NL_HTTPS_TLS_WANT_READ || r == NL_HTTPS_TLS_WANT_WRITE) return 0;
    return -1;
}

int nlh_tls_conn_write(void* handle, const void* buf, int len) {
    nlh_tls_conn* conn = (nlh_tls_conn*)handle;
    if (!conn || !buf || len <= 0) return -1;
    int r = NL_HTTPS_TLS_send(conn->tls, buf, (size_t)len);
    if (r >= 0) return r;
    if (r == NL_HTTPS_TLS_WANT_READ || r == NL_HTTPS_TLS_WANT_WRITE) return 0;
    return -1;
}

void nlh_tls_conn_free(void* handle) {
    nlh_tls_conn* conn = (nlh_tls_conn*)handle;
    if (!conn) return;
    if (conn->tls) NL_HTTPS_TLS_destroy(conn->tls);
    free(conn);
}
