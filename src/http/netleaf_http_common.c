/*
 * NetLeaf HTTP/2 公共逻辑（平台无关）。
 *
 * 本文件实现 nl_http_parse_request / hpack_* / h2_* /
 * 请求响应访问器等纯逻辑函数，原先在 linux/macos/windows 三份
 * netleaf_http_*.c 中重复出现，现统一抽到这里，仅保留一份。
 *
 * 平台相关的 I/O（TCP 写 h2 帧）通过 nl_http_io_t 函数表
 * 注入，使本文件不直接依赖 <sys/socket.h> / <pthread.h> / <winsock2.h>。
 */
#include "netleaf_http_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 公共静态数据
 * ============================================================ */

/* H2 连接预流（"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"，共 24 字节）。
 * 供各平台线程比较预流，故为非 static 并由 internal 头 extern 声明。 */
const char* h2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

static const char* hpack_static_table[][2] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

/* ============================================================
 * HTTP/1 解析
 * ============================================================ */

long nl_http_find_terminator(const char* buf, size_t total) {
    if (total < 4) return -1;
    for (size_t i = 0; i + 4 <= total; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return (long)i;
    }
    return -1;
}

nlh_http_method_t nl_http_parse_method(const char* method) {
    if (strcmp(method, "GET") == 0) return NL_HTTP_GET;
    if (strcmp(method, "POST") == 0) return NL_HTTP_POST;
    if (strcmp(method, "PUT") == 0) return NL_HTTP_PUT;
    if (strcmp(method, "DELETE") == 0) return NL_HTTP_DELETE;
    if (strcmp(method, "HEAD") == 0) return NL_HTTP_HEAD;
    if (strcmp(method, "OPTIONS") == 0) return NL_HTTP_OPTIONS;
    if (strcmp(method, "PATCH") == 0) return NL_HTTP_PATCH;
    return NL_HTTP_UNKNOWN;
}

int nl_http_parse_request(nl_http_request_t* req, const char* data, size_t len) {
    memset(req, 0, sizeof(*req));
    req->version = NL_HTTP_VERSION_1_1;

    char* buffer = malloc(len + 1);
    if (!buffer) return -1;
    memcpy(buffer, data, len);
    buffer[len] = '\0';

    char* line = buffer;
    char* end = strstr(line, "\r\n");
    if (!end) {
        free(buffer);
        return -1;
    }
    *end = '\0';

    char method[16], path[NL_HTTP_MAX_PATH], version[16];
    if (sscanf(line, "%15s %4095s %15s", method, path, version) != 3) {
        free(buffer);
        return -1;
    }

    req->method = nl_http_parse_method(method);
    memcpy(req->path, path, strlen(path) < NL_HTTP_MAX_PATH ? strlen(path) + 1 : NL_HTTP_MAX_PATH - 1);
    req->path[NL_HTTP_MAX_PATH - 1] = '\0';

    if (strstr(version, "2.0") != NULL) {
        req->version = NL_HTTP_VERSION_2;
    } else if (strstr(version, "1.1") != NULL) {
        req->version = NL_HTTP_VERSION_1_1;
    } else {
        req->version = NL_HTTP_VERSION_1_0;
    }

    line = end + 2;
    while (line < buffer + len && *line) {
        end = strstr(line, "\r\n");
        if (!end) break;
        *end = '\0';

        if (strlen(line) == 0) {
            line = end + 2;
            break;
        }

        char* colon = strchr(line, ':');
        if (colon && req->header_count < MAX_HEADERS) {
            *colon = '\0';
            snprintf(req->headers[req->header_count].name, MAX_HEADER_NAME, "%s", line);
            char* value = colon + 1;
            while (*value == ' ') value++;
            snprintf(req->headers[req->header_count].value, MAX_HEADER_VALUE, "%s", value);
            req->header_count++;
        }

        line = end + 2;
    }

    if (line < buffer + len) {
        size_t body_len = buffer + len - line;
        req->body = malloc(body_len + 1);
        if (req->body) {
            memcpy(req->body, line, body_len);
            req->body[body_len] = '\0';
            req->body_size = body_len;
        }
    }

    free(buffer);
    return 0;
}

void nl_http_generate_response_http1(nl_http_response_t* resp, char** out, size_t* out_len) {
    size_t initial_size = BUFFER_SIZE + (resp->body ? resp->body_size : 0);
    char* buffer = malloc(initial_size);
    if (!buffer) {
        *out = NULL;
        *out_len = 0;
        return;
    }

    int len = snprintf(buffer, initial_size, "HTTP/1.1 %d OK\r\n", resp->status);
    if (len < 0) {
        free(buffer);
        *out = NULL;
        *out_len = 0;
        return;
    }

    int content_len_added = 0;
    for (int i = 0; i < resp->header_count; i++) {
        if (strcmp(resp->headers[i].name, "Content-Length") == 0) {
            content_len_added = 1;
        }
        len += snprintf(buffer + len, initial_size - len, "%s: %s\r\n",
                        resp->headers[i].name, resp->headers[i].value);
        /* 累加后校验返回值与剩余空间，防止 size_t 下溢导致越界写 */
        if (len < 0 || (size_t)len >= initial_size) break;
    }

    if (!content_len_added && resp->body && len >= 0 && (size_t)len < initial_size) {
        len += snprintf(buffer + len, initial_size - len, "Content-Length: %zu\r\n",
                        resp->body_size);
    }

    if (len >= 0 && (size_t)len < initial_size) {
        len += snprintf(buffer + len, initial_size - len, "\r\n");
    }

    if (resp->body && resp->body_size > 0) {
        if (len >= 0 && (size_t)len + resp->body_size < initial_size) {
            memcpy(buffer + len, resp->body, resp->body_size);
            len += resp->body_size;
        }
    }

    /* snprintf 返回的是"理论长度"，头部过多时可能大于实际容量；
     * 需夹取到实际容量内并保证 out_len <= cap，避免发送侧按 out_len 越界读取 */
    if (len < 0) {
        len = 0;
    } else if ((size_t)len > initial_size) {
        len = (int)initial_size;
    }
    *out = buffer;
    *out_len = (size_t)len;
}

/* ============================================================
 * HPACK
 * ============================================================ */

/* ------------------------------------------------------------
 * Huffman 编解码（RFC 7541 Appendix B）
 *
 * 码表与 quic 扩展的 quictls_huffman.c 同源（同一 RFC 附录），
 * 但扩展与主库不共享符号，故主库独立实现一份（HPACK 解码端
 * 必须支持 Huffman，否则无法解析主流客户端的头块）。
 * ------------------------------------------------------------ */

