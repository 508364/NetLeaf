/**
 * @file quictls_qpack.c
 * @brief QPACK 字段行编解码（RFC 9204，静态表；无动态表 / 无霍夫曼）
 * @version 0.1.0
 *
 * 覆盖 RFC 9204 §4.5 的三种字段行（静态表范围内）：
 *   - Indexed Field Line（1Txxxxxx，T=1 静态表）
 *   - Literal Field Line With Name Reference（01NTxxxxx，T=1 静态表）
 *   - Literal Field Line With Literal Name（001NHxxx）
 * 以及 §4.1.1 前缀整数编解码。
 *
 * 未实现：动态表、霍夫曼编码（H 位）。编码端始终 H=0。
 * 静态表：RFC 9204 Appendix A（99 项）。
 */

#include "netleaf_quictls.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* RFC 9204 Appendix A 静态表（name, value） */
static const char* QPACK_STATIC[99][2] = {
    {":authority", ""}, {":path", "/"}, {"age", "0"}, {"content-disposition", ""},
    {"content-length", "0"}, {"cookie", ""}, {"date", ""}, {"etag", ""},
    {"if-modified-since", ""}, {"if-none-match", ""}, {"last-modified", ""}, {"link", ""},
    {"location", ""}, {"referer", ""}, {"set-cookie", ""},
    {":method", "CONNECT"}, {":method", "DELETE"}, {":method", "GET"}, {":method", "HEAD"},
    {":method", "OPTIONS"}, {":method", "POST"}, {":method", "PUT"},
    {":scheme", "http"}, {":scheme", "https"},
    {":status", "103"}, {":status", "200"}, {":status", "304"}, {":status", "404"}, {":status", "503"},
    {"accept", "*/*"}, {"accept", "application/dns-message"},
    {"accept-encoding", "gzip, deflate, br"}, {"accept-ranges", "bytes"},
    {"access-control-allow-headers", "cache-control"}, {"access-control-allow-headers", "content-type"},
    {"access-control-allow-origin", "*"},
    {"cache-control", "max-age=0"}, {"cache-control", "max-age=2592000"},
    {"cache-control", "max-age=604800"}, {"cache-control", "no-cache"},
    {"cache-control", "no-store"}, {"cache-control", "public, max-age=31536000"},
    {"content-encoding", "br"}, {"content-encoding", "gzip"},
    {"content-type", "application/dns-message"}, {"content-type", "application/javascript"},
    {"content-type", "application/json"}, {"content-type", "application/x-www-form-urlencoded"},
    {"content-type", "image/gif"}, {"content-type", "image/jpeg"}, {"content-type", "image/png"},
    {"content-type", "text/css"}, {"content-type", "text/html; charset=utf-8"},
    {"content-type", "text/plain"}, {"content-type", "text/plain;charset=utf-8"},
    {"range", "bytes=0-"},
    {"strict-transport-security", "max-age=31536000"},
    {"strict-transport-security", "max-age=31536000; includesubdomains"},
    {"strict-transport-security", "max-age=31536000; includesubdomains; preload"},
    {"vary", "accept-encoding"}, {"vary", "origin"},
    {"x-content-type-options", "nosniff"}, {"x-xss-protection", "1; mode=block"},
    {":status", "100"}, {":status", "204"}, {":status", "206"}, {":status", "302"},
    {":status", "400"}, {":status", "403"}, {":status", "421"}, {":status", "425"}, {":status", "500"},
    {"accept-language", ""},
    {"access-control-allow-credentials", "FALSE"}, {"access-control-allow-credentials", "TRUE"},
    {"access-control-allow-headers", "*"},
    {"access-control-allow-methods", "get"}, {"access-control-allow-methods", "get, post, options"},
    {"access-control-allow-methods", "options"},
    {"access-control-expose-headers", "content-length"},
    {"access-control-request-headers", "content-type"},
    {"access-control-request-method", "get"}, {"access-control-request-method", "post"},
    {"alt-svc", "clear"}, {"authorization", ""},
    {"content-security-policy", "script-src 'none'; object-src 'none'; base-uri 'none'"},
    {"early-data", "1"}, {"expect-ct", ""}, {"forwarded", ""}, {"if-range", ""}, {"origin", ""},
    {"purpose", "prefetch"}, {"server", ""}, {"timing-allow-origin", "*"},
    {"upgrade-insecure-requests", "1"}, {"user-agent", ""}, {"x-forwarded-for", ""},
    {"x-frame-options", "deny"}, {"x-frame-options", "sameorigin"}
};

