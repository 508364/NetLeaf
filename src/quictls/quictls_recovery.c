/**
 * @file quictls_recovery.c
 * @brief QUIC 可靠性：ACK 帧 + 丢包检测/重传 + 拥塞控制（RFC 9000/9002 简化）
 * @version 0.1.0
 *
 * 覆盖：
 *   - ACK 帧（RFC 9000 §19.3）编解码（含 ACK Range）
 *   - 接收端包号追踪 → 生成 ACK 范围
 *   - 发送端恢复：发送记录、packet-threshold 丢包检测（pn < largest_acked 且未确认即判丢）、
 *     重传队列、NewReno 式拥塞窗口（慢启动 / 拥塞避免 / 丢包减半）
 *
 * 说明：为可验证的可靠性逻辑骨架——用逻辑事件驱动（无真实时钟/PTO/RTT 采样）。
 * 参考范式：quicly / ngtcp2 的 sent-packet + ACK 处理。
 */

#include "netleaf_quictls.h"

#include <string.h>
#include <stdlib.h>

/* ============================================================
 * ACK 帧编解码（RFC 9000 §19.3）
 * ============================================================ */
size_t nl_quic_ack_encode(uint8_t* out, size_t cap, uint64_t ack_delay,
                          const nl_quic_ack_range_t* ranges, size_t n_ranges,
                          size_t* out_len) {
    if (!out || !ranges || n_ranges == 0) return 0;
    size_t p = 0, n;
    n = nl_quic_varint_write(out + p, cap - p, 0x02); if (!n) return 0; p += n;
    n = nl_quic_varint_write(out + p, cap - p, ranges[0].largest); if (!n) return 0; p += n;
    n = nl_quic_varint_write(out + p, cap - p, ack_delay); if (!n) return 0; p += n;
    n = nl_quic_varint_write(out + p, cap - p, n_ranges - 1); if (!n) return 0; p += n;
    n = nl_quic_varint_write(out + p, cap - p, ranges[0].largest - ranges[0].smallest); if (!n) return 0; p += n;
    for (size_t i = 1; i < n_ranges; i++) {
        uint64_t gap  = ranges[i - 1].smallest - ranges[i].largest - 2;
        uint64_t rlen = ranges[i].largest - ranges[i].smallest;
        n = nl_quic_varint_write(out + p, cap - p, gap);  if (!n) return 0; p += n;
        n = nl_quic_varint_write(out + p, cap - p, rlen); if (!n) return 0; p += n;
    }
    *out_len = p;
    return p;
}

int nl_quic_ack_decode(const uint8_t* d, size_t len, uint64_t* ack_delay,
                       nl_quic_ack_range_t* ranges, size_t max_ranges,
                       size_t* n_ranges, size_t* consumed) {
    size_t p = 0, n;
    uint64_t t = 0, largest = 0, delay = 0, rcount = 0, first = 0;
    n = nl_quic_varint_read(d + p, len - p, &t);       if (!n) return -1; p += n;
    if (t != 0x02 && t != 0x03) return -1;
    n = nl_quic_varint_read(d + p, len - p, &largest); if (!n) return -1; p += n;
    n = nl_quic_varint_read(d + p, len - p, &delay);   if (!n) return -1; p += n;
    n = nl_quic_varint_read(d + p, len - p, &rcount);  if (!n) return -1; p += n;
    n = nl_quic_varint_read(d + p, len - p, &first);   if (!n) return -1; p += n;

    size_t cnt = 0;
    uint64_t prev_smallest = (largest >= first) ? (largest - first) : 0;
    if (cnt < max_ranges) { ranges[cnt].largest = largest; ranges[cnt].smallest = prev_smallest; cnt++; }

    for (uint64_t i = 0; i < rcount; i++) {
        uint64_t gap = 0, rl = 0;
        n = nl_quic_varint_read(d + p, len - p, &gap); if (!n) return -1; p += n;
        n = nl_quic_varint_read(d + p, len - p, &rl);  if (!n) return -1; p += n;
        if (prev_smallest < gap + 2) return -1;
        uint64_t r_largest = prev_smallest - gap - 2;
        uint64_t r_smallest = (r_largest >= rl) ? (r_largest - rl) : 0;
        if (cnt < max_ranges) { ranges[cnt].largest = r_largest; ranges[cnt].smallest = r_smallest; cnt++; }
        prev_smallest = r_smallest;
    }
    if (ack_delay) *ack_delay = delay;
    if (n_ranges) *n_ranges = cnt;
    if (consumed) *consumed = p;
    return 0;
}