/* RFC 7541 Appendix B 霍夫曼码表（code, bits）；最后一项为 EOS */
static const struct { uint32_t code; uint8_t bits; } hpack_huff_code[257] = {
    { 0x1ff8, 13 }, { 0x7fffd8, 23 }, { 0xfffffe2, 28 }, { 0xfffffe3, 28 },
    { 0xfffffe4, 28 }, { 0xfffffe5, 28 }, { 0xfffffe6, 28 }, { 0xfffffe7, 28 },
    { 0xfffffe8, 28 }, { 0xffffea, 24 }, { 0x3ffffffc, 30 }, { 0xfffffe9, 28 },
    { 0xfffffea, 28 }, { 0x3ffffffd, 30 }, { 0xfffffeb, 28 }, { 0xfffffec, 28 },
    { 0xfffffed, 28 }, { 0xfffffee, 28 }, { 0xfffffef, 28 }, { 0xffffff0, 28 },
    { 0xffffff1, 28 }, { 0xffffff2, 28 }, { 0x3ffffffe, 30 }, { 0xffffff3, 28 },
    { 0xffffff4, 28 }, { 0xffffff5, 28 }, { 0xffffff6, 28 }, { 0xffffff7, 28 },
    { 0xffffff8, 28 }, { 0xffffff9, 28 }, { 0xffffffa, 28 }, { 0xffffffb, 28 },
    { 0x14, 6 }, { 0x3f8, 10 }, { 0x3f9, 10 }, { 0xffa, 12 },
    { 0x1ff9, 13 }, { 0x15, 6 }, { 0xf8, 8 }, { 0x7fa, 11 },
    { 0x3fa, 10 }, { 0x3fb, 10 }, { 0xf9, 8 }, { 0x7fb, 11 },
    { 0xfa, 8 }, { 0x16, 6 }, { 0x17, 6 }, { 0x18, 6 },
    { 0x0, 5 }, { 0x1, 5 }, { 0x2, 5 }, { 0x19, 6 },
    { 0x1a, 6 }, { 0x1b, 6 }, { 0x1c, 6 }, { 0x1d, 6 },
    { 0x1e, 6 }, { 0x1f, 6 }, { 0x5c, 7 }, { 0xfb, 8 },
    { 0x7ffc, 15 }, { 0x20, 6 }, { 0xffb, 12 }, { 0x3fc, 10 },
    { 0x1ffa, 13 }, { 0x21, 6 }, { 0x5d, 7 }, { 0x5e, 7 },
    { 0x5f, 7 }, { 0x60, 7 }, { 0x61, 7 }, { 0x62, 7 },
    { 0x63, 7 }, { 0x64, 7 }, { 0x65, 7 }, { 0x66, 7 },
    { 0x67, 7 }, { 0x68, 7 }, { 0x69, 7 }, { 0x6a, 7 },
    { 0x6b, 7 }, { 0x6c, 7 }, { 0x6d, 7 }, { 0x6e, 7 },
    { 0x6f, 7 }, { 0x70, 7 }, { 0x71, 7 }, { 0x72, 7 },
    { 0xfc, 8 }, { 0x73, 7 }, { 0xfd, 8 }, { 0x1ffb, 13 },
    { 0x7fff0, 19 }, { 0x1ffc, 13 }, { 0x3ffc, 14 }, { 0x22, 6 },
    { 0x7ffd, 15 }, { 0x3, 5 }, { 0x23, 6 }, { 0x4, 5 },
    { 0x24, 6 }, { 0x5, 5 }, { 0x25, 6 }, { 0x26, 6 },
    { 0x27, 6 }, { 0x6, 5 }, { 0x74, 7 }, { 0x75, 7 },
    { 0x28, 6 }, { 0x29, 6 }, { 0x2a, 6 }, { 0x7, 5 },
    { 0x2b, 6 }, { 0x76, 7 }, { 0x2c, 6 }, { 0x8, 5 },
    { 0x9, 5 }, { 0x2d, 6 }, { 0x77, 7 }, { 0x78, 7 },
    { 0x79, 7 }, { 0x7a, 7 }, { 0x7b, 7 }, { 0x7ffe, 15 },
    { 0x7fc, 11 }, { 0x3ffd, 14 }, { 0x1ffd, 13 }, { 0xffffffc, 28 },
    { 0xfffe6, 20 }, { 0x3fffd2, 22 }, { 0xfffe7, 20 }, { 0xfffe8, 20 },
    { 0x3fffd3, 22 }, { 0x3fffd4, 22 }, { 0x3fffd5, 22 }, { 0x7fffd9, 23 },
    { 0x3fffd6, 22 }, { 0x7fffda, 23 }, { 0x7fffdb, 23 }, { 0x7fffdc, 23 },
    { 0x7fffdd, 23 }, { 0x7fffde, 23 }, { 0xffffeb, 24 }, { 0x7fffdf, 23 },
    { 0xffffec, 24 }, { 0xffffed, 24 }, { 0x3fffd7, 22 }, { 0x7fffe0, 23 },
    { 0xffffee, 24 }, { 0x7fffe1, 23 }, { 0x7fffe2, 23 }, { 0x7fffe3, 23 },
    { 0x7fffe4, 23 }, { 0x1fffdc, 21 }, { 0x3fffd8, 22 }, { 0x7fffe5, 23 },
    { 0x3fffd9, 22 }, { 0x7fffe6, 23 }, { 0x7fffe7, 23 }, { 0xffffef, 24 },
    { 0x3fffda, 22 }, { 0x1fffdd, 21 }, { 0xfffe9, 20 }, { 0x3fffdb, 22 },
    { 0x3fffdc, 22 }, { 0x7fffe8, 23 }, { 0x7fffe9, 23 }, { 0x1fffde, 21 },
    { 0x7fffea, 23 }, { 0x3fffdd, 22 }, { 0x3fffde, 22 }, { 0xfffff0, 24 },
    { 0x1fffdf, 21 }, { 0x3fffdf, 22 }, { 0x7fffeb, 23 }, { 0x7fffec, 23 },
    { 0x1fffe0, 21 }, { 0x1fffe1, 21 }, { 0x3fffe0, 22 }, { 0x1fffe2, 21 },
    { 0x7fffed, 23 }, { 0x3fffe1, 22 }, { 0x7fffee, 23 }, { 0x7fffef, 23 },
    { 0xfffea, 20 }, { 0x3fffe2, 22 }, { 0x3fffe3, 22 }, { 0x3fffe4, 22 },
    { 0x7ffff0, 23 }, { 0x3fffe5, 22 }, { 0x3fffe6, 22 }, { 0x7ffff1, 23 },
    { 0x3ffffe0, 26 }, { 0x3ffffe1, 26 }, { 0xfffeb, 20 }, { 0x7fff1, 19 },
    { 0x3fffe7, 22 }, { 0x7ffff2, 23 }, { 0x3fffe8, 22 }, { 0x1ffffec, 25 },
    { 0x3ffffe2, 26 }, { 0x3ffffe3, 26 }, { 0x3ffffe4, 26 }, { 0x7ffffde, 27 },
    { 0x7ffffdf, 27 }, { 0x3ffffe5, 26 }, { 0xfffff1, 24 }, { 0x1ffffed, 25 },
    { 0x7fff2, 19 }, { 0x1fffe3, 21 }, { 0x3ffffe6, 26 }, { 0x7ffffe0, 27 },
    { 0x7ffffe1, 27 }, { 0x3ffffe7, 26 }, { 0x7ffffe2, 27 }, { 0xfffff2, 24 },
    { 0x1fffe4, 21 }, { 0x1fffe5, 21 }, { 0x3ffffe8, 26 }, { 0x3ffffe9, 26 },
    { 0xffffffd, 28 }, { 0x7ffffe3, 27 }, { 0x7ffffe4, 27 }, { 0x7ffffe5, 27 },
    { 0xfffec, 20 }, { 0xfffff3, 24 }, { 0xfffed, 20 }, { 0x1fffe6, 21 },
    { 0x3fffe9, 22 }, { 0x1fffe7, 21 }, { 0x1fffe8, 21 }, { 0x7ffff3, 23 },
    { 0x3fffea, 22 }, { 0x3fffeb, 22 }, { 0x1ffffee, 25 }, { 0x1ffffef, 25 },
    { 0xfffff4, 24 }, { 0xfffff5, 24 }, { 0x3ffffea, 26 }, { 0x7ffff4, 23 },
    { 0x3ffffeb, 26 }, { 0x7ffffe6, 27 }, { 0x3ffffec, 26 }, { 0x3ffffed, 26 },
    { 0x7ffffe7, 27 }, { 0x7ffffe8, 27 }, { 0x7ffffe9, 27 }, { 0x7ffffea, 27 },
    { 0x7ffffeb, 27 }, { 0xffffffe, 28 }, { 0x7ffffec, 27 }, { 0x7ffffed, 27 },
    { 0x7ffffee, 27 }, { 0x7ffffef, 27 }, { 0x7fffff0, 27 }, { 0x3ffffee, 26 },
    { 0x3fffffff, 30 }
};