/* 前缀整数（RFC 9204 §4.1.1） */
static size_t qp_write_int(uint8_t* p, size_t cap, uint64_t v, int prefix, uint8_t high) {
    uint64_t max = (1ULL << prefix) - 1;
    if (cap < 1) return 0;
    if (v < max) { p[0] = (uint8_t)(high | v); return 1; }
    p[0] = (uint8_t)(high | max);
    size_t n = 1;
    v -= max;
    while (v >= 128) { if (n >= cap) return 0; p[n++] = (uint8_t)((v & 0x7f) | 0x80); v >>= 7; }
    if (n >= cap) return 0;
    p[n++] = (uint8_t)v;
    return n;
}

static size_t qp_read_int(const uint8_t* p, size_t len, int prefix, uint64_t* v) {
    if (len < 1) return 0;
    uint64_t max = (1ULL << prefix) - 1;
    uint64_t r = p[0] & max;
    if (r < max) { *v = r; return 1; }
    size_t n = 1;
    uint64_t shift = 0;
    while (n < len) {
        uint8_t b = p[n++];
        r += (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) { *v = r; return n; }
        if (shift > 56) return 0;
    }
    return 0;
}

static int static_find_exact(const char* name, const char* value) {
    for (int i = 0; i < 99; i++)
        if (strcmp(QPACK_STATIC[i][0], name) == 0 && strcmp(QPACK_STATIC[i][1], value) == 0)
            return i;
    return -1;
}

static int static_find_name(const char* name) {
    for (int i = 0; i < 99; i++)
        if (strcmp(QPACK_STATIC[i][0], name) == 0) return i;
    return -1;
}

int nl_h3_qpack_encode_field(uint8_t* out, size_t cap, const char* name, const char* value) {
    if (!out || !name || !value) return -1;
    size_t p = 0, n;

    /* 1) 精确匹配 → Indexed Field Line（静态） */
    int idx = static_find_exact(name, value);
    if (idx >= 0) {
        n = qp_write_int(out, cap, (uint64_t)idx, 6, 0xC0);
        return n ? (int)n : -1;
    }

    /* 2) 名称匹配 → Literal with Name Ref（静态，N=0） */
    int nidx = static_find_name(name);
    if (nidx >= 0) {
        n = qp_write_int(out + p, cap - p, (uint64_t)nidx, 4, 0x50);   /* 0101 0000 */
        if (!n) return -1; p += n;
    } else {
        /* 3) Literal with Literal Name（N=0，H=0） */
        size_t nl = strlen(name);
        n = qp_write_int(out + p, cap - p, nl, 3, 0x20);               /* 0010 0000 */
        if (!n) return -1; p += n;
        if (p + nl > cap) return -1;
        memcpy(out + p, name, nl); p += nl;
    }

    size_t vl = strlen(value);
    n = qp_write_int(out + p, cap - p, vl, 7, 0x00);                   /* H=0 Value Len */
    if (!n) return -1; p += n;
    if (p + vl > cap) return -1;
    memcpy(out + p, value, vl); p += vl;
    return (int)p;
}

/* 前向声明（实现在下方，支持动态表） */
int nl_h3_qpack_decode_dt(const uint8_t* d, size_t len, const nl_h3_qpack_dt_t* dt,
                          nl_h3_qpack_field_t* out, int max, int* count);

