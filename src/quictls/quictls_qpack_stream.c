/**
 * @file quictls_qpack_stream.c
 * @brief QPACK 编解码器流与 Base 相对索引解析（RFC 9204 §4.2-4.4）
 * @version 0.1.0
 *
 * 覆盖：
 *   - 编码器流指令（解码器侧处理，§4.3）：
 *       Set Dynamic Table Capacity（001 + 5 位前缀）
 *       Insert With Name Reference（1 T + 6 位前缀 index，T=1 静态 / T=0 动态）
 *       Insert With Literal Name（01 H + 5 位前缀 名称长度）
 *       Duplicate（000 + 5 位前缀 index）
 *   - 解码器流指令（编码器侧处理，§4.4）：
 *       Section Acknowledgment（1 + 7 位前缀 stream_id）
 *       Stream Cancellation（01 + 6 位前缀 stream_id）
 *       Insert Count Increment（00 + 6 位前缀 increment）
 *   - Base 相对索引解析（§3.2.5/§3.2.6）
 *
 * 简化约定：
 *   - 前缀整数编码为 RFC 9204 §4.1.1 风格（首字节前缀位 + 续行 7 位）。
 *   - 不支持霍夫曼编码（H=1）；遇到 H=1 的名称/值直接返回 -1。
 *   - 动态表通过不透明的 nl_h3_qpack_dt_t + 公共 API 访问。
 *
 * 依赖：netleaf_quictls.h（公共接口）、quictls_qpack.c（动态表 / 静态表实现）。
 */

#include "netleaf_quictls.h"

#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 编解码器流状态
 * ============================================================ */
struct nl_h3_qpack_streams {
    nl_h3_qpack_dt_t* dt;             /* 关联动态表（不透明） */
    uint64_t          capacity;       /* 解码器侧：编码器设定的动态表容量 */
    uint64_t          known_received; /* 解码器侧：编码器已确认接收的插入指令数 */
    uint64_t          total_inserts;  /* 解码器侧：已处理的插入类指令总数 */
};