/* ============================================================
 * 接收端包号追踪 → ACK 范围
 * ============================================================ */
#define ACKTRACK_MAX 1024

struct nl_quic_acktrack {
    uint64_t pns[ACKTRACK_MAX];
    size_t   count;
};

nl_quic_acktrack_t* nl_quic_acktrack_new(void) {
    return (nl_quic_acktrack_t*)calloc(1, sizeof(nl_quic_acktrack_t));
}

void nl_quic_acktrack_free(nl_quic_acktrack_t* a) {
    free(a);
}

int nl_quic_acktrack_add(nl_quic_acktrack_t* a, uint64_t pn) {
    if (!a) return -1;
    for (size_t i = 0; i < a->count; i++) if (a->pns[i] == pn) return 0;   /* 幂等 */
    if (a->count >= ACKTRACK_MAX) return -1;
    a->pns[a->count++] = pn;
    return 0;
}

int nl_quic_acktrack_ranges(const nl_quic_acktrack_t* a,
                            nl_quic_ack_range_t* out, size_t max, size_t* n) {
    if (!a || !out || !n) return -1;
    if (a->count == 0) { *n = 0; return 0; }

    /* 拷贝并降序排序（插入排序，量小） */
    uint64_t s[ACKTRACK_MAX];
    memcpy(s, a->pns, a->count * sizeof(uint64_t));
    for (size_t i = 1; i < a->count; i++) {
        uint64_t v = s[i];
        size_t j = i;
        while (j > 0 && s[j - 1] < v) { s[j] = s[j - 1]; j--; }
        s[j] = v;
    }
    /* 合并连续区间 */
    size_t c = 0;
    size_t i = 0;
    while (i < a->count) {
        uint64_t hi = s[i], lo = s[i];
        size_t j = i + 1;
        while (j < a->count && s[j] + 1 == lo) { lo = s[j]; j++; }
        if (c < max) { out[c].largest = hi; out[c].smallest = lo; c++; }
        i = j;
    }
    *n = c;
    return 0;
}

/* ============================================================
 * 发送端恢复（丢包检测 + 重传 + 拥塞）
 * ============================================================ */
#define RECOVERY_MAX_PKTS 512

typedef struct {
    uint64_t pn;
    size_t   bytes;
    uint8_t* frames;
    size_t   frames_len;
    uint64_t sent_ms;      /* 发送时刻（逻辑时钟） */
    int      acked;
    int      lost;
    int      retransmitted;
} sent_pkt_t;

struct nl_quic_recovery {
    uint64_t mtu, cwnd, ssthresh, bytes_in_flight;
    uint64_t largest_acked;
    int      has_acked;
    sent_pkt_t pkts[RECOVERY_MAX_PKTS];
    size_t   count;
    /* RTT 估计（RFC 9002 §5） */
    uint64_t latest_rtt, srtt, rttvar, min_rtt, max_ack_delay;
    int      has_rtt;
    /* ECN（RFC 9002 §7.4）：已观测到的累计 CE 计数 */
    uint64_t ecn_ce;
};

nl_quic_recovery_t* nl_quic_recovery_new(uint64_t mtu, uint64_t init_cwnd) {
    nl_quic_recovery_t* r = (nl_quic_recovery_t*)calloc(1, sizeof(nl_quic_recovery_t));
    if (!r) return NULL;
    r->mtu = mtu ? mtu : 1200;
    r->cwnd = init_cwnd ? init_cwnd : (10 * r->mtu);
    r->ssthresh = (uint64_t)-1;   /* 慢启动 */
    r->max_ack_delay = 0;         /* 本骨架取 0（生产可设对端 max_ack_delay） */
    return r;
}