int nl_h3_qpack_decode(const uint8_t* d, size_t len,
                       nl_h3_qpack_field_t* out, int max, int* count) {
    return nl_h3_qpack_decode_dt(d, len, NULL, out, max, count);
}

int nl_h3_qpack_static_get(uint64_t index, char* name, size_t ncap, char* value, size_t vcap) {
    if (index > 98) return -1;
    if (name && ncap)  snprintf(name, ncap, "%s", QPACK_STATIC[index][0]);
    if (value && vcap) snprintf(value, vcap, "%s", QPACK_STATIC[index][1]);
    return 0;
}

const char* nl_h3_qpack_get(const nl_h3_qpack_field_t* fields, int count, const char* name) {
    if (!fields || !name) return NULL;
    for (int i = 0; i < count; i++)
        if (strcmp(fields[i].name, name) == 0) return fields[i].value;
    return NULL;
}

/* ============================================================
 * QPACK 动态表（RFC 9204 §3）与指令（§4.3/§4.4）
 * ============================================================ */
#define QPACK_DT_MAX 64

struct nl_h3_qpack_dt {
    uint64_t capacity, size, insert_count;
    struct { char name[64]; char value[256]; size_t sz; } e[QPACK_DT_MAX];
    size_t count;   /* e[0..count-1]，e[count-1] 为最新 */
};

static size_t dt_entry_size(const char* n, const char* v) { return strlen(n) + strlen(v) + 32; }

nl_h3_qpack_dt_t* nl_h3_qpack_dt_new(uint64_t capacity) {
    nl_h3_qpack_dt_t* dt = (nl_h3_qpack_dt_t*)calloc(1, sizeof(nl_h3_qpack_dt_t));
    if (dt) dt->capacity = capacity;
    return dt;
}
void nl_h3_qpack_dt_free(nl_h3_qpack_dt_t* dt) { free(dt); }

static void dt_evict_front(nl_h3_qpack_dt_t* dt) {
    if (dt->count == 0) return;
    dt->size -= dt->e[0].sz;
    for (size_t i = 1; i < dt->count; i++) dt->e[i - 1] = dt->e[i];
    dt->count--;
}

int nl_h3_qpack_dt_set_capacity(nl_h3_qpack_dt_t* dt, uint64_t capacity) {
    if (!dt) return -1;
    dt->capacity = capacity;
    while (dt->count > 0 && dt->size > dt->capacity) dt_evict_front(dt);
    return 0;
}
uint64_t nl_h3_qpack_dt_size(const nl_h3_qpack_dt_t* dt) { return dt ? dt->size : 0; }
uint64_t nl_h3_qpack_dt_insert_count(const nl_h3_qpack_dt_t* dt) { return dt ? dt->insert_count : 0; }

int nl_h3_qpack_dt_insert(nl_h3_qpack_dt_t* dt, const char* name, const char* value) {
    if (!dt || !name || !value) return -1;
    size_t sz = dt_entry_size(name, value);
    if (sz > dt->capacity) { while (dt->count > 0) dt_evict_front(dt); return -1; }
    while (dt->count > 0 && dt->size + sz > dt->capacity) dt_evict_front(dt);
    if (dt->count >= QPACK_DT_MAX) dt_evict_front(dt);
    snprintf(dt->e[dt->count].name, 64, "%s", name);
    snprintf(dt->e[dt->count].value, 256, "%s", value);
    dt->e[dt->count].sz = sz;
    dt->count++;
    dt->size += sz;
    dt->insert_count++;
    return 0;
}

int nl_h3_qpack_dt_get(const nl_h3_qpack_dt_t* dt, uint64_t rel_index,
                       char* name, size_t ncap, char* value, size_t vcap) {
    if (!dt || rel_index >= dt->count) return -1;
    size_t idx = dt->count - 1 - (size_t)rel_index;
    if (name && ncap) snprintf(name, ncap, "%s", dt->e[idx].name);
    if (value && vcap) snprintf(value, vcap, "%s", dt->e[idx].value);
    return 0;
}

