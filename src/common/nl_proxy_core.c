/*
 * nl_proxy_core.c - 反向代理会话层实现（平台无关）
 * 语义来源：Windows 版 pr_build_upstream_http / pr_up_parse_headers /
 * pr_up_parse_chunked / pr_parse_upstream_bytes（已修好的版本），三平台共用。
 */
#include "nl_proxy_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* netleaf_module.h 中的扩展查函数（避免公共头互相包含） */
extern void* nl_extension_get_func_by_id(const char* ext_id, const char* func_name);

/* ---------------- 小工具 ---------------- */

int nl_px_ci_starts(const char* s, size_t n, const char* prefix) {
    size_t i = 0;
    if (!s || !prefix) return 0;
    for (; prefix[i]; i++) {
        if (i >= n) return 0;
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != prefix[i]) return 0;
    }
    return 1;
}

/* 在 [s, s+n) 中查找 needle（替代非标准 memmem） */
static const char* px_memfind(const char* s, size_t n, const char* needle, size_t nl) {
    if (nl == 0) return s;
    if (!s || n < nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++)
        if (s[i] == needle[0] && memcmp(s + i, needle, nl) == 0) return s + i;
    return NULL;
}

/* 忽略大小写判断 [s, s+n) 是否包含 needle（needle 为小写字面量） */
static int px_ci_has(const char* s, size_t n, const char* needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    if (!s || n < nl) return 0;
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

/* ---------------- 上下文 ---------------- */

void nl_px_ctx_init(nl_px_ctx* c, char* linebuf, size_t linecap) {
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->up_parse = NL_PX_UP_HDR;
    c->chunk_state = NL_PX_CHUNK_SIZE;
    c->linebuf = linebuf;
    c->linecap = linecap ? linecap : 8192;
}

void nl_px_ctx_reset(nl_px_ctx* c) {
    if (!c) return;
    c->up_parse = NL_PX_UP_HDR;
    c->chunk_state = NL_PX_CHUNK_SIZE;
    c->req_is_head = 0;
    c->up_cl = 0;
    c->chunk_rem = 0;
    c->up_hlen = 0;
}

/* ---------------- 请求行解析 ---------------- */

int nl_px_parse_request_line(const char* line, size_t len,
                             char* method, size_t mcap,
                             char* path, size_t pcap,
                             char* proto, size_t vcap) {
    if (!line || !method || !path || !proto || mcap == 0 || pcap == 0 || vcap == 0)
        return -1;
    method[0] = path[0] = proto[0] = '\0';
    /* 容忍调用方把 CRLF 一起传进来（旧实现把 '\r' 当成了 proto 的一部分，
     * 转发时会在请求行尾产生双 CR）。 */
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;

    size_t i = 0;
    while (i < len && line[i] != ' ') i++;
    if (i == 0 || i >= len) return -1;            /* 无 method 或只有一段 */
    size_t mlen = i;
    size_t p0 = i + 1;
    i = p0;
    while (i < len && line[i] != ' ') i++;
    if (i >= len || i == p0) return -1;           /* 无 path 或只有两段 */
    size_t plen = i - p0;
    size_t v0 = i + 1;
    if (v0 >= len) return -1;                     /* 无版本 */
    size_t vlen = len - v0;

    if (mlen >= mcap || plen >= pcap || vlen >= vcap) return -1;
    memcpy(method, line, mlen);       method[mlen] = '\0';
    memcpy(path, line + p0, plen);    path[plen] = '\0';
    memcpy(proto, line + v0, vlen);   proto[vlen] = '\0';
    return 0;
}

/* ---------------- 请求重组 ---------------- */

int nl_px_passthrough(char* out, size_t out_cap, const char* raw, size_t raw_len) {
    if (!out || !raw || raw_len == 0) return 0;
    if (raw_len > out_cap) return 0;   /* 放不下：不转发（保持整包语义） */
    memcpy(out, raw, raw_len);
    return (int)raw_len;
}

int nl_px_build_request(char* out, size_t out_cap,
                        const char* method, const char* path, const char* proto,
                        const char* upstream_host,
                        const char* raw_hdrs, size_t raw_hdrs_len,
                        const char* peer_ip) {
    if (!out || out_cap == 0 || !method || !path || !proto || !upstream_host) return -1;

    int total = snprintf(out, out_cap, "%s %s %s\r\nHost: %s\r\n",
                         method, path, proto, upstream_host);
    if (total < 0 || (size_t)total >= out_cap) return -1;

    /* 真实客户端 IP（调用方经 getpeername 取得；旧实现硬编码 127.0.0.1） */
    if (peer_ip && peer_ip[0]) {
        int n = snprintf(out + total, out_cap - (size_t)total,
                         "X-Forwarded-For: %s\r\n", peer_ip);
        if (n > 0 && (size_t)n < out_cap - (size_t)total) total += n;
    }

    /*
     * 透传客户端原始头：Host / X-Forwarded-For 已改写（避免重复），
     * Connection 原值透传（WS 升级依赖 Connection: Upgrade），未自带时补
     * keep-alive；Upgrade / Sec-WebSocket-* / Content-Length / Cookie 等全透传。
     * （旧实现 raw_hdrs 恒传 NULL，这些头全丢，WS/POST 反代不可能成立。）
     */
    int has_conn = 0;
    size_t i = 0;
    while (raw_hdrs && i < raw_hdrs_len) {
        size_t ls = i;
        while (i < raw_hdrs_len && raw_hdrs[i] != '\n') i++;
        size_t le = i;
        if (i < raw_hdrs_len) i++;                        /* 跳过 '\n' */
        if (le > ls && raw_hdrs[le - 1] == '\r') le--;
        size_t llen = le - ls;
        if (llen == 0) continue;                          /* 空行由本函数统一补 */
        size_t nl = 0;
        while (nl < llen && raw_hdrs[ls + nl] != ':') nl++;
        if (nl == 0 || nl >= llen) continue;              /* 无冒号的非法行丢弃 */
        if ((nl == 4 && nl_px_ci_starts(raw_hdrs + ls, nl, "host")) ||
            (nl == 16 && nl_px_ci_starts(raw_hdrs + ls, nl, "x-forwarded-for"))) {
            continue;
        }
        if (nl == 10 && nl_px_ci_starts(raw_hdrs + ls, nl, "connection")) has_conn = 1;
        if ((size_t)total + llen + 2 >= out_cap) break;    /* 头过长：截断 */
        memcpy(out + total, raw_hdrs + ls, llen);
        total += (int)llen;
        out[total++] = '\r';
        out[total++] = '\n';
    }
    if (!has_conn) {
        static const char ka[] = "Connection: keep-alive\r\n";
        size_t kal = sizeof(ka) - 1;
        if ((size_t)total + kal + 2 < out_cap) {
            memcpy(out + total, ka, kal);
            total += (int)kal;
        }
    }
    /* 头部空行终止符：旧实现缺失 → 真实上游一直等请求头结束，请求挂死 */
    if ((size_t)total + 2 > out_cap) return -1;
    out[total++] = '\r';
    out[total++] = '\n';
    return total;
}

/* ---------------- 响应 framing ---------------- */

static void px_parse_headers(nl_px_ctx* c, const char* hdr, int hlen) {
    const char* end = hdr + hlen;
    int status = 0;
    const char* sp = (const char*)memchr(hdr, ' ', (size_t)hlen);
    if (sp && sp + 4 <= end) {
        status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    }
    /* 101：协议升级（WS 等）。此后是裸字节双向隧道，不存在 HTTP 消息边界，
     * 不能落入 1xx 的 no_body 判定（否则会关闭客户端并污染连接池）。 */
    if (status == 101) { c->up_parse = NL_PX_UP_TUNNEL; return; }

    int no_body = (c->req_is_head != 0) ||
                  (status >= 100 && status < 200) || status == 204 || status == 304;
    int conn_close = 0;
    long long cl = -1;
    int chunked = 0;

    const char* line = px_memfind(hdr, (size_t)hlen, "\r\n", 2);
    if (line) line += 2; else line = end;
    while (line < end) {
        const char* le = px_memfind(line, (size_t)(end - line), "\r\n", 2);
        if (!le) break;
        size_t ll = (size_t)(le - line);
        if (ll == 0) break;
        if (ll >= 11 && nl_px_ci_starts(line, ll, "connection:")) {
            if (px_ci_has(line + 11, ll - 11, "close")) conn_close = 1;
        } else if (ll >= 15 && nl_px_ci_starts(line, ll, "content-length:")) {
            cl = 0; int got = 0;
            for (size_t k = 15; k < ll; k++) {
                char ch = line[k];
                if (ch >= '0' && ch <= '9') { cl = cl * 10 + (ch - '0'); got = 1; }
                else if (got) break;
                else if (ch == ' ' || ch == '\t') continue;
                else break;
            }
        } else if (ll >= 18 && nl_px_ci_starts(line, ll, "transfer-encoding:")) {
            if (px_ci_has(line + 18, ll - 18, "chunked")) chunked = 1;
        }
        line = le + 2;
    }

    if (conn_close) { c->up_parse = NL_PX_UP_EOF; return; }   /* 对端将关闭 */
    if (no_body)    { c->up_parse = NL_PX_UP_DONE; return; }
    if (cl == 0)    { c->up_parse = NL_PX_UP_DONE; return; }
    if (cl > 0) {
        c->up_cl = (int64_t)cl;
        c->up_parse = NL_PX_UP_CL;
        return;
    }
    if (chunked) {
        c->chunk_state = NL_PX_CHUNK_SIZE;
        c->chunk_rem = 0;
        c->up_hlen = 0;
        c->up_parse = NL_PX_UP_CHUNK;
        return;
    }
    /* 无 Content-Length 且非 chunked：body 以 EOF 结束 → 不可复用 */
    c->up_parse = NL_PX_UP_EOF;
}

static void px_parse_chunked(nl_px_ctx* c, const char* data, size_t* ppos, size_t len) {
    size_t pos = *ppos;
    while (pos < len) {
        uint8_t cs = c->chunk_state;
        if (cs == NL_PX_CHUNK_SIZE || cs == NL_PX_CHUNK_TRAILER) {
            char* lb = c->linebuf;
            if (!lb || c->linecap < 4) { c->up_parse = NL_PX_UP_EOF; *ppos = pos; return; }
            int lim = (int)(c->linecap - 1);
            while (pos < len) {
                char ch = data[pos++];
                if (c->up_hlen < lim) lb[c->up_hlen++] = ch;
                else { c->up_parse = NL_PX_UP_EOF; *ppos = pos; return; }
                if (ch == '\n') break;
            }
            if (c->up_hlen > 0 && lb[c->up_hlen - 1] == '\n') {
                int line_len = c->up_hlen;
                while (line_len > 0 && (lb[line_len - 1] == '\n' || lb[line_len - 1] == '\r'))
                    line_len--;
                if (cs == NL_PX_CHUNK_SIZE) {
                    long long sz = 0;
                    int ok = 0;
                    for (int i = 0; i < line_len; i++) {
                        char ch = lb[i];
                        int d;
                        if (ch == ';') break;                  /* chunk 扩展：忽略其后 */
                        else if (ch >= '0' && ch <= '9') d = ch - '0';
                        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
                        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
                        else if (ch == ' ' || ch == '\t') { if (!ok) continue; else break; }
                        else { c->up_parse = NL_PX_UP_EOF; c->up_hlen = 0; *ppos = pos; return; }
                        sz = sz * 16 + d;
                        ok = 1;
                    }
                    c->up_hlen = 0;
                    if (sz == 0) { c->chunk_state = NL_PX_CHUNK_TRAILER; }
                    else { c->chunk_rem = sz; c->chunk_state = NL_PX_CHUNK_DATA; }
                } else {
                    c->up_hlen = 0;
                    if (line_len == 0) { c->up_parse = NL_PX_UP_DONE; *ppos = pos; return; }
                }
            }
            continue;
        }
        if (cs == NL_PX_CHUNK_DATA) {
            long long take = (long long)(len - pos);
            if (take > c->chunk_rem) take = c->chunk_rem;
            pos += (size_t)take;
            c->chunk_rem -= take;
            if (c->chunk_rem == 0) { c->chunk_state = NL_PX_CHUNK_CRLF; c->chunk_rem = 2; }
            continue;
        }
        if (cs == NL_PX_CHUNK_CRLF) {
            long long take = (long long)(len - pos);
            if (take > c->chunk_rem) take = c->chunk_rem;
            pos += (size_t)take;
            c->chunk_rem -= take;
            if (c->chunk_rem == 0) c->chunk_state = NL_PX_CHUNK_SIZE;
            continue;
        }
        break;
    }
    *ppos = pos;
}

int nl_px_feed_response(nl_px_ctx* c, const char* data, size_t len) {
    if (!c || !data || len == 0) return c ? c->up_parse : NL_PX_UP_HDR;
    if (!c->linebuf) {
        c->up_parse = NL_PX_UP_EOF;
        return c->up_parse;
    }
    int lim = (int)(c->linecap >= 64 ? c->linecap - 2 : 62);

    size_t pos = 0;
    while (pos < len) {
        uint8_t st = c->up_parse;
        if (st == NL_PX_UP_DONE || st == NL_PX_UP_EOF || st == NL_PX_UP_TUNNEL)
            return st;                                       /* 终态：停止解析 */
        if (st == NL_PX_UP_HDR) {
            char* hb = c->linebuf;
            int found = 0;
            while (pos < len) {
                if (c->up_hlen >= lim) { c->up_parse = NL_PX_UP_EOF; return c->up_parse; }
                char ch = data[pos++];
                hb[c->up_hlen++] = ch;
                if (c->up_hlen >= 4 && hb[c->up_hlen - 4] == '\r' && hb[c->up_hlen - 3] == '\n' &&
                    hb[c->up_hlen - 2] == '\r' && hb[c->up_hlen - 1] == '\n') { found = 1; break; }
            }
            if (found) { px_parse_headers(c, hb, c->up_hlen); c->up_hlen = 0; }
            continue;
        }
        if (st == NL_PX_UP_CL) {
            long long rem = c->up_cl;
            long long take = (long long)(len - pos);
            if (take >= rem) { pos += (size_t)rem; c->up_cl = 0; c->up_parse = NL_PX_UP_DONE; }
            else { c->up_cl = rem - take; pos = len; }
            continue;
        }
        if (st == NL_PX_UP_CHUNK) { px_parse_chunked(c, data, &pos, len); continue; }
        return st;
    }
    return c->up_parse;
}

/* ---------------- 路由匹配 ---------------- */

int nl_px_path_match(const char* route_path, const char* req_path) {
    if (!route_path || !req_path) return 0;
    size_t plen = strcspn(req_path, "?");
    size_t rlen = strlen(route_path);
    return rlen == plen && strncmp(route_path, req_path, plen) == 0;
}

/* ---------------- 每上游连接的 framing 状态 ---------------- */

void nl_px_upstate_reset(nl_px_upstate* st) {
    if (!st) return;
    st->up_parse = NL_PX_UP_HDR;
    st->chunk_state = NL_PX_CHUNK_SIZE;
    st->req_is_head = 0;
    st->up_cl = 0;
    st->chunk_rem = 0;
    st->up_hlen = 0;
}

int nl_px_upstate_feed(nl_px_upstate* st, char* linebuf, size_t linecap,
                       const char* data, size_t len) {
    if (!st) return NL_PX_UP_HDR;
    if (!data || len == 0) return (int)st->up_parse;
    nl_px_ctx c;
    c.up_parse    = st->up_parse;
    c.chunk_state = st->chunk_state;
    c.req_is_head = st->req_is_head;
    c.up_cl       = st->up_cl;
    c.chunk_rem   = st->chunk_rem;
    c.up_hlen     = st->up_hlen;
    c.linebuf     = linebuf;
    c.linecap     = linecap < 64 ? 64 : linecap;
    nl_px_feed_response(&c, data, len);
    st->up_parse    = c.up_parse;
    st->chunk_state = c.chunk_state;
    st->up_cl       = c.up_cl;
    st->chunk_rem   = c.chunk_rem;
    st->up_hlen     = c.up_hlen;
    return (int)st->up_parse;
}

/* ---------------- 本地响应头 ---------------- */

int nl_px_build_redirect_head(char* out, size_t cap, int code, const char* location) {
    if (!out || cap == 0 || !location) return -1;
    const char* rtext = (code == 301) ? "Moved Permanently" : "Found";
    int n = snprintf(out, cap,
        "HTTP/1.1 %d %s\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        code, rtext, location);
    if (n <= 0 || (size_t)n >= cap) return -1;
    return n;
}

int nl_px_build_body_head(char* out, size_t cap, const char* content_type,
                          long long content_len, int chunked) {
    if (!out || cap == 0) return -1;
    const char* ct = (content_type && content_type[0]) ? content_type : "text/plain";
    int n;
    if (chunked) {
        n = snprintf(out, cap,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n",
            ct);
    } else {
        n = snprintf(out, cap,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lld\r\n"
            "Connection: close\r\n\r\n",
            ct, content_len);
    }
    if (n <= 0 || (size_t)n >= cap) return -1;
    return n;
}

/* ---------------- https 扩展契约 ---------------- */

static int g_tls_state = 0;                 /* 0=未探测, 1=满足, -1=不满足 */
static nl_px_tls_api g_tls_api;
static char g_tls_missing[192];

static void* px_ext_fn(const char* name) {
    return nl_extension_get_func_by_id("https", name);
}

int nl_px_tls_contract_get(nl_px_tls_api* api) {
    if (g_tls_state == 0) {
        memset(&g_tls_api, 0, sizeof(g_tls_api));
        g_tls_api.conn_wrap  = (void* (*)(int, const char*, const char**))px_ext_fn("nlh_tls_conn_wrap");
        g_tls_api.conn_read  = (int (*)(void*, void*, int))px_ext_fn("nlh_tls_conn_read");
        g_tls_api.conn_write = (int (*)(void*, const void*, int))px_ext_fn("nlh_tls_conn_write");
        g_tls_api.conn_free  = (void (*)(void*))px_ext_fn("nlh_tls_conn_free");
        g_tls_api.handshake  = (int (*)(int, const char*))px_ext_fn("nl_tls_handshake");
        if (g_tls_api.conn_wrap && g_tls_api.conn_read &&
            g_tls_api.conn_write && g_tls_api.conn_free) {
            g_tls_api.version = NL_PX_TLS_CONTRACT_VERSION;
            g_tls_api.client_impl = 1;
            g_tls_state = 1;
        } else {
            const char* miss = !g_tls_api.conn_wrap  ? "nlh_tls_conn_wrap" :
                               !g_tls_api.conn_read  ? "nlh_tls_conn_read" :
                               !g_tls_api.conn_write ? "nlh_tls_conn_write" :
                                                       "nlh_tls_conn_free";
            snprintf(g_tls_missing, sizeof(g_tls_missing),
                     "https 扩展未满足必需能力契约：缺少 %s（反代 HTTPS 上游需要完整的 TLS 会话读写能力）",
                     miss);
            g_tls_state = -1;
        }
    }
    if (g_tls_state == 1) {
        if (api) *api = g_tls_api;
        return 1;
    }
    if (api) memset(api, 0, sizeof(*api));
    return 0;
}

const char* nl_px_tls_contract_missing(void) {
    if (g_tls_state == 0) { nl_px_tls_api tmp; (void)nl_px_tls_contract_get(&tmp); }
    return g_tls_state == 1 ? "" : g_tls_missing;
}