void nl_quic_recovery_free(nl_quic_recovery_t* r) {
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) free(r->pkts[i].frames);
    free(r);
}

int nl_quic_recovery_on_sent(nl_quic_recovery_t* r, uint64_t pn, size_t bytes,
                             const uint8_t* frames, size_t frames_len, uint64_t now_ms) {
    if (!r || r->count >= RECOVERY_MAX_PKTS) return -1;
    sent_pkt_t* s = &r->pkts[r->count];
    s->pn = pn;
    s->bytes = bytes;
    s->frames = (frames && frames_len) ? (uint8_t*)malloc(frames_len) : NULL;
    if (frames && frames_len) {
        if (!s->frames) return -1;
        memcpy(s->frames, frames, frames_len);
    }
    s->frames_len = frames_len;
    s->sent_ms = now_ms;
    s->acked = s->lost = s->retransmitted = 0;
    r->count++;
    r->bytes_in_flight += bytes;
    return 0;
}

/* RTT 估计更新（RFC 9002 §5.3，max_ack_delay=0 简化） */
static void rtt_update(nl_quic_recovery_t* r, uint64_t latest) {
    r->latest_rtt = latest;
    if (!r->has_rtt) {
        r->min_rtt = latest;
        r->srtt = latest;
        r->rttvar = latest / 2;
        r->has_rtt = 1;
        return;
    }
    if (latest < r->min_rtt) r->min_rtt = latest;
    uint64_t adjusted = (latest >= r->min_rtt + r->max_ack_delay) ? (latest - r->max_ack_delay) : latest;
    uint64_t diff = (r->srtt > adjusted) ? (r->srtt - adjusted) : (adjusted - r->srtt);
    r->rttvar = (3 * r->rttvar + diff) / 4;
    r->srtt = (7 * r->srtt + adjusted) / 8;
}

/* 时间阈值（RFC 9002 §6.1.2）：9/8 * max(latest_rtt, srtt) */
static uint64_t loss_delay_ms(const nl_quic_recovery_t* r) {
    uint64_t base = r->latest_rtt > r->srtt ? r->latest_rtt : r->srtt;
    if (base == 0) return 0;
    return (9 * base) / 8;
}

static uint64_t mark_time_lost(nl_quic_recovery_t* r, uint64_t now_ms) {
    uint64_t delay = loss_delay_ms(r);
    if (delay == 0) return 0;
    uint64_t cnt = 0;
    for (size_t i = 0; i < r->count; i++) {
        sent_pkt_t* s = &r->pkts[i];
        if (!s->acked && !s->lost && (now_ms - s->sent_ms) > delay) { s->lost = 1; cnt++; }
    }
    return cnt;
}

static void cwnd_on_loss(nl_quic_recovery_t* r) {
    r->ssthresh = r->cwnd / 2;
    if (r->ssthresh < 2 * r->mtu) r->ssthresh = 2 * r->mtu;
    r->cwnd = r->ssthresh;
}