/* 编码器指令 */
int nl_h3_qpack_enc_set_capacity(uint8_t* out, size_t cap, uint64_t capacity) {
    size_t n = qp_write_int(out, cap, capacity, 5, 0x20);   /* 001 */
    return n ? (int)n : -1;
}
int nl_h3_qpack_enc_insert_nameref(uint8_t* out, size_t cap, int is_static,
                                   uint64_t name_index, const char* value) {
    size_t p = 0, n;
    n = qp_write_int(out + p, cap - p, name_index, 6, is_static ? 0xC0 : 0x80);  /* 1 T */
    if (!n) return -1; p += n;
    size_t vl = strlen(value);
    n = qp_write_int(out + p, cap - p, vl, 7, 0x00);        /* H=0 */
    if (!n) return -1; p += n;
    if (p + vl > cap) return -1;
    memcpy(out + p, value, vl); p += vl;
    return (int)p;
}
int nl_h3_qpack_enc_insert_literal(uint8_t* out, size_t cap, const char* name, const char* value) {
    size_t p = 0, n;
    size_t nl = strlen(name);
    n = qp_write_int(out + p, cap - p, nl, 5, 0x40);        /* 01 */
    if (!n) return -1; p += n;
    if (p + nl > cap) return -1;
    memcpy(out + p, name, nl); p += nl;
    size_t vl = strlen(value);
    n = qp_write_int(out + p, cap - p, vl, 7, 0x00);
    if (!n) return -1; p += n;
    if (p + vl > cap) return -1;
    memcpy(out + p, value, vl); p += vl;
    return (int)p;
}

/* 解码器指令 */
int nl_h3_qpack_dec_section_ack(uint8_t* out, size_t cap, uint64_t stream_id) {
    size_t n = qp_write_int(out, cap, stream_id, 7, 0x80);  /* 1 */
    return n ? (int)n : -1;
}
int nl_h3_qpack_dec_stream_cancel(uint8_t* out, size_t cap, uint64_t stream_id) {
    size_t n = qp_write_int(out, cap, stream_id, 6, 0x40);  /* 01 */
    return n ? (int)n : -1;
}
int nl_h3_qpack_dec_insert_count_increment(uint8_t* out, size_t cap, uint64_t inc) {
    size_t n = qp_write_int(out, cap, inc, 6, 0x00);        /* 00 */
    return n ? (int)n : -1;
}

/* --- 结合动态表的字段行（T=0 静态相对索引；无 Base/Post-Base 跟踪，简化模型） --- */
static int dt_find_exact(const nl_h3_qpack_dt_t* dt, const char* n, const char* v, uint64_t* rel) {
    for (uint64_t r = 0; r < dt->count; r++) {
        char bn[64], bv[256];
        if (nl_h3_qpack_dt_get(dt, r, bn, sizeof(bn), bv, sizeof(bv)) != 0) continue;
        if (strcmp(bn, n) == 0 && strcmp(bv, v) == 0) { *rel = r; return 1; }
    }
    return 0;
}
static int dt_find_name(const nl_h3_qpack_dt_t* dt, const char* n, uint64_t* rel) {
    for (uint64_t r = 0; r < dt->count; r++) {
        char bn[64];
        if (nl_h3_qpack_dt_get(dt, r, bn, sizeof(bn), NULL, 0) != 0) continue;
        if (strcmp(bn, n) == 0) { *rel = r; return 1; }
    }
    return 0;
}