/* 解码树（懒构建）：内部节点 symbol=-1；构建内容确定幂等，
 * 并发首次调用最多重复写入相同值，无正确性影响。 */
struct hpack_huff_node { int child[2]; int symbol; };
static struct hpack_huff_node hpack_huff_tree[514]; /* 257 叶 + 内部节点 ≤ 256 */
static int hpack_huff_tree_nodes = 0;
static int hpack_huff_built = 0;

static void hpack_huffman_build(void) {
    if (hpack_huff_built) return;

    hpack_huff_tree[0].child[0] = -1;
    hpack_huff_tree[0].child[1] = -1;
    hpack_huff_tree[0].symbol = -1;
    hpack_huff_tree_nodes = 1;

    for (int s = 0; s < 257; s++) {
        uint32_t code = hpack_huff_code[s].code;
        int bits = hpack_huff_code[s].bits;
        int node = 0;
        for (int b = bits - 1; b >= 0; b--) {
            int bit = (int)((code >> b) & 1u);
            if (b == 0) {
                int leaf = hpack_huff_tree_nodes++;
                hpack_huff_tree[leaf].child[0] = -1;
                hpack_huff_tree[leaf].child[1] = -1;
                hpack_huff_tree[leaf].symbol = s;
                hpack_huff_tree[node].child[bit] = leaf;
            } else {
                int next = hpack_huff_tree[node].child[bit];
                if (next < 0) {
                    next = hpack_huff_tree_nodes++;
                    hpack_huff_tree[next].child[0] = -1;
                    hpack_huff_tree[next].child[1] = -1;
                    hpack_huff_tree[next].symbol = -1;
                    hpack_huff_tree[node].child[bit] = next;
                }
                node = next;
            }
        }
    }
    hpack_huff_built = 1;
}

/* Huffman 编码（MSB-first 位流，末尾以 EOS 前缀位填充）。成功返回 0。 */
static int hpack_huffman_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len) {
    uint64_t acc = 0;
    int nbits = 0;
    size_t op = 0;

    for (size_t i = 0; i < in_len; i++) {
        uint32_t code = hpack_huff_code[in[i]].code;
        int bits = hpack_huff_code[in[i]].bits;
        acc = (acc << bits) | (uint64_t)code;
        nbits += bits;
        while (nbits >= 8) {
            nbits -= 8;
            if (op >= cap) return -1;
            out[op++] = (uint8_t)(acc >> nbits);
        }
        acc &= (nbits > 0) ? (((uint64_t)1 << nbits) - 1) : 0;
    }

    if (nbits > 0) {
        int pad = 8 - nbits;
        acc = (acc << pad) | (((uint64_t)1 << pad) - 1);
        if (op >= cap) return -1;
        out[op++] = (uint8_t)acc;
    }

    *out_len = op;
    return 0;
}

/* Huffman 解码（解码树逐位匹配）。末尾允许 <=7 位全 1 填充（EOS 前缀）；
 * 出现 EOS 码字或非法路径返回 -1。成功返回 0。 */
static int hpack_huffman_decode(const uint8_t* in, size_t in_len, uint8_t* out, size_t cap, size_t* out_len) {
    hpack_huffman_build();

    int node = 0;
    int depth = 0;       /* 自上个完整符号以来的位深 */
    int all_ones = 1;    /* 该位段是否全为 1（EOS 前缀填充校验） */
    size_t op = 0;

    for (size_t i = 0; i < in_len; i++) {
        for (int k = 7; k >= 0; k--) {
            int bit = (in[i] >> k) & 1;
            int next = hpack_huff_tree[node].child[bit];
            if (next < 0) return -1;
            node = next;
            depth++;
            if (!bit) all_ones = 0;

            if (hpack_huff_tree[node].symbol >= 0) {
                int sym = hpack_huff_tree[node].symbol;
                if (sym == 256) return -1;   /* EOS 不允许出现在码流中 */
                if (op >= cap) return -1;
                out[op++] = (uint8_t)sym;
                node = 0;
                depth = 0;
                all_ones = 1;
            }
        }
    }

    /* 残留位：要么恰好停在符号边界（node==0），要么是 <=7 位全 1 填充 */
    if (node != 0 && (depth > 7 || !all_ones)) return -1;

    *out_len = op;
    return 0;
}

/* ------------------------------------------------------------
 * HPACK 动态表（RFC 7541 §2.3.2 / §4）
 * ------------------------------------------------------------ */

/* 条目计费（RFC 7541 §4.1）：32 字节开销 + 名 + 值 */
static size_t hpack_entry_size(const char* name, const char* value) {
    return 32 + strlen(name) + strlen(value);
}

/* 淘汰最旧条目直至满足上限（表尾为最旧） */
static void hpack_table_evict(struct hpack_context* ctx) {
    while (ctx->dynamic_table_count > 0 && ctx->dynamic_table_size > ctx->max_dynamic_table_size) {
        struct hpack_dynamic_entry* last = &ctx->dynamic_table[ctx->dynamic_table_count - 1];
        ctx->dynamic_table_size -= (int)hpack_entry_size(last->name, last->value);
        ctx->dynamic_table_count--;
    }
}

/* 新条目插入表头（最近使用在前：HPACK 索引 62 → dynamic_table[0]）。
 * 单条目超上限：按 RFC 清空整表并丢弃该条目。分配失败安全降级（不插入）。 */
static int hpack_table_insert(struct hpack_context* ctx, const char* name, const char* value) {
    size_t esz = hpack_entry_size(name, value);

    if (esz > (size_t)ctx->max_dynamic_table_size) {
        ctx->dynamic_table_count = 0;
        ctx->dynamic_table_size = 0;
        return 0;
    }

    if (ctx->dynamic_table_count + 1 > ctx->dynamic_table_cap) {
        int newcap = ctx->dynamic_table_cap ? ctx->dynamic_table_cap * 2 : 16;
        int hard = ctx->max_dynamic_table_size / 32 + 1; /* 最小条目 32 字节，不可能超过 */
        if (newcap > hard) newcap = hard;
        if (newcap < ctx->dynamic_table_count + 1) newcap = ctx->dynamic_table_count + 1;
        struct hpack_dynamic_entry* p = realloc(ctx->dynamic_table, (size_t)newcap * sizeof(*p));
        if (!p) return -1;
        ctx->dynamic_table = p;
        ctx->dynamic_table_cap = newcap;
    }

    memmove(&ctx->dynamic_table[1], &ctx->dynamic_table[0],
            (size_t)ctx->dynamic_table_count * sizeof(ctx->dynamic_table[0]));
    snprintf(ctx->dynamic_table[0].name, MAX_HEADER_NAME, "%s", name);
    snprintf(ctx->dynamic_table[0].value, MAX_HEADER_VALUE, "%s", value);
    ctx->dynamic_table_count++;
    ctx->dynamic_table_size += (int)esz;
    hpack_table_evict(ctx);
    return 0;
}

