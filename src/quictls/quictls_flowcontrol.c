/**
 * @file quictls_flowcontrol.c
 * @brief QUIC 流控：连接级 + 流级窗口（RFC 9000 §4 / §19.9-19.11）
 * @version 0.1.0
 *
 * 覆盖：
 *   - 发送侧：对端 MAX_DATA / MAX_STREAM_DATA 约束下的可发送字节数、阻塞判定、已发送计数
 *   - 接收侧：已收计数、按窗口自动抬升并生成 MAX_DATA / MAX_STREAM_DATA 更新
 *   - 帧编解码：MAX_DATA(0x10) / MAX_STREAM_DATA(0x11)
 *
 * 说明：同一对象同时维护"发送侧（对端上限）"与"接收侧（本地窗口）"两组状态，便于自测。
 */

#include "netleaf_quictls.h"

#include <string.h>
#include <stdlib.h>

#define FC_MAX_STREAMS 64

typedef struct { uint64_t sid, recv, lmax; } fc_lstream_t;
typedef struct { uint64_t sid, max, sent; }  fc_pstream_t;

struct nl_quic_fc {
    /* 本地接收窗口（用于抬升通告） */
    uint64_t conn_window, stream_window;
    uint64_t l_conn_recv, l_conn_max;
    fc_lstream_t ls[FC_MAX_STREAMS]; size_t ls_count;

    /* 对端上限（发送侧） */
    uint64_t p_conn_max, conn_sent;
    fc_pstream_t ps[FC_MAX_STREAMS]; size_t ps_count;
};

nl_quic_fc_t* nl_quic_fc_new(uint64_t conn_window, uint64_t stream_window) {
    nl_quic_fc_t* fc = (nl_quic_fc_t*)calloc(1, sizeof(nl_quic_fc_t));
    if (!fc) return NULL;
    fc->conn_window = conn_window;
    fc->stream_window = stream_window;
    fc->l_conn_max = conn_window;   /* 初始通告 = 窗口 */
    return fc;
}

void nl_quic_fc_free(nl_quic_fc_t* fc) { free(fc); }

/* ---------- 发送侧 ---------- */
static fc_pstream_t* ps_get(nl_quic_fc_t* fc, uint64_t sid, int create) {
    for (size_t i = 0; i < fc->ps_count; i++) if (fc->ps[i].sid == sid) return &fc->ps[i];
    if (!create || fc->ps_count >= FC_MAX_STREAMS) return NULL;
    fc_pstream_t* p = &fc->ps[fc->ps_count++];
    p->sid = sid; p->max = 0; p->sent = 0;
    return p;
}

uint64_t nl_quic_fc_send_allow(nl_quic_fc_t* fc, uint64_t stream_id, uint64_t want) {
    if (!fc) return 0;
    fc_pstream_t* ps = ps_get(fc, stream_id, 1);
    uint64_t conn_rem = (fc->p_conn_max > fc->conn_sent) ? (fc->p_conn_max - fc->conn_sent) : 0;
    uint64_t str_rem  = (ps && ps->max > ps->sent) ? (ps->max - ps->sent) : 0;
    uint64_t a = conn_rem < str_rem ? conn_rem : str_rem;
    return a < want ? a : want;
}

void nl_quic_fc_on_sent(nl_quic_fc_t* fc, uint64_t stream_id, uint64_t bytes) {
    if (!fc) return;
    fc_pstream_t* ps = ps_get(fc, stream_id, 1);
    if (ps) ps->sent += bytes;
    fc->conn_sent += bytes;
}

void nl_quic_fc_on_max_data(nl_quic_fc_t* fc, uint64_t max) {
    if (fc) fc->p_conn_max = max;
}

void nl_quic_fc_on_max_stream_data(nl_quic_fc_t* fc, uint64_t stream_id, uint64_t max) {
    if (!fc) return;
    fc_pstream_t* ps = ps_get(fc, stream_id, 1);
    if (ps) ps->max = max;
}

