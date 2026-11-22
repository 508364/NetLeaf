/*
 * nl_px_local.c - 进程内路由分发实现（平台无关）
 *
 * 设计见 nl_px_local.h。这里只做三件事：
 *   1) 本地路由（CONTENT / FILE / REDIRECT）直接产出响应头行 + body；
 *   2) HTTP 反代在调用线程内完成一次上游请求/响应交换；
 *   3) 提供 IPv6 / host:port 文本工具，供三平台共用。
 *
 * HTTPS / TCP 上游不在本模块处理（返回 FALLBACK），由平台既有回放路径负责。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "nl_px_local.h"
#include "nl_proxy_core.h"
#include "nl_uri.h"
#include "nl_filesrv.h"        /* 第203轮：DIR 端点按扩展名猜 MIME */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <sys/stat.h>
    typedef SOCKET pl_fd;
    #define PL_BAD_FD INVALID_SOCKET
    #define pl_closefd closesocket
    #define pl_errno() ((int)WSAGetLastError())
    #define PL_WOULDBLOCK(e) ((e) == WSAEWOULDBLOCK || (e) == WSAEINPROGRESS)
#else
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <errno.h>
    #include <poll.h>
    #include <sys/stat.h>
    typedef int pl_fd;
    #define PL_BAD_FD (-1)
    #define pl_closefd close
    #define pl_errno() (errno)
    #define PL_WOULDBLOCK(e) ((e) == EAGAIN || (e) == EWOULDBLOCK || (e) == EINTR)
#endif

#define PL_DEFAULT_IDLE 60000

/* 第203轮：DIR 端点 stat —— Windows 用 _stat，POSIX 用 stat */
#if defined(_WIN32)
    #define pl_stat_t   struct _stat
    #define pl_stat     _stat
    #define PL_S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
    #define PL_S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#else
    #define pl_stat_t   struct stat
    #define pl_stat     stat
    #define PL_S_ISREG(m) S_ISREG(m)
    #define PL_S_ISDIR(m) S_ISDIR(m)
#endif
#define PL_MAX_BODY     (64u << 20)
#define PL_HDR_NAME_MAX 256
#define PL_HDR_VAL_MAX  4096

/* ============================================================
 * 基础 IO
 * ============================================================ */
static int pl_wait_fd(pl_fd fd, int want_read, int want_write, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = PL_DEFAULT_IDLE;
#if defined(_WIN32)
    fd_set rs, ws;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    if (want_read)  FD_SET(fd, &rs);
    if (want_write) FD_SET(fd, &ws);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(0, want_read ? &rs : NULL, want_write ? &ws : NULL, NULL, &tv);
    return r > 0 ? 1 : 0;
#else
    struct pollfd p;
    p.fd = fd;
    p.events = (short)((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0));
    p.revents = 0;
    int r = poll(&p, 1, timeout_ms);
    return r > 0 ? 1 : 0;
#endif
}

