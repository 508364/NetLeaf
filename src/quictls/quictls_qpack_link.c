/**
 * @file quictls_qpack_link.c
 * @brief QPACK 动态表 ↔ 编解码器流 字段块联动（RFC 9204 §3 / §4.3 / §4.5）
 * @version 0.1.0
 *
 * 覆盖：
 *   - 编码端联动（nl_h3_qpack_link_encode）：
 *       遍历动态表 rel 0..insert_count-1 查找精确 (name,value) 匹配；
 *       命中 → 输出 Indexed Field Line（T=0，6 位前缀相对索引，0x80|rel），
 *              不再产生编码器流指令（*enc_len = 0）；
 *       未命中 → 用 Insert With Literal Name 生成编码器流指令写入 enc_out，
 *              同步把该键值插入本端动态表，再输出 rel=0 的 Indexed Field Line。
 *   - 解码端联动（nl_h3_qpack_link_decode）：
 *       先应用对端编码器流指令（enc）到动态表，再解码字段块（field）。
 *
 * 依赖：netleaf_quictls.h（公共接口）；链表访问全部经由公共 API。
 */

#include "netleaf_quictls.h"

#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * 6 位前缀整数写入（RFC 9204 §4.1.1 风格）
 * high 为高位标志（Indexed Field Line 用 0x80）。
 * 返回写入字节数，0 表示容量不足/失败。
 * ============================================================ */
static size_t link_write_pref6(uint8_t* out, size_t cap, uint64_t v, uint8_t high) {
    if (!out || cap < 1) return 0;

    const uint64_t max = 0x3Fu; /* (1 << 6) - 1 */
    if (v < max) {
        out[0] = (uint8_t)(high | (uint8_t)v);
        return 1;
    }

    out[0] = (uint8_t)(high | (uint8_t)max);
    size_t n = 1;
    v -= max;
    while (v >= 128) {
        if (n >= cap) return 0;
        out[n++] = (uint8_t)((v & 0x7f) | 0x80);
        v >>= 7;
    }
    if (n >= cap) return 0;
    out[n++] = (uint8_t)v;
    return n;
}

/* ============================================================
 * 联动编码
 * ============================================================ */
int nl_h3_qpack_link_encode(nl_h3_qpack_dt_t* dt,
                            const char* name, const char* value,
                            uint8_t* field_out, size_t field_cap, size_t* field_len,
                            uint8_t* enc_out, size_t enc_cap, size_t* enc_len) {
    if (!dt || !name || !value) return -1;
    if (!field_out || !field_len || !enc_out || !enc_len) return -1;

    *field_len = 0;
    *enc_len = 0;

    /* 1) 精确匹配：rel 0..insert_count-1 */
    uint64_t ic = nl_h3_qpack_dt_insert_count(dt);
    for (uint64_t rel = 0; rel < ic; rel++) {
        char bn[64];
        char bv[256];
        if (nl_h3_qpack_dt_get(dt, rel, bn, sizeof(bn), bv, sizeof(bv)) != 0) continue;
        if (strcmp(bn, name) == 0 && strcmp(bv, value) == 0) {
            size_t n = link_write_pref6(field_out, field_cap, rel, 0x80);
            if (!n) return -1;
            *field_len = n;
            *enc_len = 0;
            return 0;
        }
    }

    /* 2) 未命中：生成编码器流指令并同步插入本端动态表 */
    int m = nl_h3_qpack_enc_insert_literal(enc_out, enc_cap, name, value);
    if (m < 0) return -1;
    *enc_len = (size_t)m;

    if (nl_h3_qpack_dt_insert(dt, name, value) != 0) return -1;

    size_t n = link_write_pref6(field_out, field_cap, 0, 0x80);
    if (!n) return -1;
    *field_len = n;
    return 0;
}

/* ============================================================
 * 联动解码
 * ============================================================ */
int nl_h3_qpack_link_decode(nl_h3_qpack_streams_t* s, const nl_h3_qpack_dt_t* dt,
                            const uint8_t* enc, size_t enc_len,
                            const uint8_t* field, size_t field_len,
                            nl_h3_qpack_field_t* out, int max, int* count) {
    if (!s || !dt || !field || !out || !count) return -1;

    if (enc_len > 0) {
        if (!enc) return -1;
        if (nl_h3_qpack_streams_on_encoder(s, enc, enc_len) != 0) return -1;
    }

    return nl_h3_qpack_decode_dt(field, field_len, dt, out, max, count);
}

/* ============================================================
 * 自测：失败返回步号（>0），全部通过返回 0
 * ============================================================ */
int nl_quictls_qpack_link_selftest(void) {
    nl_h3_qpack_dt_t* dt_s = nl_h3_qpack_dt_new(200);
    if (!dt_s) return 1;

    nl_h3_qpack_dt_t* dt_r = nl_h3_qpack_dt_new(200);
    if (!dt_r) { nl_h3_qpack_dt_free(dt_s); return 2; }

    nl_h3_qpack_streams_t* s_r = nl_h3_qpack_streams_new(dt_r);
    if (!s_r) {
        nl_h3_qpack_dt_free(dt_r);
        nl_h3_qpack_dt_free(dt_s);
        return 3;
    }

    uint8_t field[64];
    uint8_t enc[256];
    size_t field_len = 0;
    size_t enc_len = 0;
    nl_h3_qpack_field_t out[4];
    int n = 0;
    int rc = 0;

    /* 首次编码：未命中 → 产生编码器流指令，字段行为 rel=0 的 T=0 索引 */
    if (nl_h3_qpack_link_encode(dt_s, "x-tenant", "acme",
                                field, sizeof(field), &field_len,
                                enc, sizeof(enc), &enc_len) != 0) rc = 4;
    else if (enc_len == 0) rc = 5;
    else if (field_len < 1 || field[0] != 0x80) rc = 6;
    else if (nl_h3_qpack_link_decode(s_r, dt_r, enc, enc_len,
                                     field, field_len, out, 4, &n) != 0) rc = 7;
    else if (n != 1) rc = 8;
    else if (strcmp(out[0].name, "x-tenant") != 0) rc = 9;
    else if (strcmp(out[0].value, "acme") != 0) rc = 10;

    /* 二次编码：命中 → 不再产生编码器流指令 */
    if (rc == 0) {
        if (nl_h3_qpack_link_encode(dt_s, "x-tenant", "acme",
                                    field, sizeof(field), &field_len,
                                    enc, sizeof(enc), &enc_len) != 0) rc = 11;
        else if (enc_len != 0) rc = 12;
        else if (field_len < 1 || field[0] != 0x80) rc = 13;
        else if (nl_h3_qpack_link_decode(s_r, dt_r, enc, enc_len,
                                         field, field_len, out, 4, &n) != 0) rc = 14;
        else if (n != 1) rc = 15;
        else if (strcmp(out[0].name, "x-tenant") != 0) rc = 16;
        else if (strcmp(out[0].value, "acme") != 0) rc = 17;
    }

    nl_h3_qpack_streams_free(s_r);
    nl_h3_qpack_dt_free(dt_r);
    nl_h3_qpack_dt_free(dt_s);
    return rc;
}