int nl_quic_fc_send_blocked(const nl_quic_fc_t* fc, uint64_t stream_id) {
    if (!fc) return 1;
    uint64_t conn_rem = (fc->p_conn_max > fc->conn_sent) ? (fc->p_conn_max - fc->conn_sent) : 0;
    if (conn_rem == 0) return 1;
    for (size_t i = 0; i < fc->ps_count; i++) {
        if (fc->ps[i].sid == stream_id) {
            uint64_t str_rem = (fc->ps[i].max > fc->ps[i].sent) ? (fc->ps[i].max - fc->ps[i].sent) : 0;
            return str_rem == 0;
        }
    }
    return 1;   /* 未获知流上限 → 视为阻塞 */
}

/* ---------- 接收侧 ---------- */
static fc_lstream_t* ls_get(nl_quic_fc_t* fc, uint64_t sid, int create) {
    for (size_t i = 0; i < fc->ls_count; i++) if (fc->ls[i].sid == sid) return &fc->ls[i];
    if (!create || fc->ls_count >= FC_MAX_STREAMS) return NULL;
    fc_lstream_t* s = &fc->ls[fc->ls_count++];
    s->sid = sid; s->recv = 0; s->lmax = fc->stream_window;   /* 初始流窗口 */
    return s;
}

int nl_quic_fc_on_recv(nl_quic_fc_t* fc, uint64_t stream_id, uint64_t bytes) {
    if (!fc) return 0;
    int upd = 0;
    fc->l_conn_recv += bytes;
    if (fc->l_conn_recv + fc->conn_window / 2 > fc->l_conn_max) {
        fc->l_conn_max = fc->l_conn_recv + fc->conn_window;
        upd = 1;
    }
    fc_lstream_t* ls = ls_get(fc, stream_id, 1);
    if (ls) {
        ls->recv += bytes;
        if (ls->recv + fc->stream_window / 2 > ls->lmax) {
            ls->lmax = ls->recv + fc->stream_window;
            upd = 1;
        }
    }
    return upd;
}

uint64_t nl_quic_fc_advertised_max_data(const nl_quic_fc_t* fc) {
    return fc ? fc->l_conn_max : 0;
}

uint64_t nl_quic_fc_advertised_max_stream_data(const nl_quic_fc_t* fc, uint64_t stream_id) {
    if (!fc) return 0;
    for (size_t i = 0; i < fc->ls_count; i++)
        if (fc->ls[i].sid == stream_id) return fc->ls[i].lmax;
    return fc->stream_window;
}

/* ---------- 帧编解码 ---------- */
size_t nl_quic_max_data_encode(uint8_t* out, size_t cap, uint64_t max, size_t* out_len) {
    size_t p = 0, n;
    n = nl_quic_varint_write(out + p, cap - p, 0x10); if (!n) return 0; p += n;
    n = nl_quic_varint_write(out + p, cap - p, max);  if (!n) return 0; p += n;
    if (out_len) *out_len = p;
    return p;
}

int nl_quic_max_data_decode(const uint8_t* d, size_t len, uint64_t* max, size_t* consumed) {
    size_t p = 0, n; uint64_t t = 0;
    n = nl_quic_varint_read(d + p, len - p, &t); if (!n) return -1; p += n;
    if (t != 0x10) return -1;
    n = nl_quic_varint_read(d + p, len - p, max); if (!n) return -1; p += n;
    if (consumed) *consumed = p;
    return 0;
}

size_t nl_quic_max_stream_data_encode(uint8_t* out, size_t cap, uint64_t sid, uint64_t max, size_t* out_len) {
    size_t p = 0, n;
    n = nl_quic_varint_write(out + p, cap - p, 0x11); if (!n) return 0; p += n;
    n = nl_quic_varint_write(out + p, cap - p, sid);  if (!n) return 0; p += n;
    n = nl_quic_varint_write(out + p, cap - p, max);  if (!n) return 0; p += n;
    if (out_len) *out_len = p;
    return p;
}