int nl_quic_recovery_on_ack(nl_quic_recovery_t* r,
                            const nl_quic_ack_range_t* ranges, size_t n_ranges,
                            uint64_t now_ms) {
    if (!r || !ranges || n_ranges == 0) return -1;
    uint64_t largest = ranges[0].largest;

    /* RTT 采样：以本次新确认的最大包号的发送时刻为基准 */
    int sample = 0; uint64_t sample_sent = 0;
    for (size_t i = 0; i < r->count; i++) {
        if (r->pkts[i].pn == largest && !r->pkts[i].acked && !r->pkts[i].lost) {
            sample = 1; sample_sent = r->pkts[i].sent_ms; break;
        }
    }

    if (!r->has_acked || largest > r->largest_acked) { r->largest_acked = largest; r->has_acked = 1; }

    uint64_t newly_acked = 0;
    int lost_any = 0;
    for (size_t i = 0; i < r->count; i++) {
        sent_pkt_t* s = &r->pkts[i];
        if (s->acked) continue;
        int in_range = 0;
        for (size_t k = 0; k < n_ranges; k++) {
            if (s->pn >= ranges[k].smallest && s->pn <= ranges[k].largest) { in_range = 1; break; }
        }
        if (in_range) {
            s->acked = 1;
            if (r->bytes_in_flight >= s->bytes) r->bytes_in_flight -= s->bytes; else r->bytes_in_flight = 0;
            newly_acked += s->bytes;
        } else if (s->pn < largest) {
            if (!s->lost) { s->lost = 1; lost_any = 1; }
        }
    }

    if (sample) rtt_update(r, now_ms - sample_sent);

    /* 时间阈值丢包（基于刚更新的 RTT） */
    if (mark_time_lost(r, now_ms) > 0) lost_any = 1;

    if (lost_any) {
        cwnd_on_loss(r);
    } else if (newly_acked) {
        if (r->cwnd < r->ssthresh) r->cwnd += newly_acked;                          /* 慢启动 */
        else r->cwnd += (r->mtu * newly_acked) / (r->cwnd ? r->cwnd : 1);           /* 拥塞避免 */
    }
    return 0;
}

int nl_quic_recovery_on_time(nl_quic_recovery_t* r, uint64_t now_ms, uint64_t* out_lost) {
    if (!r) return -1;
    uint64_t cnt = mark_time_lost(r, now_ms);
    if (cnt > 0) cwnd_on_loss(r);
    if (out_lost) *out_lost = cnt;
    return 0;
}

int nl_quic_recovery_next_retransmit(nl_quic_recovery_t* r,
                                     uint8_t* out, size_t cap, size_t* out_len,
                                     uint64_t* out_pn) {
    if (!r) return 0;
    for (size_t i = 0; i < r->count; i++) {
        sent_pkt_t* s = &r->pkts[i];
        if (s->lost && !s->retransmitted && s->frames_len <= cap) {
            if (s->frames_len) memcpy(out, s->frames, s->frames_len);
            if (out_len) *out_len = s->frames_len;
            if (out_pn) *out_pn = s->pn;
            s->retransmitted = 1;
            return 1;
        }
    }
    return 0;
}

uint64_t nl_quic_recovery_cwnd(const nl_quic_recovery_t* r) { return r ? r->cwnd : 0; }
uint64_t nl_quic_recovery_bytes_in_flight(const nl_quic_recovery_t* r) { return r ? r->bytes_in_flight : 0; }

uint64_t nl_quic_recovery_latest_rtt(const nl_quic_recovery_t* r) { return r ? r->latest_rtt : 0; }
uint64_t nl_quic_recovery_srtt(const nl_quic_recovery_t* r) { return r ? r->srtt : 0; }
uint64_t nl_quic_recovery_rttvar(const nl_quic_recovery_t* r) { return r ? r->rttvar : 0; }
/* PTO = srtt + max(4*rttvar, kGranularity=1) + max_ack_delay（RFC 9002 §6.2.1） */
uint64_t nl_quic_recovery_pto(const nl_quic_recovery_t* r) {
    if (!r || !r->has_rtt) return 0;
    uint64_t g = 4 * r->rttvar;
    if (g < 1) g = 1;
    return r->srtt + g + r->max_ack_delay;
}

/* ============================================================
 * 拥塞控制补充（RFC 9002 §7）：最小窗口 / 持续拥塞 / ECN
 * ============================================================ */
/* 最小拥塞窗口 = 2 * mtu（RFC 9002 §7.2） */
static uint64_t r_cwnd_min(const nl_quic_recovery_t* r) {
    return r ? 2 * r->mtu : 0;
}

uint64_t nl_quic_recovery_cwnd_min(const nl_quic_recovery_t* r) {
    return r_cwnd_min(r);
}