static int pl_send_all(pl_fd fd, const char* data, size_t len) {
    while (len > 0) {
        int chunk = (int)(len > (1u << 20) ? (1u << 20) : len);
        int w = (int)send(fd, data, chunk, 0);
        if (w > 0) { data += w; len -= (size_t)w; continue; }
        if (w < 0 && PL_WOULDBLOCK(pl_errno())) {
            if (!pl_wait_fd(fd, 0, 1, PL_DEFAULT_IDLE)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

static int pl_buf_append(char** buf, size_t* len, size_t* cap,
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

static int pl_ci_eq(const char* a, const char* b) {
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

/* 逐行遍历 "name: value\r\n"：1 = 取到一行，0 = 结束，-1 = 格式错误 */
static int pl_next_header(const char* p, size_t len, size_t* off,
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

/* 需要丢弃的逐跳响应头（H2 由帧边界表达长度） */
static int pl_skip_resp_header(const char* name) {
    static const char* const list[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding",
        "upgrade", "content-length", "trailer", NULL
    };
    for (int i = 0; list[i]; i++) if (pl_ci_eq(name, list[i])) return 1;
    return 0;
}

/* ---------------- HTTP/1.1 响应读取（含 framing） ---------------- */
static int pl_chunk_complete(const char* d, size_t n) {
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

static char* pl_dechunk(const char* d, size_t n, size_t* out_len) {
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

static int pl_read_http1(pl_fd fd, int idle_ms, int is_head,
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
                char* sp = (char*)memchr(buf, ' ', hdr_end);
                status = (sp && sp + 1 < buf + hdr_end) ? atoi(sp + 1) : 0;
                if (status >= 100 && status < 200 && status != 101) {
                    size_t skip = hdr_end + 4;
                    memmove(buf, buf + skip, len - skip);
                    len -= skip;
                    buf[len] = '\0';
                    continue;
                }
                char name[PL_HDR_NAME_MAX], value[PL_HDR_VAL_MAX];
                char* nl = (char*)memchr(buf, '\n', hdr_end);
                if (!nl) goto done;
                size_t off = (size_t)(nl - buf) + 1;
                while (off < hdr_end) {
                    int r = pl_next_header(buf, hdr_end + 2, &off,
                                           name, sizeof(name), value, sizeof(value));
                    if (r <= 0) break;
                    if (pl_ci_eq(name, "content-length")) cl = atoll(value);
                    else if (pl_ci_eq(name, "transfer-encoding") &&
                             strstr(value, "chunked")) chunked = 1;
                    if (pl_skip_resp_header(name)) continue;
                    char line[PL_HDR_NAME_MAX + PL_HDR_VAL_MAX + 4];
                    int ln = snprintf(line, sizeof(line), "%s: %s\r\n", name, value);
                    if (ln > 0 && pl_buf_append(&hdrs, &hlen, &hcap, line, (size_t)ln) != 0)
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
                int c = pl_chunk_complete(buf + body_off, len - body_off);
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
            if (cap >= PL_MAX_BODY) goto done;
            size_t nc = cap * 2;
            char* nb = (char*)realloc(buf, nc + 1);
            if (!nb) goto done;
            buf = nb;
            cap = nc;
        }
        if (!pl_wait_fd(fd, 1, 0, idle_ms)) goto done;
        int r = (int)recv(fd, buf + len, (int)(cap - len), 0);
        if (r > 0) { len += (size_t)r; buf[len] = '\0'; continue; }
        if (r == 0) { eof = 1; continue; }
        if (PL_WOULDBLOCK(pl_errno())) continue;
        goto done;
    }

    {
        size_t bl = len > body_off ? len - body_off : 0;
        char* body = NULL;
        size_t blen = 0;
        if (is_head || status == 204 || status == 304) {
            body = (char*)malloc(1);
            blen = 0;
        } else if (chunked) {
            body = pl_dechunk(buf + body_off, bl, &blen);
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
 * 地址文本工具
 * ============================================================ */
int nl_px_sockaddr_ip(const struct sockaddr* sa, char* out, size_t cap) {
    if (!sa || !out || cap < 8) return 0;
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in* v4 = (const struct sockaddr_in*)sa;
        const unsigned char* b = (const unsigned char*)&v4->sin_addr.s_addr;
        int n = snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        return n > 0 && (size_t)n < cap;
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6* v6 = (const struct sockaddr_in6*)sa;
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
            const unsigned char* b = (const unsigned char*)&v6->sin6_addr.s6_addr[12];
            int n = snprintf(out, cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            return n > 0 && (size_t)n < cap;
        }
        return inet_ntop(AF_INET6, &v6->sin6_addr, out, cap) != NULL;
    }
    return 0;
}

int nl_px_split_hostport(const char* in, char* host, size_t host_cap,
                         int* port, int def_port) {
    if (!in || !host || host_cap == 0) return 0;
    char tmp[512];
    size_t n = strlen(in);
    if (n >= sizeof(tmp)) return 0;
    memcpy(tmp, in, n + 1);
    int p = def_port;
    const char* h = tmp;
    size_t hlen = n;

    if (tmp[0] == '[') {
        char* rb = strchr(tmp + 1, ']');
        if (!rb) return 0;
        hlen = (size_t)(rb - (tmp + 1));
        if (rb[1] == ':') p = atoi(rb + 2);
        else if (rb[1] != '\0') return 0;
        h = tmp + 1;
    } else {
        int colons = 0;
        for (const char* q = tmp; *q; q++) if (*q == ':') colons++;
        if (colons == 1) {
            char* c = strrchr(tmp, ':');
            hlen = (size_t)(c - tmp);
            p = atoi(c + 1);
        }
        /* colons > 1：裸 IPv6 字面量，无端口；colons == 0：仅主机 */
    }
    if (hlen >= host_cap) return 0;
    memcpy(host, h, hlen);
    host[hlen] = '\0';
    if (p <= 0) p = def_port;
    if (port) *port = p;
    return 1;
}

int nl_px_format_hostport(const char* host, int port, char* out, size_t cap) {
    if (!host || !out || cap == 0) return -1;
    int n;
    if (host[0] == '[') n = snprintf(out, cap, "%s:%d", host, port);
    else if (strchr(host, ':')) n = snprintf(out, cap, "[%s]:%d", host, port);
    else n = snprintf(out, cap, "%s:%d", host, port);
    if (n < 0 || (size_t)n >= cap) return -1;
    return n;
}

/* ============================================================
 * 响应构造辅助
 * ============================================================ */
static int pl_set_response(int status, char* hdrs, size_t hlen,
                           char* body, size_t blen, int is_head,
                           int* out_status, char** out_hdrs, size_t* out_hlen,
                           char** out_body, size_t* out_blen) {
    /* HEAD / 204 / 304：不分配 body（调用方 free(NULL) 安全） */
    if (is_head || status == 204 || status == 304) {
        free(body);
        body = NULL;
        blen = 0;
    }
    *out_status = status;
    *out_hdrs = hdrs;
    *out_hlen = hlen;
    *out_body = body;
    *out_blen = blen;
    return 0;
}

static int pl_simple_response(int status, int* out_status, char** out_hdrs,
                              size_t* out_hlen, char** out_body, size_t* out_blen) {
    return pl_set_response(status, NULL, 0, NULL, 0, 0,
                           out_status, out_hdrs, out_hlen, out_body, out_blen);
}

/* 一次 HTTP/1.1 上游交换（仅明文 HTTP） */
static int pl_proxy_http(const nl_px_local_ops* ops, void* ctx,
                         const nl_px_local_route* route,
                         const char* method, const char* path,
                         const char* req_hdrs, size_t req_hdr_len,
                         const char* req_body, size_t req_body_len,
                         const char* peer_ip, int is_head, int idle_ms,
                         int* out_status, char** out_hdrs, size_t* out_hlen,
                         char** out_body, size_t* out_blen) {
    char up_host[256];
    int up_port = 80;
    if (!nl_px_split_hostport(route->upstream, up_host, sizeof(up_host), &up_port, 80))
        return pl_simple_response(502, out_status, out_hdrs, out_hlen, out_body, out_blen);
    char hostport[320];
    if (nl_px_format_hostport(up_host, up_port, hostport, sizeof(hostport)) < 0)
        return pl_simple_response(502, out_status, out_hdrs, out_hlen, out_body, out_blen);

    size_t cap = req_hdr_len + req_body_len + 1024;
    char* req = (char*)malloc(cap);
    if (!req) return -1;
    int n = nl_px_build_request(req, cap, method, path, "HTTP/1.1", hostport,
                                req_hdrs, req_hdr_len, peer_ip);
    if (n <= 0) {
        free(req);
        return pl_simple_response(502, out_status, out_hdrs, out_hlen, out_body, out_blen);
    }
    if (req_body_len > 0) {
        if ((size_t)n + req_body_len > cap) {
            char* nb = (char*)realloc(req, (size_t)n + req_body_len + 1);
            if (!nb) { free(req); return -1; }
            req = nb;
            cap = (size_t)n + req_body_len + 1;
        }
        memcpy(req + n, req_body, req_body_len);
        n += (int)req_body_len;
    }

    intptr_t fd = ops->connect_upstream ? ops->connect_upstream(ctx, route) : -1;
    if (fd < 0) {
        free(req);
        return pl_simple_response(502, out_status, out_hdrs, out_hlen, out_body, out_blen);
    }

    int rc;
    if (pl_send_all((pl_fd)fd, req, (size_t)n) != 0) {
        rc = pl_simple_response(502, out_status, out_hdrs, out_hlen, out_body, out_blen);
    } else {
        rc = pl_read_http1((pl_fd)fd, idle_ms, is_head,
                           out_status, out_hdrs, out_hlen, out_body, out_blen);
        if (rc != 0)
            rc = pl_simple_response(502, out_status, out_hdrs, out_hlen, out_body, out_blen);
    }
    pl_closefd((pl_fd)fd);
    free(req);
    return rc;
}

/* ============================================================
 * 第203轮：DIR 端点（端点即根目录 / 端点即文件）
 * ============================================================ */
#define PL_DIR_ENOENT (-100)   /* 端点根下目标不存在（调用方映射 404） */

/* Content-Type：路由显式值优先，否则按扩展名猜（nl_filesrv）。 */
static const char* pl_guess_ct(const char* fpath, const char* ct_hint) {
    if (ct_hint && ct_hint[0]) return ct_hint;
    char ext[32];
    nl_filesrv_ext(fpath, ext, sizeof(ext));
    const char* m = nl_filesrv_mime(ext);
    return (m && m[0]) ? m : "application/octet-stream";
}

/* 把 clamp 规范化结果中的 '%' 重编码为 %25 —— 下一步
 * nl_uri_resolve_file 会再次 percent-decode，重编码保证解码往返一致：
 *   /web/a%2520b → clamp 解码 → /a%20b → 重编码 → /a%2520b
 *   → resolve 解码 → /a%20b（文件名字面 "a%20b"，防双重解码）。
 * 返回 0 成功，-1 容量不足。 */
static int pl_reencode_pct(const char* in, char* out, size_t cap) {
    size_t w = 0;
    for (const char* p = in; *p; p++) {
        if (*p == '%') {
            if (w + 4 > cap) return -1;
            out[w++] = '%'; out[w++] = '2'; out[w++] = '5';
        } else {
            if (w + 2 > cap) return -1;
            out[w++] = *p;
        }
    }
    out[w] = '\0';
    return 0;
}

/* 打开本地文件构造 200 响应（FILE 路由与 DIR 端点共用）。 */
static int pl_file_response(const char* fpath, const char* ct_hint, int is_head,
                            int* out_status, char** out_hdrs, size_t* out_hlen,
                            char** out_body, size_t* out_blen) {
    if (!fpath)
        return pl_simple_response(404, out_status, out_hdrs, out_hlen,
                                  out_body, out_blen);
    FILE* f = fopen(fpath, "rb");
    if (!f)
        return pl_simple_response(404, out_status, out_hdrs, out_hlen,
                                  out_body, out_blen);
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return pl_simple_response(500, out_status, out_hdrs, out_hlen,
                                  out_body, out_blen);
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > NL_PX_LOCAL_FILE_MAX) {
        fclose(f);
        return pl_simple_response(500, out_status, out_hdrs, out_hlen,
                                  out_body, out_blen);
    }
    rewind(f);
    char* body = NULL;
    size_t got = 0;
    if (!is_head && sz > 0) {
        body = (char*)malloc((size_t)sz);
        if (!body) {
            fclose(f);
            return NL_PX_LOCAL_ERROR;
        }
        got = fread(body, 1, (size_t)sz, f);
    }
    fclose(f);
    const char* ct = pl_guess_ct(fpath, ct_hint);
    size_t hl = strlen(ct) + 32;
    char* hdrs = (char*)malloc(hl);
    if (!hdrs) {
        free(body);
        return NL_PX_LOCAL_ERROR;
    }
    int n = snprintf(hdrs, hl, "Content-Type: %s\r\n", ct);
    if (n <= 0) {
        free(hdrs);
        free(body);
        return NL_PX_LOCAL_ERROR;
    }
    return pl_set_response(200, hdrs, (size_t)n, body, got, is_head,
                           out_status, out_hdrs, out_hlen, out_body, out_blen);
}

/*
 * DIR 端点解析：path（含 query）→ 端点根下的完整磁盘路径 out。
 * 语义：
 *   - 端点指向文件：端点即根，任意余段都服务该文件；
 *   - 端点指向目录：字面剥端点前缀 → clamp 规范化（端点内 ../ 不逃逸，
 *     越根 .. 忽略）→ % 重编码 → nl_uri_resolve_file 安全拼接 → stat：
 *       文件 → 直接服务；
 *       目录且余段以 '/' 结尾 → 找 index.html（is_under 二次校验）；
 *       其余 → PL_DIR_ENOENT。
 * 返回 NL_URI_OK / PL_DIR_ENOENT / NL_URI_*（EINVAL→400、
 * ESCAPES/EROOT→403、ENOSPC→500）。
 */
static int pl_dir_resolve(const nl_px_local_route* route, const char* path,
                          char* out, size_t cap) {
    if (!route || !route->file_path || !route->dir_prefix || !path || cap < 2)
        return NL_URI_EINVAL;

    pl_stat_t st;
    if (pl_stat(route->file_path, &st) != 0) return PL_DIR_ENOENT;
    if (PL_S_ISREG(st.st_mode)) {
        /* 端点指向文件：文件即根，忽略余段 */
        size_t n = strlen(route->file_path);
        if (n + 1 > cap) return NL_URI_ENOSPC;
        memcpy(out, route->file_path, n + 1);
        return NL_URI_OK;
    }
    if (!PL_S_ISDIR(st.st_mode)) return PL_DIR_ENOENT;

    /* 1) 字面剥端点前缀（注册前缀以 '/' 开头、无尾 '/'，见 nl_web_add_dir） */
    const char* pre = route->dir_prefix;
    size_t plen = strlen(pre);
    char work[NL_URI_MAX];       /* 规范化/端点内规范化共用缓冲 */
    char remalt[2];              /* 端点根的余段写法 "/" */
    const char* rem = NULL;
    int rem_in_work = 0;         /* rem 是否指向 work（work 可能被原地改写） */
    if (strncmp(path, pre, plen) == 0) {
        char c = path[plen];
        if (c == '\0' || c == '/') {
            rem = path + plen;
        } else if (c == '?' || c == '#') {
            remalt[0] = '/'; remalt[1] = '\0';
            rem = remalt;
        }
    }
    if (!rem) {
        /* 编码/折叠变体（/a/../web/x、%77eb 等）：全局规范化后再剥 */
        if (nl_uri_normalize(path, work, sizeof(work)) != NL_URI_OK)
            return NL_URI_EINVAL;
        if (strncmp(work, pre, plen) != 0 ||
            !(work[plen] == '\0' || work[plen] == '/'))
            return PL_DIR_ENOENT;   /* 防御：lookup 命中应保证在端点下 */
        rem = work + plen;
        rem_in_work = 1;
    }
    if (rem[0] == '\0') {
        remalt[0] = '/'; remalt[1] = '\0';
        rem = remalt;
        rem_in_work = 0;
    }

    /* 2) 端点内 clamp 规范化：越根 ".." 忽略（端点即根，../ 不逃逸） */
    char nrem[NL_URI_MAX];
    int rc = rem_in_work ? nl_uri_normalize_clamp(rem, rem, sizeof(work))   /* 原地 */
                         : nl_uri_normalize_clamp(rem, nrem, sizeof(nrem));
    if (rc != NL_URI_OK) return rc;
    const char* use = rem_in_work ? rem : nrem;

    /* 3) % 重编码 → resolve_file 安全拼接（canonical root + realpath 二次校验） */
    char renc[NL_URI_MAX * 2];
    if (pl_reencode_pct(use, renc, sizeof(renc)) != 0) return NL_URI_ENOSPC;
    char full[NL_URI_MAX];
    rc = nl_uri_resolve_file(route->file_path, renc, full, sizeof(full));
    if (rc != NL_URI_OK) return rc;

    /* 4) stat：文件 → 服务；目录 → 仅余段以 '/' 结尾时找 index.html */
    if (pl_stat(full, &st) != 0) return PL_DIR_ENOENT;
    if (PL_S_ISREG(st.st_mode)) {
        size_t n = strlen(full);
        if (n + 1 > cap) return NL_URI_ENOSPC;
        memcpy(out, full, n + 1);
        return NL_URI_OK;
    }
    if (!PL_S_ISDIR(st.st_mode)) return PL_DIR_ENOENT;
    size_t nlen = strlen(use);
    if (nlen == 0 || use[nlen - 1] != '/') return PL_DIR_ENOENT;

    size_t fl = strlen(full);
    if (fl + 12 > sizeof(full)) return NL_URI_ENOSPC;
    if (full[fl - 1] != '/' && full[fl - 1] != '\\')
        full[fl++] = '/';
    memcpy(full + fl, "index.html", 11);
    if (!nl_uri_is_under(route->file_path, full)) return NL_URI_EROOT;
    if (pl_stat(full, &st) != 0 || !PL_S_ISREG(st.st_mode))
        return PL_DIR_ENOENT;
    if (strlen(full) + 1 > cap) return NL_URI_ENOSPC;
    strcpy(out, full);
    return NL_URI_OK;
}

/* ============================================================
 * 分发入口
 * ============================================================ */
int nl_px_local_dispatch(void* ctx, const nl_px_local_ops* ops,
                         const char* method, const char* path, const char* authority,
                         const char* req_hdrs, size_t req_hdr_len,
                         const char* req_body, size_t req_body_len,
                         const char* peer_ip, int is_head, int idle_ms,
                         int* out_status, char** out_hdrs, size_t* out_hlen,
                         char** out_body, size_t* out_blen) {
    (void)authority;
    if (!ops || !ops->lookup || !method || !path ||
        !out_status || !out_hdrs || !out_hlen || !out_body || !out_blen)
        return NL_PX_LOCAL_ERROR;
    *out_status = 0;
    *out_hdrs = NULL;
    *out_hlen = 0;
    *out_body = NULL;
    *out_blen = 0;

    /* 第192轮：RFC 3986 端点校验——无法规范化（坏 % 编码 / 越根 / 反斜杠）
     * 直接 400；PROXY 上游转发仍用原始 path（含 query）。
     * 第203轮：改为先查路由再校验 —— 目录端点（DIR）把端点当根，越根
     * ".." 由端点内 clamp 解释（/web/../../x → 端点内 /x），不能被全局
     * ESCAPES 拦成 400；其它路由维持 normalize 失败 → 400。
     * 第203轮（补）：lookup 统一传原始 path —— 规范化会吃掉端点前缀
     * （"/web/../p" → "/p"），DIR 字面前缀将无法命中；三阶段 lookup
     * 内部已覆盖字面精确/DIR 字面前缀/规范化匹配三种情形。 */
    char norm_path[4096];
    int norm_ok = (nl_uri_normalize(path, norm_path, sizeof(norm_path)) == NL_URI_OK);
    (void)norm_path;

    nl_px_local_route route;
    memset(&route, 0, sizeof(route));
    if (!ops->lookup(ctx, path, &route)) {
        if (!norm_ok)
            return pl_simple_response(400, out_status, out_hdrs, out_hlen,
                                      out_body, out_blen);
        return pl_simple_response(404, out_status, out_hdrs, out_hlen, out_body, out_blen);
    }
    if (!norm_ok && route.type != NL_PX_LOCAL_DIR) {
        /* 非法端点只有目录端点可 clamp 放行 */
        return pl_simple_response(400, out_status, out_hdrs, out_hlen,
                                  out_body, out_blen);
    }

    if (route.type == NL_PX_LOCAL_REDIRECT) {
        int code = route.redirect_code > 0 ? route.redirect_code : 302;
        const char* loc = route.redirect_url ? route.redirect_url : "/";
        char* hdrs = (char*)malloc(strlen(loc) + 64);
        if (!hdrs) return NL_PX_LOCAL_ERROR;
        int n = snprintf(hdrs, strlen(loc) + 64, "Location: %s\r\n", loc);
        if (n <= 0) { free(hdrs); return NL_PX_LOCAL_ERROR; }
        return pl_set_response(code, hdrs, (size_t)n, NULL, 0, is_head,
                               out_status, out_hdrs, out_hlen, out_body, out_blen);
    }

    if (route.type == NL_PX_LOCAL_CONTENT) {
        size_t blen = route.content_len;
        if (blen == 0 && route.content) blen = strlen(route.content);
        if (!route.content) blen = 0;
        char* body = NULL;
        if (!is_head && blen > 0) {
            body = (char*)malloc(blen);
            if (!body) return NL_PX_LOCAL_ERROR;
            memcpy(body, route.content, blen);
        }
        const char* ct = route.content_type && route.content_type[0]
                             ? route.content_type : "text/plain; charset=utf-8";
        char* hdrs = (char*)malloc(strlen(ct) + 32);
        if (!hdrs) { free(body); return NL_PX_LOCAL_ERROR; }
        int n = snprintf(hdrs, strlen(ct) + 32, "Content-Type: %s\r\n", ct);
        if (n <= 0) { free(hdrs); free(body); return NL_PX_LOCAL_ERROR; }
        return pl_set_response(200, hdrs, (size_t)n, body, blen, is_head,
                               out_status, out_hdrs, out_hlen, out_body, out_blen);
    }

    if (route.type == NL_PX_LOCAL_FILE)
        return pl_file_response(route.file_path, route.content_type, is_head,
                                out_status, out_hdrs, out_hlen,
                                out_body, out_blen);

    /* 第203轮：DIR 端点 —— 端点即根（目录或文件），解析后按文件响应 */
    if (route.type == NL_PX_LOCAL_DIR) {
        char dpath[NL_URI_MAX];
        int rc = pl_dir_resolve(&route, path, dpath, sizeof(dpath));
        if (rc == PL_DIR_ENOENT)
            return pl_simple_response(404, out_status, out_hdrs, out_hlen,
                                      out_body, out_blen);
        if (rc == NL_URI_ESCAPES || rc == NL_URI_EROOT)
            return pl_simple_response(403, out_status, out_hdrs, out_hlen,
                                      out_body, out_blen);
        if (rc == NL_URI_ENOSPC)
            return pl_simple_response(500, out_status, out_hdrs, out_hlen,
                                      out_body, out_blen);
        if (rc != NL_URI_OK)
            return pl_simple_response(400, out_status, out_hdrs, out_hlen,
                                      out_body, out_blen);
        return pl_file_response(dpath, NULL, is_head,
                                out_status, out_hdrs, out_hlen,
                                out_body, out_blen);
    }

    if (route.type == NL_PX_LOCAL_PROXY) {
        if (route.protocol != NL_PX_LOCAL_PROTO_HTTP || !route.upstream)
            return NL_PX_LOCAL_FALLBACK;
        /* 第195轮：目录转发改写（nginx proxy_pass URI 拼接语义）。
         * up_uri 为空不改写（原样透传，热路径零拷贝）；前缀路由剥 dir_prefix，
         * 精确路由剥整段（nl_px_dir_rewrite 对 is_prefix=0 不使用 match_path
         * 内容，此处传 path 占位即可）。 */
        char rpath[4096];
        const char* fwd_path = path;
        if (route.up_uri && route.up_uri[0]) {
            const char* mp = route.dir_prefix ? route.dir_prefix : path;
            int pre = route.dir_prefix ? 1 : 0;
            if (nl_px_dir_rewrite(mp, pre, route.up_uri, path,
                                  rpath, sizeof(rpath)) > 0)
                fwd_path = rpath;
        }
        return pl_proxy_http(ops, ctx, &route, method, fwd_path,
                             req_hdrs, req_hdr_len, req_body, req_body_len,
                             peer_ip, is_head, idle_ms,
                             out_status, out_hdrs, out_hlen, out_body, out_blen);
    }

    return pl_simple_response(404, out_status, out_hdrs, out_hlen, out_body, out_blen);
}

/* ============================================================
 * HTTP/1.1 本地路由计划
 * ============================================================ */
static const char PL_PLAN_404[] =
    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
static const char PL_PLAN_500[] =
    "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
static const char PL_PLAN_400[] =
    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
static const char PL_PLAN_403[] =
    "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

static void pl_plan_status(nl_px_local_plan* out, int status, const char* head) {
    size_t n = strlen(head);
    if (n >= sizeof(out->head)) n = sizeof(out->head) - 1;
    memcpy(out->head, head, n);
    out->head[n] = '\0';
    out->head_len = n;
    out->status = status;
    out->type = NL_PX_LOCAL_CONTENT;
    out->body = NULL;
    out->body_len = 0;
    out->file_path[0] = '\0';    /* 第203轮：内嵌数组（不再是指针） */
    out->file_size = 0;
}

int nl_px_local_plan_build(void* ctx, const nl_px_local_ops* ops,
                           const char* path, int is_head, nl_px_local_plan* out) {
    if (!ops || !ops->lookup || !path || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->status = 200;

    /* 第192轮：无法规范化的端点直接产出 400 计划（RFC 3986）。
     * 第203轮：先查路由再校验 —— 目录端点越根 ".." 由端点内 clamp 解释
     * （/web/../../x → 端点内 /x），不能被全局 ESCAPES 拦成 400；
     * 其它路由维持 normalize 失败 → 400。
     * 第203轮（补）：lookup 统一传原始 path —— 规范化会吃掉端点前缀
     * （"/web/../p" → "/p"），DIR 字面前缀将无法命中；三阶段 lookup
     * 内部已覆盖字面精确/DIR 字面前缀/规范化匹配三种情形。 */
    char norm_path[4096];
    int norm_ok = (nl_uri_normalize(path, norm_path, sizeof(norm_path)) == NL_URI_OK);
    (void)norm_path;

    nl_px_local_route route;
    memset(&route, 0, sizeof(route));
    if (!ops->lookup(ctx, path, &route)) {
        if (!norm_ok) { pl_plan_status(out, 400, PL_PLAN_400); return 1; }
        return 0;     /* 未命中：平台按 404 处理 */
    }
    if (!norm_ok && route.type != NL_PX_LOCAL_DIR) {
        /* 非法端点只有目录端点可 clamp 放行 */
        pl_plan_status(out, 400, PL_PLAN_400);
        return 1;
    }
    if (route.type == NL_PX_LOCAL_PROXY) return 0;          /* 反代走平台既有池/异步路径 */

    if (route.type == NL_PX_LOCAL_REDIRECT) {
        int code = route.redirect_code > 0 ? route.redirect_code : 302;
        const char* loc = route.redirect_url ? route.redirect_url : "/";
        int n = nl_px_build_redirect_head(out->head, sizeof(out->head), code, loc);
        if (n < 0) { pl_plan_status(out, 500, PL_PLAN_500); return 1; }
        out->status = code;
        out->type = NL_PX_LOCAL_REDIRECT;
        out->head_len = (size_t)n;
        return 1;
    }

    if (route.type == NL_PX_LOCAL_CONTENT) {
        size_t blen = route.content_len;
        if (blen == 0 && route.content) blen = strlen(route.content);
        if (!route.content) blen = 0;
        const char* ct = (route.content_type && route.content_type[0])
                             ? route.content_type : "text/plain; charset=utf-8";
        int n = nl_px_build_body_head(out->head, sizeof(out->head), ct,
                                      (long long)blen, 0);
        if (n < 0) { pl_plan_status(out, 500, PL_PLAN_500); return 1; }
        out->status = 200;
        out->type = NL_PX_LOCAL_CONTENT;
        out->head_len = (size_t)n;
        if (!is_head) { out->body = route.content; out->body_len = blen; }
        return 1;
    }

    if (route.type == NL_PX_LOCAL_FILE || route.type == NL_PX_LOCAL_DIR) {
        char dpath[NL_URI_MAX];
        const char* fpath = route.file_path;
        if (route.type == NL_PX_LOCAL_DIR) {
            /* 第203轮：DIR 端点解析（端点即根，../ clamp） */
            int rc = pl_dir_resolve(&route, path, dpath, sizeof(dpath));
            if (rc == PL_DIR_ENOENT) { pl_plan_status(out, 404, PL_PLAN_404); return 1; }
            if (rc == NL_URI_ESCAPES || rc == NL_URI_EROOT) { pl_plan_status(out, 403, PL_PLAN_403); return 1; }
            if (rc == NL_URI_ENOSPC) { pl_plan_status(out, 500, PL_PLAN_500); return 1; }
            if (rc != NL_URI_OK) { pl_plan_status(out, 400, PL_PLAN_400); return 1; }
            fpath = dpath;
        }
        if (!fpath) { pl_plan_status(out, 404, PL_PLAN_404); return 1; }
        /* 第203轮：file_path 为内嵌数组（plan 会 memcpy 进延迟上下文） */
        if (strlen(fpath) >= sizeof(out->file_path)) {
            pl_plan_status(out, 500, PL_PLAN_500);
            return 1;
        }
        FILE* f = fopen(fpath, "rb");
        if (!f) { pl_plan_status(out, 404, PL_PLAN_404); return 1; }
        long long sz = -1;
        if (fseek(f, 0, SEEK_END) == 0) {
            long v = ftell(f);
            if (v >= 0) sz = (long long)v;
        }
        fclose(f);
        if (sz < 0) { pl_plan_status(out, 404, PL_PLAN_404); return 1; }
        if (sz <= 0 || sz > (long long)NL_PX_LOCAL_FILE_MAX) {
            pl_plan_status(out, 500, PL_PLAN_500);
            return 1;
        }
        const char* ct;
        if (route.content_type && route.content_type[0]) {
            ct = route.content_type;
        } else if (route.type == NL_PX_LOCAL_DIR) {
            ct = pl_guess_ct(fpath, NULL);      /* DIR：按扩展名猜 MIME */
        } else {
            ct = "application/octet-stream";
        }
        int n = nl_px_build_body_head(out->head, sizeof(out->head), ct, sz, 0);
        if (n < 0) { pl_plan_status(out, 500, PL_PLAN_500); return 1; }
        out->status = 200;
        out->type = NL_PX_LOCAL_FILE;
        out->head_len = (size_t)n;
        strcpy(out->file_path, fpath);          /* 拷贝（数组），memcpy 进 defer 安全 */
        out->file_size = sz;
        return 1;
    }

    pl_plan_status(out, 404, PL_PLAN_404);
    return 1;
}

/* ============================================================
 * 监听地址族选择
 * ============================================================ */
static int pl_env_eq_ci(const char* v, const char* lit) {
    if (!v) return 0;
    return pl_ci_eq(v, lit);
}

int nl_px_listen_mode_resolve(int configured) {
    if (configured == NL_PX_LISTEN_V4 || configured == NL_PX_LISTEN_V6)
        return configured;
    /* 注意：DUAL（0）与"未设置"共用 0，调用方需以 -1 表示未设置 */
    if (configured == NL_PX_LISTEN_DUAL) {
        /* 用户显式指定 dual 时也要能被环境变量覆盖吗？不：显式优先 */
        return NL_PX_LISTEN_DUAL;
    }
    const char* mode = getenv("NETLEAF_LISTEN_MODE");
    if (mode && mode[0]) {
        if (pl_env_eq_ci(mode, "v4") || pl_env_eq_ci(mode, "ipv4")) return NL_PX_LISTEN_V4;
        if (pl_env_eq_ci(mode, "v6") || pl_env_eq_ci(mode, "ipv6")) return NL_PX_LISTEN_V6;
        if (pl_env_eq_ci(mode, "dual") || pl_env_eq_ci(mode, "both")) return NL_PX_LISTEN_DUAL;
    }
    /* 旧开关兼容：NETLEAF_LISTEN_IPV6=0 → 仅 IPv4 */
    const char* legacy = getenv("NETLEAF_LISTEN_IPV6");
    if (legacy && legacy[0] == '0' && legacy[1] == '\0') return NL_PX_LISTEN_V4;
    return NL_PX_LISTEN_DUAL;
}