int nl_h3_qpack_encode_field_dt(uint8_t* out, size_t cap, const nl_h3_qpack_dt_t* dt,
                                const char* name, const char* value) {
    if (!out || !name || !value) return -1;
    if (dt) {
        uint64_t rel = 0;
        if (dt_find_exact(dt, name, value, &rel)) {
            size_t n = qp_write_int(out, cap, rel, 6, 0x80);       /* Indexed Field Line，T=0 */
            return n ? (int)n : -1;
        }
        if (dt_find_name(dt, name, &rel)) {
            size_t p = 0, n;
            n = qp_write_int(out + p, cap - p, rel, 4, 0x40);      /* 01 N=0 T=0 */
            if (!n) return -1; p += n;
            size_t vl = strlen(value);
            n = qp_write_int(out + p, cap - p, vl, 7, 0x00);
            if (!n) return -1; p += n;
            if (p + vl > cap) return -1;
            memcpy(out + p, value, vl); p += vl;
            return (int)p;
        }
    }
    return nl_h3_qpack_encode_field(out, cap, name, value);
}

int nl_h3_qpack_decode_dt(const uint8_t* d, size_t len, const nl_h3_qpack_dt_t* dt,
                          nl_h3_qpack_field_t* out, int max, int* count) {
    if (!d || !out || !count) return -1;
    size_t p = 0;
    int c = 0;
    while (p < len) {
        uint8_t b = d[p];
        if (b & 0x80) {                         /* Indexed Field Line */
            uint64_t idx = 0;
            size_t n = qp_read_int(d + p, len - p, 6, &idx);
            if (!n) return -1; p += n;
            if (b & 0x40) {                     /* T=1 静态 */
                if (idx > 98) return -1;
                if (c < max) {
                    snprintf(out[c].name, sizeof(out[c].name), "%s", QPACK_STATIC[idx][0]);
                    snprintf(out[c].value, sizeof(out[c].value), "%s", QPACK_STATIC[idx][1]);
                    c++;
                }
            } else {                            /* T=0 动态 */
                if (!dt) return -1;
                char nm[64], vv[256];
                if (nl_h3_qpack_dt_get(dt, idx, nm, sizeof(nm), vv, sizeof(vv)) != 0) return -1;
                if (c < max) {
                    snprintf(out[c].name, sizeof(out[c].name), "%s", nm);
                    snprintf(out[c].value, sizeof(out[c].value), "%s", vv);
                    c++;
                }
            }
            continue;
        }
        if ((b & 0xC0) == 0x40) {               /* Literal with Name Reference */
            int T = (b & 0x10) ? 1 : 0;
            uint64_t idx = 0, vl = 0;
            size_t n = qp_read_int(d + p, len - p, 4, &idx);
            if (!n) return -1; p += n;
            char nm[64];
            if (T) { if (idx > 98) return -1; snprintf(nm, sizeof(nm), "%s", QPACK_STATIC[idx][0]); }
            else   { if (!dt) return -1; if (nl_h3_qpack_dt_get(dt, idx, nm, sizeof(nm), NULL, 0) != 0) return -1; }
            n = qp_read_int(d + p, len - p, 7, &vl);
            if (!n || p + vl > len || vl >= 256) return -1; p += n;
            if (c < max) {
                snprintf(out[c].name, sizeof(out[c].name), "%s", nm);
                memcpy(out[c].value, d + p, (size_t)vl); out[c].value[vl] = '\0';
                c++;
            }
            p += (size_t)vl;
            continue;
        }
        if ((b & 0xE0) == 0x20) {               /* Literal with Literal Name */
            uint64_t nl = 0, vl = 0;
            size_t n = qp_read_int(d + p, len - p, 3, &nl);
            if (!n || p + nl > len || nl >= 64) return -1; p += n;
            char nm[64];
            memcpy(nm, d + p, (size_t)nl); nm[nl] = '\0'; p += (size_t)nl;
            n = qp_read_int(d + p, len - p, 7, &vl);
            if (!n || p + vl > len || vl >= 256) return -1; p += n;
            if (c < max) {
                snprintf(out[c].name, sizeof(out[c].name), "%s", nm);
                memcpy(out[c].value, d + p, (size_t)vl); out[c].value[vl] = '\0';
                c++;
            }
            p += (size_t)vl;
            continue;
        }
        return -1;   /* 未实现形式 */
    }
    *count = c;
    return 0;
}