/* 动态表索引查找（index >= 62；value 可为 NULL 表示只取名字） */
static int hpack_find_dynamic(struct hpack_context* ctx, uint64_t index, char* name, char* value) {
    size_t pos = (size_t)(index - 62);
    if (pos >= (size_t)ctx->dynamic_table_count) return -1;
    snprintf(name, MAX_HEADER_NAME, "%s", ctx->dynamic_table[pos].name);
    if (value) snprintf(value, MAX_HEADER_VALUE, "%s", ctx->dynamic_table[pos].value);
    return 0;
}

/* 动态表容量变更（对端 SETTINGS 的 HEADER_TABLE_SIZE）。收缩时立即淘汰。 */
static void hpack_set_max_size(struct hpack_context* ctx, uint32_t max_size) {
    if (!ctx) return;
    ctx->max_dynamic_table_size = (int)max_size;
    hpack_table_evict(ctx);
}

/* 读取一个字符串字面量（RFC 7541 §5.2）：7 位前缀长度（最高位为 H 标志），
 * H=1 时内容为 Huffman 编码。输出按 cap 截断。成功返回 0。 */
static int hpack_read_string(const uint8_t* data, size_t len, char* out, size_t cap, size_t* consumed) {
    uint64_t n;
    size_t c;
    if (len == 0 || cap == 0) return -1;
    if (hpack_read_varint(data, len, 7, &n, &c) != 0) return -1;
    if (c + n > len) return -1;

    if (data[0] & 0x80) {
        /* Huffman 编码内容：先解到临时缓冲再截断拷贝 */
        uint8_t tmp[MAX_HEADER_VALUE];
        size_t dn = 0;
        if (hpack_huffman_decode(data + c, (size_t)n, tmp, sizeof(tmp) - 1, &dn) != 0) return -1;
        tmp[dn] = '\0';
        snprintf(out, cap, "%s", (char*)tmp);
    } else {
        size_t ncpy = (size_t)n < cap - 1 ? (size_t)n : cap - 1;
        memcpy(out, data + c, ncpy);
        out[ncpy] = '\0';
    }

    *consumed = c + (size_t)n;
    return 0;
}

int hpack_read_varint(const uint8_t* data, size_t len, uint8_t prefix_bits, uint64_t* value, size_t* consumed) {
    if (len == 0) return -1;

    uint8_t prefix_mask = (1 << prefix_bits) - 1;
    *value = data[0] & prefix_mask;

    if (*value < prefix_mask) {
        *consumed = 1;
        return 0;
    }

    *consumed = 1;
    uint8_t shift = 0;

    while (*consumed < len) {
        uint8_t byte = data[*consumed];
        *value |= (uint64_t)(byte & 0x7F) << shift;
        shift += 7;
        (*consumed)++;

        if (!(byte & 0x80)) {
            return 0;
        }

        if (shift >= 64) {
            return -1;
        }
    }

    return -1;
}

int hpack_write_varint(uint8_t* data, size_t len, uint8_t prefix_bits, uint64_t value, uint8_t prefix) {
    if (len == 0) return 0;

    uint8_t prefix_mask = (1 << prefix_bits) - 1;

    if (value < prefix_mask) {
        data[0] = (prefix & ~prefix_mask) | (uint8_t)value;
        return 1;
    }

    size_t offset = 0;
    data[offset++] = (prefix & ~prefix_mask) | prefix_mask;

    value -= prefix_mask;

    while (value >= 0x80) {
        if (offset >= len) return 0;
        data[offset++] = (uint8_t)(value & 0x7F) | 0x80;
        value >>= 7;
    }

    if (offset >= len) return 0;
    data[offset++] = (uint8_t)value;

    return offset;
}

void hpack_init(struct hpack_context* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->max_dynamic_table_size = 4096;
}

void hpack_free(struct hpack_context* ctx) {
    if (!ctx) return;
    free(ctx->dynamic_table);
    ctx->dynamic_table = NULL;
    ctx->dynamic_table_size = 0;
    ctx->dynamic_table_count = 0;
    ctx->dynamic_table_cap = 0;
}

int hpack_find_static(const char* name, const char* value) {
    for (size_t i = 0; i < sizeof(hpack_static_table) / sizeof(hpack_static_table[0]); i++) {
        if (strcmp(hpack_static_table[i][0], name) == 0) {
            if (strcmp(hpack_static_table[i][1], value) == 0) {
                return i + 1;
            }
        }
    }
    for (size_t i = 0; i < sizeof(hpack_static_table) / sizeof(hpack_static_table[0]); i++) {
        if (strcmp(hpack_static_table[i][0], name) == 0) {
            return -(i + 1);
        }
    }
    return 0;
}

int hpack_decode_header(struct hpack_context* ctx, const uint8_t* data, size_t len, char* name, char* value, size_t* consumed) {
    if (len == 0) return -1;

    *consumed = 0;

    if (data[0] & 0x80) {
        /* 索引型 header 字段（RFC 7541 §6.1）：静态表 1..61，动态表 62.. */
        uint64_t index;
        size_t c;
        if (hpack_read_varint(data, len, 7, &index, &c) != 0) return -1;
        *consumed = c;

        if (index == 0) return -1; /* 索引 0 非法 */
        if (index <= 61) {
            snprintf(name, MAX_HEADER_NAME, "%s", hpack_static_table[index - 1][0]);
            snprintf(value, MAX_HEADER_VALUE, "%s", hpack_static_table[index - 1][1]);
            return 0;
        }
        if (hpack_find_dynamic(ctx, index, name, value) != 0) return -1;
        return 0;
    }

    if ((data[0] & 0xC0) == 0x40 || (data[0] & 0xF0) == 0x00 || (data[0] & 0xF0) == 0x10) {
        /* 字面量三种形态（RFC 7541 §6.2）：
         *   0x40 增量索引（6 位前缀）/ 0x00 无索引（4 位前缀）/ 0x10 永不索引（4 位前缀）。
         * name_index==0 表示新名字（随后是 name 字符串本身）。 */
        int incremental = (data[0] & 0xC0) == 0x40;
        uint8_t prefix_bits = incremental ? 6 : 4;
        uint8_t mask = (uint8_t)((1u << prefix_bits) - 1);
        uint64_t name_index = 0;
        size_t c = 0;

        if (data[0] & mask) {
            if (hpack_read_varint(data, len, prefix_bits, &name_index, &c) != 0) return -1;
            *consumed = c;
        } else {
            *consumed = 1;
        }

        if (name_index == 0) {
            if (hpack_read_string(data + *consumed, len - *consumed, name, MAX_HEADER_NAME, &c) != 0) return -1;
            *consumed += c;
        } else if (name_index <= 61) {
            snprintf(name, MAX_HEADER_NAME, "%s", hpack_static_table[name_index - 1][0]);
        } else {
            if (hpack_find_dynamic(ctx, name_index, name, NULL) != 0) return -1;
        }

        if (hpack_read_string(data + *consumed, len - *consumed, value, MAX_HEADER_VALUE, &c) != 0) return -1;
        *consumed += c;

        if (incremental) hpack_table_insert(ctx, name, value);
        return 0;
    }

    return -1;
}

/* 写入一个字符串字面量（RFC 7541 §5.2）：Huffman 编码更短时置 H 位用之。
 * 返回写入字节数；缓冲不足或编码失败返回 0（空串的原始编码至少 1 字节，无歧义）。 */