int nl_quic_max_stream_data_decode(const uint8_t* d, size_t len, uint64_t* sid, uint64_t* max, size_t* consumed) {
    size_t p = 0, n; uint64_t t = 0;
    n = nl_quic_varint_read(d + p, len - p, &t);   if (!n) return -1; p += n;
    if (t != 0x11) return -1;
    n = nl_quic_varint_read(d + p, len - p, sid);  if (!n) return -1; p += n;
    n = nl_quic_varint_read(d + p, len - p, max);  if (!n) return -1; p += n;
    if (consumed) *consumed = p;
    return 0;
}

/* ============================================================
 * 自测
 * ============================================================ */
int nl_quictls_flowcontrol_selftest(void) {
    nl_quic_fc_t* fc = nl_quic_fc_new(1000 /*conn window*/, 400 /*stream window*/);
    if (!fc) return 1;
    int rc = 0;

    /* 模拟对端 transport params：conn 1000，流 0/4 各 400 */
    nl_quic_fc_on_max_data(fc, 1000);
    nl_quic_fc_on_max_stream_data(fc, 0, 400);
    nl_quic_fc_on_max_stream_data(fc, 4, 800);

    /* 流 0：受流窗口限制 → 可发 400 */
    if (nl_quic_fc_send_allow(fc, 0, 1000) != 400) { rc = 2; goto done; }
    nl_quic_fc_on_sent(fc, 0, 400);
    if (nl_quic_fc_send_allow(fc, 0, 100) != 0) { rc = 3; goto done; }
    if (!nl_quic_fc_send_blocked(fc, 0)) { rc = 4; goto done; }

    /* 流 4：对端流窗口 800、剩余连接窗口 600 → 可发 600 */
    if (nl_quic_fc_send_allow(fc, 4, 1000) != 600) { rc = 5; goto done; }
    nl_quic_fc_on_sent(fc, 4, 600);

    /* 连接窗口耗尽 → 流 4 阻塞 */
    if (nl_quic_fc_send_allow(fc, 4, 1000) != 0) { rc = 6; goto done; }
    if (!nl_quic_fc_send_blocked(fc, 4)) { rc = 7; goto done; }

    /* 接收侧：收到数据并按窗口抬升通告 */
    if (!nl_quic_fc_on_recv(fc, 0, 400)) { rc = 8; goto done; }    /* 应触发更新 */
    uint64_t adv_s0 = nl_quic_fc_advertised_max_stream_data(fc, 0);
    if (adv_s0 != 800) { rc = 9; goto done; }                      /* 400(收) + 400(窗口) */

    if (!nl_quic_fc_on_recv(fc, 4, 600)) { rc = 10; goto done; }   /* 应触发连接更新 */
    uint64_t adv_c = nl_quic_fc_advertised_max_data(fc);
    if (adv_c != 2000) { rc = 11; goto done; }                     /* 1000(收) + 1000(窗口) */

    /* 帧编解码往返 */
    uint8_t b[16]; size_t bl = 0;
    if (nl_quic_max_data_encode(b, sizeof(b), adv_c, &bl) == 0) { rc = 12; goto done; }
    uint64_t m = 0, cons = 0;
    if (nl_quic_max_data_decode(b, bl, &m, &cons) != 0 || m != adv_c || cons != bl) { rc = 13; goto done; }
    if (nl_quic_max_stream_data_encode(b, sizeof(b), 0, adv_s0, &bl) == 0) { rc = 14; goto done; }
    uint64_t sid = 0, m2 = 0, cons2 = 0;
    if (nl_quic_max_stream_data_decode(b, bl, &sid, &m2, &cons2) != 0 || sid != 0 || m2 != adv_s0 || cons2 != bl) { rc = 15; goto done; }

    /* 发送侧收到更新后解除阻塞：流 4 已发 600、窗口 800 → 剩 200；连接剩 1000 → min=200 */
    nl_quic_fc_on_max_stream_data(fc, 4, adv_s0);
    nl_quic_fc_on_max_data(fc, adv_c);
    if (nl_quic_fc_send_allow(fc, 4, 1000) != 200) { rc = 16; goto done; }
    if (nl_quic_fc_send_blocked(fc, 4)) { rc = 17; goto done; }

done:
    nl_quic_fc_free(fc);
    return rc;
}