/* ============================================================
 * 自测
 * ============================================================ */
int nl_quictls_qpack_selftest(void) {
    uint8_t b[256];
    size_t p = 0;
    int n;

    /* :method GET → 静态索引 17 → Indexed 0xD1 */
    n = nl_h3_qpack_encode_field(b + p, sizeof(b) - p, ":method", "GET");
    if (n != 1 || b[p] != 0xD1) return 1;
    p += (size_t)n;

    /* :status 200 → 静态索引 25 → Indexed 0xD9 */
    n = nl_h3_qpack_encode_field(b + p, sizeof(b) - p, ":status", "200");
    if (n != 1 || b[p] != 0xD9) return 2;
    p += (size_t)n;

    /* :path /index.html → 名称引用（静态索引 1）→ 0x51 + 长度 + 值 */
    n = nl_h3_qpack_encode_field(b + p, sizeof(b) - p, ":path", "/index.html");
    if (n < 2 || b[p] != 0x51) return 3;
    p += (size_t)n;

    /* x-custom-header（不在静态表）→ 字面量名称 → 0x27(名长15续) 0x08 "x-custom-header" ... */
    n = nl_h3_qpack_encode_field(b + p, sizeof(b) - p, "x-custom-header", "NetLeaf");
    if (n < 2 || b[p] != 0x27) return 4;
    p += (size_t)n;

    /* 解码往返 */
    nl_h3_qpack_field_t f[16];
    int c = 0;
    if (nl_h3_qpack_decode(b, p, f, 16, &c) != 0) return 5;
    if (c != 4) return 6;
    if (strcmp(f[0].name, ":method") != 0 || strcmp(f[0].value, "GET") != 0) return 7;
    if (strcmp(f[1].name, ":status") != 0 || strcmp(f[1].value, "200") != 0) return 8;
    if (strcmp(f[2].name, ":path") != 0 || strcmp(f[2].value, "/index.html") != 0) return 9;
    if (strcmp(f[3].name, "x-custom-header") != 0 || strcmp(f[3].value, "NetLeaf") != 0) return 10;

    /* 便捷查找 */
    const char* m = nl_h3_qpack_get(f, c, ":method");
    if (!m || strcmp(m, "GET") != 0) return 11;

    /* 解析"外部风格"的静态索引块：0xC1 → :path / */
    { uint8_t ext[1] = { 0xC1 }; nl_h3_qpack_field_t g[4]; int gc = 0;
      if (nl_h3_qpack_decode(ext, 1, g, 4, &gc) != 0 || gc != 1) return 12;
      if (strcmp(g[0].name, ":path") != 0 || strcmp(g[0].value, "/") != 0) return 13; }

    /* 动态表：插入 / 逐出 / 索引 / 容量缩减 */
    {
        nl_h3_qpack_dt_t* dt = nl_h3_qpack_dt_new(200);
        if (!dt) return 14;
        int drc = 0;
        char n[64], v[256];
        if (nl_h3_qpack_dt_insert(dt, "a", "b") != 0) drc = 15;
        else if (nl_h3_qpack_dt_insert(dt, "c", "d") != 0) drc = 16;
        else if (nl_h3_qpack_dt_size(dt) != 68) drc = 17;              /* (1+1+32)*2 */
        else if (nl_h3_qpack_dt_insert_count(dt) != 2) drc = 18;
        else if (nl_h3_qpack_dt_get(dt, 0, n, sizeof(n), v, sizeof(v)) != 0) drc = 19;
        else if (strcmp(n, "c") || strcmp(v, "d")) drc = 20;          /* 最新 */
        else {
            char big[128]; memset(big, 'x', 100); big[100] = 0;
            if (nl_h3_qpack_dt_insert(dt, "e", big) != 0) drc = 21;   /* 逐出 a(34) */
            else if (nl_h3_qpack_dt_size(dt) != 167) drc = 22;        /* 34(c) + 133(e) */
            else if (nl_h3_qpack_dt_insert_count(dt) != 3) drc = 23;
            else if (nl_h3_qpack_dt_get(dt, 1, n, sizeof(n), v, sizeof(v)) != 0) drc = 24;
            else if (strcmp(n, "c") || strcmp(v, "d")) drc = 25;      /* 次新 = c */
            else if (nl_h3_qpack_dt_set_capacity(dt, 150) != 0) drc = 26;
            else if (nl_h3_qpack_dt_size(dt) != 133) drc = 27;        /* 逐出 c(34) → 剩 e(133) */
        }
        nl_h3_qpack_dt_free(dt);
        if (drc) return drc;
    }

    /* 指令首字节 */
    {
        uint8_t o[16]; int m;
        m = nl_h3_qpack_enc_set_capacity(o, sizeof(o), 200);
        if (m < 1 || o[0] != 0x3F) return 28;                          /* 0x20|31 */
        m = nl_h3_qpack_enc_insert_nameref(o, sizeof(o), 1, 17, "GET");
        if (m < 2 || o[0] != 0xD1) return 29;                          /* 0xC0|17 */
        m = nl_h3_qpack_enc_insert_literal(o, sizeof(o), "x", "y");
        if (m < 2 || o[0] != 0x41) return 30;                          /* 0x40|1 */
        if (nl_h3_qpack_dec_section_ack(o, sizeof(o), 0) < 1 || o[0] != 0x80) return 31;
        if (nl_h3_qpack_dec_stream_cancel(o, sizeof(o), 0) < 1 || o[0] != 0x40) return 32;
        if (nl_h3_qpack_dec_insert_count_increment(o, sizeof(o), 0) < 1 || o[0] != 0x00) return 33;
    }

    /* 动态表字段行接线（T=0：索引引用 / 名称引用） */
    {
        nl_h3_qpack_dt_t* de = nl_h3_qpack_dt_new(200);
        nl_h3_qpack_dt_t* dd = nl_h3_qpack_dt_new(200);
        if (!de || !dd) { nl_h3_qpack_dt_free(de); nl_h3_qpack_dt_free(dd); return 34; }
        int drc = 0;
        uint8_t fb[64];
        nl_h3_qpack_field_t ff[4];
        int fc = 0;
        if (nl_h3_qpack_dt_insert(de, "x-tenant", "acme") != 0) drc = 35;
        else if (nl_h3_qpack_dt_insert(dd, "x-tenant", "acme") != 0) drc = 36;
        else {
            int m = nl_h3_qpack_encode_field_dt(fb, sizeof(fb), de, "x-tenant", "acme");
            if (m < 1 || fb[0] != 0x80) drc = 37;                 /* Indexed Field Line，T=0，rel 0 */
            else if (nl_h3_qpack_decode_dt(fb, (size_t)m, dd, ff, 4, &fc) != 0 || fc != 1) drc = 38;
            else if (strcmp(ff[0].name, "x-tenant") || strcmp(ff[0].value, "acme")) drc = 39;
            else {
                m = nl_h3_qpack_encode_field_dt(fb, sizeof(fb), de, "x-tenant", "other");
                if (m < 2 || fb[0] != 0x40) drc = 40;             /* Literal w/ Name Ref，T=0，rel 0 */
                else if (nl_h3_qpack_decode_dt(fb, (size_t)m, dd, ff, 4, &fc) != 0 || fc != 1) drc = 41;
                else if (strcmp(ff[0].name, "x-tenant") || strcmp(ff[0].value, "other")) drc = 42;
            }
        }
        nl_h3_qpack_dt_free(de);
        nl_h3_qpack_dt_free(dd);
        if (drc) return drc;
    }

    return 0;
}