static size_t hpack_literal_string(const uint8_t* src, size_t slen, uint8_t* dst, size_t cap) {
    uint8_t huff[MAX_HEADER_VALUE * 2];
    size_t hlen = 0;

    if (slen > 0 && hpack_huffman_encode(src, slen, huff, sizeof(huff), &hlen) == 0 && hlen < slen) {
        size_t w = hpack_write_varint(dst, cap, 7, hlen, 0x80);
        if (w == 0 || w + hlen > cap) return 0;
        memcpy(dst + w, huff, hlen);
        return w + hlen;
    }

    size_t w = hpack_write_varint(dst, cap, 7, slen, 0x00);
    if (w == 0 || w + slen > cap) return 0;
    memcpy(dst + w, src, slen);
    return w + slen;
}

int hpack_encode_header(struct hpack_context* ctx, uint8_t* data, size_t len, const char* name, const char* value) {
    size_t offset = 0;

    /* 1) 完全匹配（name+value）：先静态表，再动态表 → 索引型（0x80） */
    int idx = hpack_find_static(name, value);
    if (idx > 0) {
        return hpack_write_varint(data, len, 7, (uint64_t)idx, 0x80);
    }
    for (int i = 0; i < ctx->dynamic_table_count; i++) {
        if (strcmp(ctx->dynamic_table[i].name, name) == 0 &&
            strcmp(ctx->dynamic_table[i].value, value) == 0) {
            return hpack_write_varint(data, len, 7, (uint64_t)(62 + i), 0x80);
        }
    }

    /* 2) 名字匹配：静态表（负索引）或动态表（62+i） */
    uint64_t name_index = 0;
    if (idx < 0) {
        name_index = (uint64_t)(-idx);
    } else {
        for (int i = 0; i < ctx->dynamic_table_count; i++) {
            if (strcmp(ctx->dynamic_table[i].name, name) == 0) {
                name_index = (uint64_t)(62 + i);
                break;
            }
        }
    }

    /* 3) 字面量：容量允许则增量索引（0x40，同步动态表），
     *    否则无索引（0x00）。编码端插入必须与解码端严格同步，故只在
     *    确定写 0x40 时才插入。 */
    size_t esz = hpack_entry_size(name, value);
    int use_indexing = (ctx->max_dynamic_table_size > 0) &&
                       (esz <= (size_t)ctx->max_dynamic_table_size);

    uint8_t prefix_bits = use_indexing ? 6 : 4;
    uint8_t prefix_flag = use_indexing ? 0x40 : 0x00;
    uint64_t limit = ((uint64_t)1 << prefix_bits);

    if (name_index > 0 && name_index < limit) {
        offset += hpack_write_varint(data + offset, len - offset, prefix_bits, name_index, prefix_flag);
    } else {
        /* 无名字索引，或索引超出前缀容量：退化为新名字全字面量 */
        if (offset >= len) return 0;
        data[offset++] = prefix_flag;
        name_index = 0;
    }

    if (name_index == 0) {
        size_t w = hpack_literal_string((const uint8_t*)name, strlen(name), data + offset, len - offset);
        if (w == 0) return 0;
        offset += w;
    }

    {
        size_t w = hpack_literal_string((const uint8_t*)value, strlen(value), data + offset, len - offset);
        if (w == 0) return 0;
        offset += w;
    }

    if (use_indexing) hpack_table_insert(ctx, name, value);

    return (int)offset;
}

/* ============================================================
 * HTTP/2 帧头与发送（通过 io 表写帧）
 * ============================================================ */

void h2_frame_header_write(uint8_t* data, const struct nl_h2_frame_header* header) {
    data[0] = (uint8_t)((header->length >> 16) & 0xFF);
    data[1] = (uint8_t)((header->length >> 8) & 0xFF);
    data[2] = (uint8_t)(header->length & 0xFF);
    data[3] = header->type;
    data[4] = header->flags;
    data[5] = (uint8_t)((header->stream_id >> 24) & 0x7F);
    data[6] = (uint8_t)((header->stream_id >> 16) & 0xFF);
    data[7] = (uint8_t)((header->stream_id >> 8) & 0xFF);
    data[8] = (uint8_t)(header->stream_id & 0xFF);
}

void h2_frame_header_read(const uint8_t* data, struct nl_h2_frame_header* header) {
    header->length = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | (uint32_t)data[2];
    header->type = data[3];
    header->flags = data[4];
    header->stream_id = ((uint32_t)(data[5] & 0x7F) << 24) | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 8) | (uint32_t)data[8];
}

int h2_send_settings_ack(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn) {
    (void)conn;
    uint8_t frame[9];
    struct nl_h2_frame_header header = {0};
    header.type = NL_H2_FRAME_SETTINGS;
    header.flags = 0x01;
    header.stream_id = 0;
    h2_frame_header_write(frame, &header);
    return io->write_h2_frame(io, fd, frame, 9);
}