/* 持续拥塞（RFC 9002 §7.6）：窗口收敛到最小值，ssthresh 保持 */
void nl_quic_recovery_on_persistent_congestion(nl_quic_recovery_t* r) {
    if (!r) return;
    r->cwnd = r_cwnd_min(r);
}

/* ECN CE 计数增长即视为一次拥塞事件（RFC 9002 §7.4）：窗口减半 */
void nl_quic_recovery_on_ecn_ce(nl_quic_recovery_t* r, uint64_t ce_total) {
    if (!r) return;
    if (ce_total > r->ecn_ce) {
        r->ecn_ce = ce_total;
        cwnd_on_loss(r);
    }
}

uint64_t nl_quic_recovery_ecn_ce(const nl_quic_recovery_t* r) {
    return r ? r->ecn_ce : 0;
}

/* ============================================================
 * 自测：ACK 帧往返 + 丢包检测/重传 + 拥塞
 * ============================================================ */
int nl_quictls_recovery_selftest(void) {
    uint8_t data[8];
    for (int i = 0; i < 8; i++) data[i] = (uint8_t)(0xA0 + i);

    nl_quic_recovery_t* r = nl_quic_recovery_new(1200, 12000);
    nl_quic_acktrack_t* ack = nl_quic_acktrack_new();
    if (!r || !ack) { nl_quic_recovery_free(r); nl_quic_acktrack_free(ack); return 1; }

    int rc = 0;

    /* 发送 8 个包，每包携带 2 字节帧 {index, data} */
    for (uint64_t pn = 0; pn < 8; pn++) {
        uint8_t fr[2] = { (uint8_t)pn, data[pn] };
        if (nl_quic_recovery_on_sent(r, pn, 100, fr, 2, 0) != 0) { rc = 2; goto done; }
    }

    /* 接收端：丢弃 pn=2 与 pn=5 */
    for (uint64_t pn = 0; pn < 8; pn++) {
        if (pn == 2 || pn == 5) continue;
        nl_quic_acktrack_add(ack, pn);
    }

    /* 生成 ACK 范围 + 编解码往返 */
    nl_quic_ack_range_t ranges[NL_QUIC_MAX_ACK_RANGES]; size_t nr = 0;
    if (nl_quic_acktrack_ranges(ack, ranges, NL_QUIC_MAX_ACK_RANGES, &nr) != 0 || nr == 0) { rc = 3; goto done; }
    /* 期望 2 段：[7..6] 与 [4..3] 与 [1..0] → 共 3 段 */
    if (nr != 3) { rc = 4; goto done; }

    uint8_t abuf[64]; size_t alen = 0;
    if (nl_quic_ack_encode(abuf, sizeof(abuf), 0, ranges, nr, &alen) == 0) { rc = 5; goto done; }
    nl_quic_ack_range_t r2[NL_QUIC_MAX_ACK_RANGES]; size_t nr2 = 0, cons = 0; uint64_t dly = 0;
    if (nl_quic_ack_decode(abuf, alen, &dly, r2, NL_QUIC_MAX_ACK_RANGES, &nr2, &cons) != 0) { rc = 6; goto done; }
    if (nr2 != nr || cons != alen) { rc = 7; goto done; }
    for (size_t i = 0; i < nr; i++)
        if (r2[i].smallest != ranges[i].smallest || r2[i].largest != ranges[i].largest) { rc = 8; goto done; }

    /* 发送端处理 ACK：pn=2/5 判丢，cwnd 减半 */
    uint64_t cwnd_before = nl_quic_recovery_cwnd(r);
    if (nl_quic_recovery_on_ack(r, r2, nr2, 100) != 0) { rc = 9; goto done; }
    uint64_t cwnd_after = nl_quic_recovery_cwnd(r);
    if (cwnd_after >= cwnd_before) { rc = 10; goto done; }          /* 有丢包 → 拥塞窗口下降 */

    /* 重传队列应返回 pn=2 与 pn=5 */
    int got2 = 0, got5 = 0, got_other = 0;
    for (;;) {
        uint8_t out[16]; size_t ol = 0; uint64_t pn = 0;
        int rc2 = nl_quic_recovery_next_retransmit(r, out, sizeof(out), &ol, &pn);
        if (!rc2) break;
        if (pn == 2) got2 = 1;
        else if (pn == 5) got5 = 1;
        else got_other = 1;
    }
    if (!got2 || !got5 || got_other) { rc = 11; goto done; }

    /* 重传成功送达 → 接收端补齐全部 0..7 */
    nl_quic_acktrack_add(ack, 2);
    nl_quic_acktrack_add(ack, 5);
    nl_quic_ack_range_t full[4]; size_t nf = 0;
    if (nl_quic_acktrack_ranges(ack, full, 4, &nf) != 0) { rc = 12; goto done; }
    if (nf != 1 || full[0].smallest != 0 || full[0].largest != 7) { rc = 13; goto done; }

    /* Phase B：RTT 估计与 PTO（pn=7 于 t=0 发送，t=100 确认 → latest_rtt=100） */
    if (nl_quic_recovery_latest_rtt(r) != 100) { rc = 20; goto done; }
    if (nl_quic_recovery_srtt(r) != 100) { rc = 21; goto done; }
    if (nl_quic_recovery_rttvar(r) != 50) { rc = 22; goto done; }
    if (nl_quic_recovery_pto(r) != 300) { rc = 23; goto done; }   /* 100 + max(200,1) */

    /* Phase C：基于时间的丢包检测（RFC 9002 §6.1.2） */
    {
        uint8_t fr[2] = { 8, 0xEE };
        if (nl_quic_recovery_on_sent(r, 8, 100, fr, 2, 1000) != 0) { rc = 24; goto done; }
    }
    {
        uint64_t lost = 0;
        if (nl_quic_recovery_on_time(r, 1130, &lost) != 0 || lost == 0) { rc = 25; goto done; }   /* 130 > 9/8*100=112 */
        int got8 = 0;
        uint8_t out[16]; size_t ol = 0; uint64_t pn = 0;
        while (nl_quic_recovery_next_retransmit(r, out, sizeof(out), &ol, &pn)) if (pn == 8) got8 = 1;
        if (!got8) { rc = 26; goto done; }
    }

    /* Phase D：RFC 9002 §7 拥塞控制补充（最小窗口 / 持续拥塞 / ECN） */
    /* 40：最小拥塞窗口 = 2 * mtu */
    if (nl_quic_recovery_cwnd_min(r) != 2 * 1200) { rc = 40; goto done; }

    /* 41：持续拥塞 → cwnd 收敛到 cwnd_min */
    {
        nl_quic_recovery_t* rp = nl_quic_recovery_new(1200, 12000);
        if (!rp) { rc = 41; goto done; }
        nl_quic_recovery_on_persistent_congestion(rp);
        int bad = (nl_quic_recovery_cwnd(rp) != nl_quic_recovery_cwnd_min(rp));
        nl_quic_recovery_free(rp);
        if (bad) { rc = 41; goto done; }
    }

    /* 42：ECN CE 计数增长触发拥塞事件；无新增则窗口不变 */
    {
        if (nl_quic_recovery_ecn_ce(r) != 0) { rc = 42; goto done; }
        uint64_t cwnd_pre = nl_quic_recovery_cwnd(r);
        nl_quic_recovery_on_ecn_ce(r, 5);
        if (nl_quic_recovery_ecn_ce(r) != 5) { rc = 42; goto done; }
        uint64_t cwnd_post = nl_quic_recovery_cwnd(r);
        if (cwnd_post >= cwnd_pre) { rc = 42; goto done; }      /* 期望窗口减小 */
        nl_quic_recovery_on_ecn_ce(r, 5);                        /* 计数无新增 → 不动作 */
        if (nl_quic_recovery_cwnd(r) != cwnd_post) { rc = 42; goto done; }
    }

done:
    nl_quic_recovery_free(r);
    nl_quic_acktrack_free(ack);
    return rc;
}