/* 前缀整数读取（RFC 9204 §4.1.1 风格）：返回消耗字节数，0 表示失败/字节不足 */
static size_t qpack_read_prefix_int(const uint8_t* p, size_t len, int prefix, uint64_t* v) {
    if (!p || len < 1 || prefix < 1 || prefix > 8) return 0;
    uint64_t max = (prefix >= 8) ? 0xFFu : ((1ULL << prefix) - 1);
    uint64_t r = (uint64_t)(p[0] & (uint8_t)max);
    if (r < max) { if (v) *v = r; return 1; }
    size_t n = 1;
    uint64_t shift = 0;
    while (n < len) {
        uint8_t b = p[n++];
        r += (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { if (v) *v = r; return n; }
        shift += 7;
        if (shift > 56) return 0;
    }
    return 0;
}

/* ============================================================
 * 生命周期
 * ============================================================ */
nl_h3_qpack_streams_t* nl_h3_qpack_streams_new(nl_h3_qpack_dt_t* dt) {
    nl_h3_qpack_streams_t* s = (nl_h3_qpack_streams_t*)calloc(1, sizeof(nl_h3_qpack_streams_t));
    if (!s) return NULL;
    s->dt = dt;
    return s;
}

void nl_h3_qpack_streams_free(nl_h3_qpack_streams_t* s) {
    free(s);
}

/* ============================================================
 * 解码器侧：处理编码器流指令（RFC 9204 §4.3）
 * ============================================================ */
int nl_h3_qpack_streams_on_encoder(nl_h3_qpack_streams_t* s, const uint8_t* d, size_t len) {
    if (!s || !d) return -1;
    size_t p = 0;

    while (p < len) {
        uint8_t b = d[p];

        if (b & 0x80) {
            /* Insert With Name Reference：1 T + 6 位前缀 index */
            int T = (b & 0x40) ? 1 : 0;
            uint64_t idx = 0;
            size_t n = qpack_read_prefix_int(d + p, len - p, 6, &idx);
            if (!n) return -1;
            p += n;

            char name[64];
            if (T) {
                if (nl_h3_qpack_static_get(idx, name, sizeof(name), NULL, 0) != 0) return -1;
            } else {
                if (!s->dt) return -1;
                if (nl_h3_qpack_dt_get(s->dt, idx, name, sizeof(name), NULL, 0) != 0) return -1;
            }

            /* 值：H + 7 位前缀长度 + 值字节 */
            if (p >= len) return -1;
            if (d[p] & 0x80) return -1;              /* H=1（霍夫曼）不支持 */
            uint64_t vl = 0;
            n = qpack_read_prefix_int(d + p, len - p, 7, &vl);
            if (!n || p + n + vl > len) return -1;
            p += n;

            char value[256];
            if (vl >= sizeof(value)) return -1;
            memcpy(value, d + p, (size_t)vl);
            value[vl] = '\0';
            p += (size_t)vl;

            if (!s->dt) return -1;
            if (nl_h3_qpack_dt_insert(s->dt, name, value) != 0) return -1;
            s->total_inserts++;
            continue;
        }

        if ((b & 0xC0) == 0x40) {
            /* Insert With Literal Name：01 H + 5 位前缀 名称长度 */
            if (b & 0x20) return -1;                 /* H=1（霍夫曼）不支持 */
            uint64_t nl = 0;
            size_t n = qpack_read_prefix_int(d + p, len - p, 5, &nl);
            if (!n || p + n + nl > len) return -1;
            p += n;

            char name[64];
            if (nl >= sizeof(name)) return -1;
            memcpy(name, d + p, (size_t)nl);
            name[nl] = '\0';
            p += (size_t)nl;

            if (p >= len) return -1;
            if (d[p] & 0x80) return -1;              /* H=1（霍夫曼）不支持 */
            uint64_t vl = 0;
            n = qpack_read_prefix_int(d + p, len - p, 7, &vl);
            if (!n || p + n + vl > len) return -1;
            p += n;

            char value[256];
            if (vl >= sizeof(value)) return -1;
            memcpy(value, d + p, (size_t)vl);
            value[vl] = '\0';
            p += (size_t)vl;

            if (!s->dt) return -1;
            if (nl_h3_qpack_dt_insert(s->dt, name, value) != 0) return -1;
            s->total_inserts++;
            continue;
        }

        if ((b & 0xE0) == 0x20) {
            /* Set Dynamic Table Capacity：001 + 5 位前缀 capacity */
            uint64_t cap = 0;
            size_t n = qpack_read_prefix_int(d + p, len - p, 5, &cap);
            if (!n) return -1;
            p += n;
            if (!s->dt) return -1;
            if (nl_h3_qpack_dt_set_capacity(s->dt, cap) != 0) return -1;
            s->capacity = cap;
            continue;
        }

        if ((b & 0xE0) == 0x00) {
            /* Duplicate：000 + 5 位前缀 index（动态表相对索引） */
            uint64_t idx = 0;
            size_t n = qpack_read_prefix_int(d + p, len - p, 5, &idx);
            if (!n) return -1;
            p += n;
            if (!s->dt) return -1;

            char name[64], value[256];
            if (nl_h3_qpack_dt_get(s->dt, idx, name, sizeof(name), value, sizeof(value)) != 0) return -1;
            if (nl_h3_qpack_dt_insert(s->dt, name, value) != 0) return -1;
            s->total_inserts++;
            continue;
        }

        return -1;
    }

    return 0;
}

/* ============================================================
 * 编码器侧：处理解码器流指令（RFC 9204 §4.4）
 * ============================================================ */
int nl_h3_qpack_streams_on_decoder(nl_h3_qpack_streams_t* s, const uint8_t* d, size_t len) {
    if (!s || !d) return -1;
    size_t p = 0;

    while (p < len) {
        uint8_t b = d[p];

        if (b & 0x80) {
            /* Section Acknowledgment：1 + 7 位前缀 stream_id */
            uint64_t sid = 0;
            size_t n = qpack_read_prefix_int(d + p, len - p, 7, &sid);
            if (!n) return -1;
            p += n;
            (void)sid;                               /* 简化为忽略细节 */
            continue;
        }

        if ((b & 0xC0) == 0x40) {
            /* Stream Cancellation：01 + 6 位前缀 stream_id */
            uint64_t sid = 0;
            size_t n = qpack_read_prefix_int(d + p, len - p, 6, &sid);
            if (!n) return -1;
            p += n;
            (void)sid;
            continue;
        }

        /* Insert Count Increment：00 + 6 位前缀 increment */
        uint64_t inc = 0;
        size_t n = qpack_read_prefix_int(d + p, len - p, 6, &inc);
        if (!n) return -1;
        p += n;
        s->known_received += inc;
    }

    return 0;
}

/* ============================================================
 * Base 相对索引解析（RFC 9204 §3.2.5/§3.2.6）
 * ============================================================ */
int nl_h3_qpack_resolve_dynamic(const nl_h3_qpack_dt_t* dt, uint64_t base, int post_base,
                                uint64_t rel_index, char* name, size_t ncap,
                                char* value, size_t vcap) {
    if (!dt) return -1;

    uint64_t abs;
    if (post_base) {
        abs = base + rel_index;
    } else {
        if (rel_index + 1 > base) return -1;         /* 避免下溢：base 必须 > rel_index */
        abs = base - rel_index - 1;
    }

    uint64_t ic = nl_h3_qpack_dt_insert_count(dt);
    if (abs >= ic) return -1;                        /* 绝对插入索引越界 */

    uint64_t r = ic - 1 - abs;                       /* 换算为动态表相对索引（0 为最新） */
    return nl_h3_qpack_dt_get(dt, r, name, ncap, value, vcap);
}

/* ============================================================
 * 查询
 * ============================================================ */
uint64_t nl_h3_qpack_streams_known_received(const nl_h3_qpack_streams_t* s) {
    return s ? s->known_received : 0;
}

/* ============================================================
 * 自测
 * ============================================================ */
int nl_quictls_qpack_stream_selftest(void) {
    /* 编码器流：Set Capacity 200 | Insert NameRef(T=1,idx=17 :method)="GET"
     *           | Insert Literal Name "x-a"="b" */
    static const uint8_t enc_stream[] = {
        0x3F, 0xA9, 0x01,                        /* Set Capacity 200 */
        0xD1, 0x03, 'G', 'E', 'T',               /* Insert Name Ref T=1 idx=17 */
        0x43, 'x', '-', 'a', 0x01, 'b'           /* Insert Literal Name "x-a"="b" */
    };
    /* 解码器流：Section Ack(5) | Stream Cancel(3) | Insert Count Increment(3) */
    static const uint8_t dec_stream[] = { 0x85, 0x43, 0x03 };
    static const uint8_t dup_stream[] = { 0x00 };   /* Duplicate 相对索引 0（最新） */

    nl_h3_qpack_dt_t* dt = nl_h3_qpack_dt_new(200);
    if (!dt) return 1;

    nl_h3_qpack_streams_t* s = nl_h3_qpack_streams_new(dt);
    if (!s) { nl_h3_qpack_dt_free(dt); return 2; }

    int rc = 0;
    char name[64], value[256];

    /* 1) 处理编码器流 */
    if (nl_h3_qpack_streams_on_encoder(s, enc_stream, sizeof(enc_stream)) != 0) rc = 3;
    else if (s->capacity != 200) rc = 4;
    else if (s->total_inserts != 2) rc = 5;
    else if (nl_h3_qpack_dt_insert_count(dt) != 2) rc = 6;

    /* 2) Base 解析：base=insert_count=2，非 post_base */
    else if (nl_h3_qpack_resolve_dynamic(dt, 2, 0, 0, name, sizeof(name), value, sizeof(value)) != 0) rc = 7;
    else if (strcmp(name, "x-a") != 0) rc = 8;       /* rel=0 → 最新插入 */
    else if (strcmp(value, "b") != 0) rc = 9;
    else if (nl_h3_qpack_resolve_dynamic(dt, 2, 0, 1, name, sizeof(name), value, sizeof(value)) != 0) rc = 10;
    else if (strcmp(name, ":method") != 0) rc = 11;  /* rel=1 → 次新 = T=1 插入项 */
    else if (strcmp(value, "GET") != 0) rc = 12;

    /* 3) 处理解码器流：Insert Count Increment(3) */
    else if (nl_h3_qpack_streams_on_decoder(s, dec_stream, sizeof(dec_stream)) != 0) rc = 13;
    else if (nl_h3_qpack_streams_known_received(s) != 3) rc = 14;

    /* 4) Duplicate 指令：复制最新条目 "x-a"/"b" */
    else if (nl_h3_qpack_streams_on_encoder(s, dup_stream, sizeof(dup_stream)) != 0) rc = 15;
    else if (s->total_inserts != 3) rc = 16;
    else if (nl_h3_qpack_dt_insert_count(dt) != 3) rc = 17;

    /* 5) Post-Base 解析：base=2，post_base=1，rel=0 → abs=2 → 相对索引 0（最新副本） */
    else if (nl_h3_qpack_resolve_dynamic(dt, 2, 1, 0, name, sizeof(name), value, sizeof(value)) != 0) rc = 18;
    else if (strcmp(name, "x-a") != 0) rc = 19;
    else if (strcmp(value, "b") != 0) rc = 20;

    /* 6) 越界索引应被拒绝（base=2，非 post_base，rel=100 → 下溢） */
    else if (nl_h3_qpack_resolve_dynamic(dt, 2, 0, 100, name, sizeof(name), value, sizeof(value)) == 0) rc = 21;

    nl_h3_qpack_streams_free(s);
    nl_h3_qpack_dt_free(dt);
    return rc;
}