int h2_send_settings(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn) {
    uint8_t payload[6 * 6];
    size_t offset = 0;

    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_HEADER_TABLE_SIZE;
    payload[offset++] = (conn->local_settings.header_table_size >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.header_table_size >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.header_table_size >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.header_table_size & 0xFF;

    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_ENABLE_PUSH;
    payload[offset++] = 0x00;
    payload[offset++] = 0x00;
    payload[offset++] = 0x00;
    payload[offset++] = conn->local_settings.enable_push & 0xFF;

    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_MAX_CONCURRENT_STREAMS;
    payload[offset++] = (conn->local_settings.max_concurrent_streams >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.max_concurrent_streams >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.max_concurrent_streams >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.max_concurrent_streams & 0xFF;

    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_INITIAL_WINDOW_SIZE;
    payload[offset++] = (conn->local_settings.initial_window_size >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.initial_window_size >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.initial_window_size >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.initial_window_size & 0xFF;

    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_MAX_FRAME_SIZE;
    payload[offset++] = (conn->local_settings.max_frame_size >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.max_frame_size >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.max_frame_size >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.max_frame_size & 0xFF;

    payload[offset++] = 0x00;
    payload[offset++] = NL_H2_SETTINGS_MAX_HEADER_LIST_SIZE;
    payload[offset++] = (conn->local_settings.max_header_list_size >> 24) & 0xFF;
    payload[offset++] = (conn->local_settings.max_header_list_size >> 16) & 0xFF;
    payload[offset++] = (conn->local_settings.max_header_list_size >> 8) & 0xFF;
    payload[offset++] = conn->local_settings.max_header_list_size & 0xFF;

    uint8_t frame[9 + sizeof(payload)];
    struct nl_h2_frame_header header = {0};
    header.length = sizeof(payload);
    header.type = NL_H2_FRAME_SETTINGS;
    header.flags = 0;
    header.stream_id = 0;
    h2_frame_header_write(frame, &header);
    memcpy(frame + 9, payload, sizeof(payload));

    return io->write_h2_frame(io, fd, frame, 9 + sizeof(payload));
}

int h2_send_window_update(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, uint32_t stream_id, uint32_t increment) {
    (void)conn;
    uint8_t frame[13];
    struct nl_h2_frame_header header = {0};
    header.length = 4;
    header.type = NL_H2_FRAME_WINDOW_UPDATE;
    header.flags = 0;
    header.stream_id = stream_id;
    h2_frame_header_write(frame, &header);
    frame[9] = (uint8_t)((increment >> 24) & 0x7F);
    frame[10] = (uint8_t)((increment >> 16) & 0xFF);
    frame[11] = (uint8_t)((increment >> 8) & 0xFF);
    frame[12] = (uint8_t)(increment & 0xFF);
    return io->write_h2_frame(io, fd, frame, 13);
}

int h2_send_rst_stream(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, uint32_t stream_id, uint32_t error_code) {
    (void)conn;
    uint8_t frame[13];
    struct nl_h2_frame_header header = {0};
    header.length = 4;
    header.type = NL_H2_FRAME_RST_STREAM;
    header.flags = 0;
    header.stream_id = stream_id;
    h2_frame_header_write(frame, &header);
    frame[9] = (uint8_t)((error_code >> 24) & 0xFF);
    frame[10] = (uint8_t)((error_code >> 16) & 0xFF);
    frame[11] = (uint8_t)((error_code >> 8) & 0xFF);
    frame[12] = (uint8_t)(error_code & 0xFF);
    return io->write_h2_frame(io, fd, frame, 13);
}

int h2_send_goaway(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, uint32_t last_stream_id, uint32_t error_code) {
    (void)conn;
    uint8_t frame[17];
    struct nl_h2_frame_header header = {0};
    header.length = 8;
    header.type = NL_H2_FRAME_GOAWAY;
    header.flags = 0;
    header.stream_id = 0;  /* GOAWAY 作用于连接，流 ID 恒为 0 */
    h2_frame_header_write(frame, &header);
    frame[9] = (uint8_t)((last_stream_id >> 24) & 0x7F);
    frame[10] = (uint8_t)((last_stream_id >> 16) & 0xFF);
    frame[11] = (uint8_t)((last_stream_id >> 8) & 0xFF);
    frame[12] = (uint8_t)(last_stream_id & 0xFF);
    frame[13] = (uint8_t)((error_code >> 24) & 0xFF);
    frame[14] = (uint8_t)((error_code >> 16) & 0xFF);
    frame[15] = (uint8_t)((error_code >> 8) & 0xFF);
    frame[16] = (uint8_t)(error_code & 0xFF);
    return io->write_h2_frame(io, fd, frame, 17);
}

int h2_send_headers(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, struct nl_http2_stream* stream, nl_http_response_t* resp) {
    /* 单一堆缓冲复用：前段写 HEADERS 头块（offset），后段 [9+chunk..9+chunk+chunk]
     * 作为 DATA 分块帧体，避免原 16KB(header_block) + 16KB(frame) ≈32KB 双栈占用。
     * 动态分配（按需、可回收），连接/流生命周期内复用本帧仅一次。 */
    uint8_t* frame = (uint8_t*)malloc(9 + H2_MAX_FRAME_SIZE + 9 + H2_MAX_FRAME_SIZE);
    if (!frame) return -1;
    uint8_t* header_block = frame;             /* 头块区（前 H2_MAX_FRAME_SIZE）*/
    uint8_t* data_frame   = frame + 9 + H2_MAX_FRAME_SIZE; /* DATA 分块帧区 */
    size_t offset = 0;

    char status[8];
    snprintf(status, sizeof(status), "%d", resp->status);
    /* 响应头用编码表（hpack_enc，与对端解码端同步的方向独立表） */
    offset += hpack_encode_header(&conn->hpack_enc, header_block + offset, H2_MAX_FRAME_SIZE - offset, ":status", status);

    for (int i = 0; i < resp->header_count; i++) {
        /* RFC 7540 §8.1.2：h2 字段名必须全小写。公共 API 按 HTTP/1 习惯
         * 允许 "Content-Type" 等混合大小写，编码前在此转换（截断保护）。 */
        char lname[128];
        size_t n = 0;
        for (; resp->headers[i].name[n] != '\0' && n + 1 < sizeof(lname); n++) {
            char c = resp->headers[i].name[n];
            lname[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        lname[n] = '\0';
        offset += hpack_encode_header(&conn->hpack_enc, header_block + offset, H2_MAX_FRAME_SIZE - offset,
                                     lname, resp->headers[i].value);
    }

    /* 头块过长（超出 H2 单帧上限）截断以保护帧结构；正常响应头远小于此值。 */
    if (offset > H2_MAX_FRAME_SIZE) offset = H2_MAX_FRAME_SIZE;

    int has_body = (resp->body && resp->body_size > 0);

    struct nl_h2_frame_header frame_header = {0};
    frame_header.length = offset;
    frame_header.type = NL_H2_FRAME_HEADERS;
    /* 无响应体：HEADERS 直接携带 END_STREAM，流随即关闭 */
    frame_header.flags = has_body ? 0x04 : (0x04 | 0x01);
    frame_header.stream_id = stream->id;

    h2_frame_header_write(data_frame, &frame_header);
    memcpy(data_frame + 9, header_block, offset);

    int result = io->write_h2_frame(io, fd, data_frame, 9 + offset);
    if (result < 0) { free(frame); return -1; }

    if (!has_body) {
        stream->state = NL_H2_ST_CLOSED;
    } else {
        if (stream->state == NL_H2_ST_IDLE || stream->state == NL_H2_ST_OPEN) {
            stream->state = NL_H2_ST_HALF_CLOSED_LOCAL;
        }

        /* DATA 分块上限遵从远端 SETTINGS_MAX_FRAME_SIZE（RFC 7540 §6.5.2） */
        size_t max_chunk = H2_MAX_FRAME_SIZE;
        if (conn->remote_settings.max_frame_size > 0 &&
            conn->remote_settings.max_frame_size < max_chunk) {
            max_chunk = conn->remote_settings.max_frame_size;
        }

        size_t remaining = resp->body_size;
        const char* body_ptr = resp->body;

        while (remaining > 0) {
            size_t chunk_size = remaining < max_chunk ? remaining : max_chunk;
            uint8_t flags = (remaining == chunk_size) ? 0x01 : 0x00;

            frame_header.length = chunk_size;
            frame_header.type = NL_H2_FRAME_DATA;
            frame_header.flags = flags;
            frame_header.stream_id = stream->id;

            h2_frame_header_write(data_frame, &frame_header);
            memcpy(data_frame + 9, body_ptr, chunk_size);

            result = io->write_h2_frame(io, fd, data_frame, 9 + chunk_size);
            if (result < 0) { free(frame); return -1; }

            body_ptr += chunk_size;
            remaining -= chunk_size;

            if (flags & 0x01) {
                /* 已发 END_STREAM */
                stream->state = (stream->state == NL_H2_ST_HALF_CLOSED_REMOTE)
                                    ? NL_H2_ST_CLOSED
                                    : NL_H2_ST_HALF_CLOSED_LOCAL;
            }
        }
    }

    free(frame);
    return 0;
}

struct nl_http2_stream* h2_find_or_create_stream(struct nl_http2_connection* conn, uint32_t stream_id) {
    struct nl_http2_stream* stream = conn->streams;
    while (stream) {
        if (stream->id == stream_id) return stream;
        stream = stream->next;
    }

    stream = calloc(1, sizeof(*stream));
    if (!stream) return NULL;
    stream->id = stream_id;
    stream->state = 0;
    stream->window_size = H2_DEFAULT_WINDOW_SIZE;
    stream->next = conn->streams;
    conn->streams = stream;

    return stream;
}

int h2_apply_header_block(struct nl_http2_connection* conn, struct nl_http2_stream* stream, const uint8_t* block, size_t len) {
    if (!stream) return -1;

    size_t offset = 0;
    while (offset < len) {
        char name[MAX_HEADER_NAME];
        char value[MAX_HEADER_VALUE];
        size_t consumed = 0;

        if (hpack_decode_header(&conn->hpack, block + offset, len - offset, name, value, &consumed) == 0) {
            if (consumed == 0) break;
            if (strcmp(name, ":method") == 0) {
                stream->request.method = nl_http_parse_method(value);
            } else if (strcmp(name, ":path") == 0) {
                snprintf(stream->request.path, NL_HTTP_MAX_PATH, "%s", value);
            } else if (name[0] != ':') {
                if (stream->request.header_count < MAX_HEADERS) {
                    snprintf(stream->request.headers[stream->request.header_count].name, MAX_HEADER_NAME, "%s", name);
                    snprintf(stream->request.headers[stream->request.header_count].value, MAX_HEADER_VALUE, "%s", value);
                    stream->request.header_count++;
                }
            }
        } else {
            break;
        }
        offset += consumed;
    }
    return 0;
}

int h2_process_frame(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
    if (len < 9) return -1;

    struct nl_h2_frame_header header;
    h2_frame_header_read(data, &header);

    if (header.length > len - 9) return -1;

    const uint8_t* payload = data + 9;

    switch (header.type) {
        case NL_H2_FRAME_SETTINGS:
            if (header.flags & 0x01) {
                conn->settings_ack_received = 1;
            } else {
                for (size_t i = 0; i + 6 <= header.length; i += 6) {
                    uint16_t id = ((uint16_t)payload[i] << 8) | payload[i + 1];
                    uint32_t value = ((uint32_t)payload[i + 2] << 24) | ((uint32_t)payload[i + 3] << 16) |
                                     ((uint32_t)payload[i + 4] << 8) | payload[i + 5];

                    switch (id) {
                        case NL_H2_SETTINGS_HEADER_TABLE_SIZE:
                            conn->remote_settings.header_table_size = value;
                            break;
                        case NL_H2_SETTINGS_ENABLE_PUSH:
                            conn->remote_settings.enable_push = value;
                            break;
                        case NL_H2_SETTINGS_MAX_CONCURRENT_STREAMS:
                            conn->remote_settings.max_concurrent_streams = value;
                            break;
                        case NL_H2_SETTINGS_INITIAL_WINDOW_SIZE:
                            conn->remote_settings.initial_window_size = value;
                            break;
                        case NL_H2_SETTINGS_MAX_FRAME_SIZE:
                            conn->remote_settings.max_frame_size = value;
                            break;
                        case NL_H2_SETTINGS_MAX_HEADER_LIST_SIZE:
                            conn->remote_settings.max_header_list_size = value;
                            break;
                    }
                }
                /* 远端调小动态表上限时须同步收缩编码表，保持与对端解码端一致 */
                hpack_set_max_size(&conn->hpack_enc, conn->remote_settings.header_table_size);
                h2_send_settings_ack(io, fd, conn);
            }
            break;

        case NL_H2_FRAME_HEADERS: {
            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
            if (!stream) return -1;

            stream->request.version = NL_HTTP_VERSION_2;

            /* 流状态机（服务端视角）：IDLE→OPEN；END_STREAM 表示客户端
             * 不再发 DATA，置 HALF_CLOSED_REMOTE。 */
            if (stream->state == NL_H2_ST_IDLE)
                stream->state = NL_H2_ST_OPEN;
            if (header.flags & 0x01)  /* END_STREAM */
                stream->state = NL_H2_ST_HALF_CLOSED_REMOTE;

            /* END_HEADERS(0x04) 置位：头块完整于本帧；未置位：后续由 CONTINUATION 续帧。 */
            int end_headers = (header.flags & 0x04) != 0;

            if (!end_headers) {
                /* 累积已到达的头部块，等待 CONTINUATION 补全后一次性解析 */
                size_t need = (conn->pending_headers_len ? conn->pending_headers_len : 0) + header.length;
                uint8_t* buf = realloc(conn->pending_headers_block, need + 1);
                if (!buf) return -1;
                memcpy(buf + conn->pending_headers_len, payload, header.length);
                conn->pending_headers_block = buf;
                conn->pending_headers_len = need;
                conn->pending_headers_stream = header.stream_id;
                break;
            }

            if (h2_apply_header_block(conn, stream, payload, header.length) < 0) {
                return -1;
            }

            if (handler) {
                nl_http_response_t resp = {0};
                resp.status = 200;
                handler(&stream->request, &resp, user_data);
                h2_send_headers(io, fd, conn, stream, &resp);

                if (resp.body) free(resp.body);
            }
            break;
        }

        case NL_H2_FRAME_CONTINUATION: {
            /* 无流/无累积中的头块：协议违规，忽略 */
            if (header.stream_id == 0 || !conn->pending_headers_block) break;
            if (conn->pending_headers_stream != header.stream_id) break;

            int end_headers = (header.flags & 0x04) != 0;

            if (!end_headers) {
                /* 仍未结束，继续累积（本帧是中间 CONTINUATION） */
                size_t need = conn->pending_headers_len + header.length;
                uint8_t* buf = realloc(conn->pending_headers_block, need + 1);
                if (!buf) return -1;
                memcpy(buf + conn->pending_headers_len, payload, header.length);
                conn->pending_headers_block = buf;
                conn->pending_headers_len = need;
                break;
            }

            /* END_HEADERS：解析已累积的完整头块 */
            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
            if (!stream) return -1;
            stream->request.version = NL_HTTP_VERSION_2;

            if (h2_apply_header_block(conn, stream, conn->pending_headers_block, conn->pending_headers_len) < 0) {
                free(conn->pending_headers_block);
                conn->pending_headers_block = NULL;
                conn->pending_headers_len = 0;
                conn->pending_headers_stream = 0;
                return -1;
            }

            free(conn->pending_headers_block);
            conn->pending_headers_block = NULL;
            conn->pending_headers_len = 0;
            conn->pending_headers_stream = 0;

            if (handler) {
                nl_http_response_t resp = {0};
                resp.status = 200;
                handler(&stream->request, &resp, user_data);
                h2_send_headers(io, fd, conn, stream, &resp);
                if (resp.body) free(resp.body);
            }
            break;
        }

        case NL_H2_FRAME_PUSH_PROMISE: {
            /* 服务端→客户端预留流 + promise 头块（:path/:method）。
             * 这里按 H2 语义创建预留流并解析 promise 头块；若远端
             * 禁止 PUSH（本地 enable_push=0）则忽略。 */
            if (!conn->local_settings.enable_push) break;

            if (header.length < 4) break;
            /* PRIORITY 标志时 payload 首 4 字节为依赖流/优先级 */
            size_t offset = (header.flags & 0x02) ? 4 : 0;
            uint32_t promised_stream = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                       ((uint32_t)payload[2] << 8) | payload[3];
            offset += 4;

            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, promised_stream);
            if (!stream) return -1;
            stream->request.version = NL_HTTP_VERSION_2;

            /* promise 头块（仅 :method/:path）解析进 request；不触发 handler */
            if (offset < header.length) {
                h2_apply_header_block(conn, stream, payload + offset, header.length - offset);
            }
            break;
        }

        case NL_H2_FRAME_DATA: {
            struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
            if (!stream) return -1;

            if (header.length > 0) {
                char* new_body = realloc(stream->request.body, stream->request.body_size + header.length);
                if (new_body) {
                    memcpy(new_body + stream->request.body_size, payload, header.length);
                    stream->request.body = new_body;
                    stream->request.body_size += header.length;
                }
            }

            h2_send_window_update(io, fd, conn, header.stream_id, header.length);
            h2_send_window_update(io, fd, conn, 0, header.length);

            if (header.flags & 0x01)  /* END_STREAM：请求体收完 */
                stream->state = NL_H2_ST_HALF_CLOSED_REMOTE;
            break;
        }

        case NL_H2_FRAME_WINDOW_UPDATE:
            if (header.length >= 4) {
                if (header.stream_id == 0) {
                    conn->connection_window_size += ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                                    ((uint32_t)payload[2] << 8) | payload[3];
                } else {
                    struct nl_http2_stream* stream = h2_find_or_create_stream(conn, header.stream_id);
                    if (stream) {
                        stream->window_size += ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                               ((uint32_t)payload[2] << 8) | payload[3];
                    }
                }
            }
            break;

        case NL_H2_FRAME_PING:
            if (!(header.flags & 0x01)) {
                struct nl_h2_frame_header ack_header = header;
                ack_header.flags = 0x01;
                uint8_t frame[17];
                h2_frame_header_write(frame, &ack_header);
                if (payload + 8 <= data + len)
                    memcpy(frame + 9, payload, 8);
                io->write_h2_frame(io, fd, frame, 17);  /* PING ACK 写失败可忽略 */
            }
            break;

        case NL_H2_FRAME_PRIORITY:
            /* RFC 7540 §6.3：服务端可安全忽略优先级信息，仅需消费帧 */
            break;

        case NL_H2_FRAME_RST_STREAM: {
            /* 仅搜索已有流，不创建（对未知流的 RST 无需理会） */
            struct nl_http2_stream* stream = conn->streams;
            while (stream && stream->id != header.stream_id) stream = stream->next;
            if (stream) {
                stream->state = NL_H2_ST_CLOSED;
                if (stream->request.body) {
                    free(stream->request.body);
                    stream->request.body = NULL;
                    stream->request.body_size = 0;
                }
            }
            break;
        }

        case NL_H2_FRAME_GOAWAY:
            /* 收到 GOAWAY：对端不再接受新流，置 draining（仍处理已开的流） */
            conn->draining = 1;
            if (header.length >= 8) {
                conn->goaway_last_stream = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                           ((uint32_t)payload[2] << 8) | payload[3];
            }
            break;
    }

    return 0;
}

/* ============================================================
 * 连接初始化（common 部分：默认值 + hpack）
 *
 * 锁的初始化（pthread_mutex_init / InitializeCriticalSection）属于平台
 * 行为，由各平台 .c 在调用本函数后通过 NL_HTTP_MUTEX_LOCK 宏补齐；
 * 此处保持平台无关，避免直接依赖 <pthread.h>/<winsock2.h>。
 * ============================================================ */

void h2_connection_init(struct nl_http2_connection* conn, nl_http_handle_t fd) {
    if (!conn) return;
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;

    conn->local_settings.header_table_size = 4096;
    conn->local_settings.enable_push = 0;
    conn->local_settings.max_concurrent_streams = 100;
    conn->local_settings.initial_window_size = H2_DEFAULT_WINDOW_SIZE;
    conn->local_settings.max_frame_size = H2_MAX_FRAME_SIZE;
    conn->local_settings.max_header_list_size = 16384;

    conn->connection_window_size = H2_DEFAULT_WINDOW_SIZE;
    conn->preface_sent = 0;
    conn->settings_ack_received = 0;
    conn->draining = 0;
    conn->goaway_last_stream = 0;
    conn->streams = NULL;

    /* 远端初始参数为 RFC 7540 §6.5.2 默认值（未收到 SETTINGS 前生效）：
     * 无并发流限制用 0xFFFFFFFF 表示；max_header_list_size=0 表示无限制。 */
    conn->remote_settings.header_table_size = 4096;
    conn->remote_settings.enable_push = 1;
    conn->remote_settings.max_concurrent_streams = 0xFFFFFFFFu;
    conn->remote_settings.initial_window_size = H2_DEFAULT_WINDOW_SIZE;
    conn->remote_settings.max_frame_size = H2_MAX_FRAME_SIZE;
    conn->remote_settings.max_header_list_size = 0;

    hpack_init(&conn->hpack);
    hpack_init(&conn->hpack_enc);
    /* 编码表初始上限跟随远端默认动态表容量（4096），收到 SETTINGS 后再调整 */
    conn->hpack_enc.max_dynamic_table_size = 4096;
}

void h2_connection_cleanup(struct nl_http2_connection* conn) {
    if (!conn) return;

    hpack_free(&conn->hpack);
    hpack_free(&conn->hpack_enc);

    if (conn->pending_headers_block) {
        free(conn->pending_headers_block);
        conn->pending_headers_block = NULL;
        conn->pending_headers_len = 0;
        conn->pending_headers_stream = 0;
    }

    struct nl_http2_stream* stream = conn->streams;
    while (stream) {
        struct nl_http2_stream* next = stream->next;
        if (stream->request.body) free(stream->request.body);
        if (stream->response.body) free(stream->response.body);
        free(stream);
        stream = next;
    }
    conn->streams = NULL;
}

/* ============================================================
 * 请求 / 响应 访问器
 * ============================================================ */

nlh_http_method_t nl_http_request_get_method(const nl_http_request_t* req) {
    if (!req) return NL_HTTP_UNKNOWN;
    return req->method;
}

nl_http_version_t nl_http_request_get_version(const nl_http_request_t* req) {
    if (!req) return NL_HTTP_VERSION_1_1;
    return req->version;
}

const char* nl_http_request_get_path(const nl_http_request_t* req) {
    if (!req) return NULL;
    return req->path;
}

const char* nl_http_request_get_header(const nl_http_request_t* req, const char* name) {
    if (!req || !name) return NULL;
    for (int i = 0; i < req->header_count; i++) {
        if (strcmp(req->headers[i].name, name) == 0) {
            return req->headers[i].value;
        }
    }
    return NULL;
}

const char* nl_http_request_get_body(const nl_http_request_t* req) {
    if (!req) return NULL;
    return req->body;
}

size_t nl_http_request_get_body_size(const nl_http_request_t* req) {
    if (!req) return 0;
    return req->body_size;
}

void nl_http_response_set_status(nl_http_response_t* resp, int status) {
    if (resp) resp->status = status;
}

void nl_http_response_set_header(nl_http_response_t* resp, const char* name, const char* value) {
    if (!resp || !name || !value || resp->header_count >= MAX_HEADERS) return;

    snprintf(resp->headers[resp->header_count].name, MAX_HEADER_NAME, "%s", name);
    snprintf(resp->headers[resp->header_count].value, MAX_HEADER_VALUE, "%s", value);
    resp->header_count++;
}

void nl_http_response_set_body(nl_http_response_t* resp, const char* body, size_t len) {
    if (!resp) return;

    if (resp->body) free(resp->body);
    resp->body = malloc(len + 1);
    if (resp->body) {
        memcpy(resp->body, body, len);
        resp->body[len] = '\0';
        resp->body_size = len;
    }
}

/* ============================================================
 * 帧编解码回环（端对端联调）
 *
 * 纯内存，供单元测试 / 联调校验编解码正确性。
 * ============================================================ */

int h2_frame_roundtrip(uint8_t* data, size_t data_cap,
                       const struct nl_h2_frame_header* header,
                       const uint8_t* payload, size_t payload_len) {
    if (!data || !header || payload_len > H2_MAX_FRAME_SIZE) return 0;
    if (data_cap < 9 + payload_len) return 0;

    h2_frame_header_write(data, header);
    if (payload_len > 0) memcpy(data + 9, payload, payload_len);

    struct nl_h2_frame_header back;
    h2_frame_header_read(data, &back);
    if (back.length != header->length || back.type != header->type ||
        back.flags != header->flags || back.stream_id != header->stream_id) {
        return 0;
    }
    if (header->length != payload_len) return 0;
    if (payload_len > 0 && back.length > 0 &&
        memcmp(data + 9, payload, back.length) != 0) return 0;
    return 1;
}
