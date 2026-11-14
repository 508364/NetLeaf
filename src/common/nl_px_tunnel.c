/*
 * nl_px_tunnel.c - HTTPS 上游字节隧道（平台无关，一份实现）
 *
 * 反代到 TLS 上游时，加密通道的收发必须始终经过同一个会话对象；把这段
 * 双向对灌逻辑下沉到公共层，三平台只负责"谁在什么时机开线程/回收槽位"。
 *
 * 线程模型：单线程独占两个 fd，客户端侧阻塞收发，上游侧用非阻塞会话 +
 * select 事件驱动。热路径固定 16KB 栈缓冲，零动态分配（会话对象由调用方建）。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nl_proxy_core.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #define NL_TUN_FD       SOCKET
    #define nl_tun_close(f) closesocket((SOCKET)(f))
    #define nl_tun_errno()  ((int)WSAGetLastError())
    #define NL_TUN_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK)
#else
    #include <sys/select.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    #define NL_TUN_FD       int
    #define nl_tun_close(f) close((int)(f))
    #define nl_tun_errno()  (errno)
    #define NL_TUN_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#endif

#define NL_TUN_BUF          16384
#define NL_TUN_WRITE_TIMEOUT 10000   /* 单次写等待上限（毫秒） */
#define NL_TUN_IDLE_DEFAULT 60000

/* 等待 fd 可读和/或可写，返回 1=就绪 0=超时/错误 */
static int nl_tun_wait(intptr_t fd, int want_read, int want_write, int timeout_ms) {
    NL_TUN_FD f = (NL_TUN_FD)fd;
    fd_set rs, ws;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    if (want_read)  FD_SET(f, &rs);
    if (want_write) FD_SET(f, &ws);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    int r = select(0, want_read ? &rs : NULL, want_write ? &ws : NULL, NULL, &tv);
#else
    int r = select((int)f + 1, want_read ? &rs : NULL, want_write ? &ws : NULL, NULL, &tv);
#endif
    return r > 0 ? 1 : 0;
}

/* 客户端侧：确保阻塞（隧道线程独占该 fd） */
static void nl_tun_set_blocking(intptr_t fd) {
#if defined(_WIN32)
    u_long off = 0;
    ioctlsocket((SOCKET)fd, FIONBIO, &off);
#else
    int fl = fcntl((int)fd, F_GETFL, 0);
    if (fl >= 0) fcntl((int)fd, F_SETFL, fl & ~O_NONBLOCK);
#endif
}

/* 把 len 字节完整写入 TLS 会话；0（WANT）时等 fd 就绪再重试 */
static int nl_tun_tls_write(intptr_t up_fd, const nl_px_tls_api* api,
                            void* tls_conn, const char* p, int len) {
    int off = 0;
    while (off < len) {
        int w = api->conn_write(tls_conn, p + off, len - off);
        if (w > 0) { off += w; continue; }
        if (w == 0) {
            if (!nl_tun_wait(up_fd, 1, 1, NL_TUN_WRITE_TIMEOUT)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* 客户端侧完整发送（写慢时等待可写） */
static int nl_tun_send_all(intptr_t cfd, const char* p, int len) {
    int off = 0;
    while (off < len) {
        int w = (int)send((NL_TUN_FD)cfd, p + off, len - off, 0);
        if (w > 0) { off += w; continue; }
        if (w < 0 && NL_TUN_WOULDBLOCK(nl_tun_errno())) {
            if (!nl_tun_wait(cfd, 0, 1, NL_TUN_WRITE_TIMEOUT)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

int nl_px_tls_tunnel_run(intptr_t client_fd, intptr_t upstream_fd,
                         void* tls_conn, const nl_px_tls_api* api,
                         const char* initial, int initial_len,
                         int idle_timeout_ms) {
    if (!api || !tls_conn || !api->conn_read || !api->conn_write || !api->conn_free)
        return -1;
    if (idle_timeout_ms <= 0) idle_timeout_ms = NL_TUN_IDLE_DEFAULT;

    int status = 0;
    char buf[NL_TUN_BUF];

    nl_tun_set_blocking(client_fd);

    /* 首包：已构造好的上游请求头 + 已到达的 body */
    if (initial && initial_len > 0) {
        if (nl_tun_tls_write(upstream_fd, api, tls_conn, initial, initial_len) != 0) {
            status = -1;
            goto done;
        }
    }

    for (;;) {
        /* 1) 上游 → 客户端：把 TLS 会话里可读的明文一次读净 */
        for (;;) {
            int n = api->conn_read(tls_conn, buf, NL_TUN_BUF);
            if (n > 0) {
                if (nl_tun_send_all(client_fd, buf, n) != 0) { status = -1; goto done; }
                continue;
            }
            if (n == 0) break;          /* 暂无数据 */
            status = 0;                 /* 上游 EOF/错误：正常收尾 */
            goto done;
        }

        /* 2) 等待任一侧可读（空闲超时即关闭，防止槽位泄漏） */
        {
            NL_TUN_FD cf = (NL_TUN_FD)client_fd;
            NL_TUN_FD uf = (NL_TUN_FD)upstream_fd;
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(cf, &rs);
            FD_SET(uf, &rs);
            struct timeval tv;
            tv.tv_sec  = idle_timeout_ms / 1000;
            tv.tv_usec = (idle_timeout_ms % 1000) * 1000;
#if defined(_WIN32)
            int r = select(0, &rs, NULL, NULL, &tv);
#else
            int maxfd = (int)(client_fd > upstream_fd ? client_fd : upstream_fd);
            int r = select(maxfd + 1, &rs, NULL, NULL, &tv);
#endif
            if (r == 0) goto done;                       /* 空闲超时 */
            if (r < 0) {
                if (NL_TUN_WOULDBLOCK(nl_tun_errno())) continue;
                status = -1;
                goto done;
            }

            /* 客户端 → 上游 */
            if (FD_ISSET(cf, &rs)) {
                int n = (int)recv(cf, buf, NL_TUN_BUF, 0);
                if (n > 0) {
                    if (nl_tun_tls_write(upstream_fd, api, tls_conn, buf, n) != 0) {
                        status = -1;
                        goto done;
                    }
                } else if (n == 0) {
                    goto done;                            /* 客户端关闭 */
                } else if (!NL_TUN_WOULDBLOCK(nl_tun_errno())) {
                    status = -1;
                    goto done;
                }
            }
            /* 上游可读：回到循环顶部读净 */
        }
    }

done:
    if (tls_conn) api->conn_free(tls_conn);
    nl_tun_close(client_fd);
    nl_tun_close(upstream_fd);
    return status;
}
