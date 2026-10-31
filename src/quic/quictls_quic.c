/**
 * @file quictls_quic.c
 * @brief 自研 QUIC 传输层 + HTTP/3（C5/C6 最小可用实现）
 * @version 0.1.0
 *
 * 覆盖：
 *   - QUIC 变长整数（RFC 9000 §16）
 *   - CRYPTO 帧（§19.6）/ STREAM 帧（§19.8）编解码
 *   - Initial / Handshake（长首部）/ 1-RTT（短首部）数据包构建与解析（复用 nl_quic_*_packet）
 *   - HTTP/3：SETTINGS/HEADERS/DATA 帧（RFC 9114）+ 最小 QPACK（RFC 9204，Literal Field Line
 *     With Literal Name 子集）
 *   - 端到端自测：QUIC 握手（Initial CH/SH + Handshake 其余）+ 1-RTT 上一次 H3 GET 请求/响应
 *
 * 说明：为可验证的 happy-path 骨架——假设传输有序无丢包（暂未实现 ACK/重传/拥塞/QPACK 动态表）。
 * 参考范式：quicly / ngtcp2 / nghttp3。
 */

/* POSIX 特性宏须先于任何系统头包含（C_EXTENSIONS OFF = -std=c99 严格模式），
 * 供文件末尾的 HTTP/3 服务端/客户端（socket/pthread/unistd/arpa/inet）使用。 */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
    #define _DEFAULT_SOURCE
  #endif
#endif

#include "netleaf_quic.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
  #include <direct.h>
  #define NL_MKDIR(p) _mkdir(p)
#else
  #include <sys/stat.h>
  #define NL_MKDIR(p) mkdir(p, 0755)
#endif

/* ============================================================
 * QUIC 变长整数（RFC 9000 §16）
 * ============================================================ */
NL_QUICTLS_API size_t nl_quic_varint_write(uint8_t* p, size_t cap, uint64_t v) {
    if (v < (1ULL << 6)) {
        if (cap < 1) return 0;
        p[0] = (uint8_t)v; return 1;
    }
    if (v < (1ULL << 14)) {
        if (cap < 2) return 0;
        p[0] = (uint8_t)(0x40 | (v >> 8)); p[1] = (uint8_t)v; return 2;
    }
    if (v < (1ULL << 30)) {
        if (cap < 4) return 0;
        p[0] = (uint8_t)(0x80 | (v >> 24)); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; return 4;
    }
    if (cap < 8) return 0;
    p[0] = (uint8_t)(0xc0 | (v >> 56));
    for (size_t i = 1; i < 8; i++) p[i] = (uint8_t)(v >> (8 * (7 - i)));
    return 8;
}

NL_QUICTLS_API size_t nl_quic_varint_read(const uint8_t* p, size_t len, uint64_t* v) {
    if (len < 1) return 0;
    size_t n = (size_t)1 << (p[0] >> 6);
    if (len < n) return 0;
    uint64_t r = p[0] & 0x3f;
    for (size_t i = 1; i < n; i++) r = (r << 8) | p[i];
    *v = r;
    return n;
}

/* ============================================================
 * 帧编解码
 * ============================================================ */
/* CRYPTO 帧：type(0x06) + offset + length + data */
static int crypto_frame_write(uint8_t* out, size_t cap, uint64_t off,
                              const uint8_t* data, size_t len, size_t* out_len) {
    size_t p = 0, n;
    n = nl_quic_varint_write(out + p, cap - p, 0x06); if (!n) return -1; p += n;
    n = nl_quic_varint_write(out + p, cap - p, off);  if (!n) return -1; p += n;
    n = nl_quic_varint_write(out + p, cap - p, len);  if (!n) return -1; p += n;
    if (p + len > cap) return -1;
    if (len) memcpy(out + p, data, len);
    p += len;
    *out_len = p;
    return 0;
}

/* 解析 CRYPTO 帧（datagram 起始）：返回 0 并给出 offset/data/len/consumed；
 * -1 = 帧非 CRYPTO 类型（调用方应判为"本 datagram 无 CRYPTO 帧"并跳过重处理）；
 * -2 = 越界/截断（调用方应保留该 CRYPTO 段等待后续 datagram 补齐）。 */
static int crypto_frame_read(const uint8_t* d, size_t len,
                             uint64_t* off, const uint8_t** data, size_t* dlen, size_t* consumed) {
    size_t p = 0, n;
    uint64_t type = 0;
    n = nl_quic_varint_read(d + p, len - p, &type); if (!n) return -1; p += n;
    if (type != 0x06) return -1;
    n = nl_quic_varint_read(d + p, len - p, off); if (!n) return -2; p += n;
    n = nl_quic_varint_read(d + p, len - p, &type); if (!n) return -2; p += n;   /* length */
    if (p + type > len) return -2;
    *data = d + p; *dlen = (size_t)type;
    *consumed = p + (size_t)type;
    return 0;
}

/* STREAM 帧：type(0x08|LEN|FIN) + sid + (offset) + length + data（本实现 OFF 不置位） */
static int stream_frame_write(uint8_t* out, size_t cap, uint64_t sid,
                              const uint8_t* data, size_t len, int fin, size_t* out_len) {
    uint8_t t = (uint8_t)(0x08 | 0x02 | (fin ? 0x01 : 0x00));
    size_t p = 0, n;
    if (p + 1 > cap) return -1;
    out[p++] = t;
    n = nl_quic_varint_write(out + p, cap - p, sid); if (!n) return -1; p += n;
    n = nl_quic_varint_write(out + p, cap - p, len); if (!n) return -1; p += n;
    if (p + len > cap) return -1;
    if (len) memcpy(out + p, data, len);
    p += len;
    *out_len = p;
    return 0;
}

static int stream_frame_read(const uint8_t* d, size_t len,
                             uint64_t* sid, uint64_t* off,
                             const uint8_t** data, size_t* dlen, int* fin, size_t* consumed) {
    if (len < 1) return -1;
    uint8_t t = d[0];
    /* RFC 9000 §19.9：STREAM 帧 base type 0x08，bit2=has_offset, bit1=has_length, bit0=FIN。
     * type 字节合法范围 0x08~0x0f。 */
    if ((t & 0xf0) != 0x08) return -1;
    int has_off = (t & 0x04) != 0;
    int has_len = (t & 0x02) != 0;
    *fin = (t & 0x01) != 0;
    size_t p = 1, n;
    n = nl_quic_varint_read(d + p, len - p, sid); if (!n) return -1; p += n;
    *off = 0;
    if (has_off) { n = nl_quic_varint_read(d + p, len - p, off); if (!n) return -1; p += n; }
    uint64_t dl = 0;
    if (has_len) { n = nl_quic_varint_read(d + p, len - p, &dl); if (!n) return -1; p += n; }
    else dl = len - p;
    if (p + dl > len) return -1;
    *data = d + p; *dlen = (size_t)dl;
    *consumed = p + (size_t)dl;
    return 0;
}

/* HTTP/3 帧：type + length + payload */
static int h3_frame_write(uint8_t* out, size_t cap, uint64_t type,
                          const uint8_t* data, size_t len, size_t* out_len) {
    size_t p = 0, n;
    n = nl_quic_varint_write(out + p, cap - p, type); if (!n) return -1; p += n;
    n = nl_quic_varint_write(out + p, cap - p, len);  if (!n) return -1; p += n;
    if (p + len > cap) return -1;
    if (len) memcpy(out + p, data, len);
    p += len;
    *out_len = p;
    return 0;
}

static int h3_frame_read(const uint8_t* d, size_t len,
                         uint64_t* type, const uint8_t** data, size_t* dlen, size_t* consumed) {
    size_t p = 0, n;
    n = nl_quic_varint_read(d + p, len - p, type); if (!n) return -1; p += n;
    uint64_t dl = 0;
    n = nl_quic_varint_read(d + p, len - p, &dl); if (!n) return -1; p += n;
    if (p + dl > len) return -1;
    *data = d + p; *dlen = (size_t)dl;
    *consumed = p + (size_t)dl;
    return 0;
}

/* ============================================================
 * QPACK 字段行（复用 quictls_qpack.c 公共实现）
 * ============================================================ */
typedef nl_h3_qpack_field_t h3_kv_t;

static int qpack_encode_field(uint8_t* out, size_t cap, const char* name, const char* value) {
    return nl_h3_qpack_encode_field(out, cap, name, value);
}

static int qpack_decode(const uint8_t* d, size_t len, h3_kv_t* out, int max, int* count) {
    return nl_h3_qpack_decode(d, len, out, max, count);
}

/* ============================================================
 * ACK 帧（QUIC RFC 9000：type=0x02, delay, ack_range, n_ranges, ...）
 * ============================================================ */
static int ack_frame_write(uint8_t* out, size_t cap, uint64_t largest_acked,
                           uint64_t ack_delay, size_t* out_len) {
    size_t p = 0, n;
    n = nl_quic_varint_write(out + p, cap - p, 0x02); if (!n) return -1; p += n;   /* type ACK */
    /* RFC 9000 §19.3：ACK 帧 = type + largest_acked + ack_delay + ack_range_count + ranges。
     * 旧实现把 ack_delay 写在 largest_acked 之前，真实客户端解析会失败，此处修正顺序。 */
    n = nl_quic_varint_write(out + p, cap - p, largest_acked); if (!n) return -1; p += n; /* largest_acked */
    n = nl_quic_varint_write(out + p, cap - p, ack_delay); if (!n) return -1; p += n;  /* ack_delay */
    n = nl_quic_varint_write(out + p, cap - p, 0); if (!n) return -1; p += n;   /* ack_range_count = 0（无额外 range） */
    /* RFC 9000 §19.3：first range 的 width 无条件存在（紧随 count 之后，count=0 时也写），
     * 否则真实客户端（aioquic）按 RFC 解析会越界误读后续帧字节 → 1-RTT 响应解析错位。 */
    n = nl_quic_varint_write(out + p, cap - p, 0); if (!n) return -1; p += n;   /* first range width = 0 */
    *out_len = p;
    return 0;
}

/* 解析 ACK 帧：type + largest_acked + ack_delay + ack_range_count [+ 各 range]。
 * 与 ack_frame_write / RFC 9000 §19.3 一致：ack_range_count = 额外 range 数量，
 * first range 的 width 无条件紧随 count 之后，随后每多一个 range 读 gap + width。
 * 返回 0 成功；*largest_acked / *ack_delay 写出，*consumed 为完整帧消耗字节数。 */
static int ack_frame_read(const uint8_t* d, size_t len,
                          uint64_t* largest_acked, uint64_t* ack_delay, size_t* consumed) {
    size_t p = 0, n;
    uint64_t ftype = 0, cnt = 0, w = 0;
    n = nl_quic_varint_read(d + p, len - p, &ftype); if (!n) return -1; p += n;
    if (ftype != 0x02) return -1;
    n = nl_quic_varint_read(d + p, len - p, largest_acked); if (!n) return -1; p += n;
    n = nl_quic_varint_read(d + p, len - p, ack_delay); if (!n) return -1; p += n;
    n = nl_quic_varint_read(d + p, len - p, &cnt); if (!n) return -1; p += n;   /* ack_range_count */
    /* RFC 9000 §19.3：first range width 无条件存在于 count 之后（count=0 时也有） */
    n = nl_quic_varint_read(d + p, len - p, &w); if (!n) return -1; p += n;     /* first width */
    for (size_t i = 0; i < cnt; i++) {
        if (p + 1 > len) return -1;
        n = nl_quic_varint_read(d + p, len - p, &w); if (!n) return -1; p += n; /* gap */
        n = nl_quic_varint_read(d + p, len - p, &w); if (!n) return -1; p += n; /* width */
    }
    if (consumed) *consumed = p;
    return 0;
}

/* ============================================================
 * 数据包构建 / 解析
 * ============================================================ */
/* 长首部（type: 0=Initial, 2=Handshake）
 * pad_len：在明文帧后追加的 PADDING 帧（0x00）字节数。
 * RFC 9000 §14.3：Initial 包在 datagram 内必须被 PADDING 填充到至少 1200B，
 * 否则客户端（aioquic）无法完成 loss detection 与 flight sizing，
 * 表现为持续 PTO 重传 Initial、永不推进到 Handshake。
 * pad_len 计入 CRYPTO 之后的 PADDING 帧，使 datagram 总长恰为 1200B。
 * out_pn_offset / out_aad / out_aad_len 保持不变，仅在调用方需要时回填。 */
static int pkt_build_long(uint8_t type,
                          const uint8_t* dcid, size_t dcid_len,
                          const uint8_t* scid, size_t scid_len,
                          uint64_t pn, size_t pn_len,
                          const uint8_t* frames, size_t frames_len,
                          const nl_quic_keys_t* keys,
                          uint8_t* out, size_t cap, size_t* out_len,
                          size_t* out_pn_offset,
                          uint8_t* out_aad, size_t* out_aad_len,
                          size_t pad_len) {
    size_t p = 0, n;
    /* 包号长度：服务端 Initial/Handshake 用 2 字节包号（对齐 aioquic 客户端的
     * PACKET_NUMBER_SEND_SIZE=2 与 RFC 9000 §17.2 长首 b0 低 2 位 = pn_len-1）。
     * 旧实现强制 pn_len=1 且 b0 低 2 位写 0，导致客户端按 pn_len 解析 PN 字段、
     * 首部保护掩码采样窗口整体错位 1 字节，无法解密我们发出的 Handshake 包 →
     * 客户端持续 PTO 重传 Initial、握手卡死。现由调用方传入 pn_len(=2)。 */
    if (pn_len == 0 || pn_len > 4) pn_len = 2;
    if (p + 1 > cap) return -1;
    /* RFC 9000 §17.2 长首第 0 字节：
     *   0   1   2   3   4   5   6   7
     *  +---+---+---+---+---+---+---+---+
     *  | 1 | 1 | T | T | 0 | 0 | L | L |
     *  +---+---+---+---+---+---+---+---+
     *  bit7=1 (long header), bit6=1 (fixed)
     *  bits5-4 = type (0=Initial, 1=0-RTT, 2=Handshake, 3=Retry)
     *  bits3-2 = 0 (reserved, HP 覆盖)
     *  bits1-0 = pn_len-1 (L 值)
     */
    out[p++] = (uint8_t)(0xC0 | (type << 4) | (pn_len - 1));
    if (p + 4 > cap) return -1;
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 1;   /* version 1 */
    if (p + 1 + dcid_len > cap) return -1;
    out[p++] = (uint8_t)dcid_len; memcpy(out + p, dcid, dcid_len); p += dcid_len;
    /* RFC 9000 §17.2：SCID 字段（len+bytes）存在于所有长首部（Initial/Handshake/
     * 0-RTT/1-RTT 均携带 SCID），故此处对所有 type 无条件写入，保持与解析侧
     * pkt_parse_long 一致。仅 Retry Token（下方 type==0）为 Initial 专属字段。 */
    if (p + 1 + scid_len > cap) return -1;
    out[p++] = (uint8_t)scid_len; memcpy(out + p, scid, scid_len); p += scid_len;
    /* RFC 9000 §17.2：Retry Token 字段（变长）仅存在于 Initial 包（type=0）。
     * Handshake/0-RTT/1-RTT 长首在 SCID 之后直接是 Length 字段，无 Token 字段。
     * 旧实现对所有 type 都写 token，使 Handshake 包多 1 字节，接收方（aioquic）
     * 按 RFC 解析时 Length 字段错位 → 首部保护采样窗口偏移、无法解密。
     * 现与解析侧 pkt_parse_long 一致：仅 type==0 时写 token。 */
    if (type == 0) { n = nl_quic_varint_write(out + p, cap - p, 0); if (!n) return -1; p += n; }  /* token */

    /* RFC 9000 §17.2：Length 字段覆盖从 PN 到 datagram 末尾（PN + payload + PADDING + TAG）。
     * pkt_long_total_len 解析侧 total = q + length，q 已跳过 Length varint 停在 PN 之前，
     * 故 length 必须包含 pn_len 才能覆盖到 datagram 末尾。 */
    uint64_t length = (uint64_t)(pn_len + frames_len + pad_len + NL_QUIC_TAG_LEN);
    n = nl_quic_varint_write(out + p, cap - p, length); if (!n) return -1; p += n;

    size_t pn_offset = p;
    if (p + pn_len > cap) return -1;
    for (size_t i = 0; i < pn_len; i++) out[p++] = (uint8_t)(pn >> (8 * (pn_len - 1 - i)));
    if (p + frames_len > cap) return -1;
    memcpy(out + p, frames, frames_len); p += frames_len;
    /* RFC 9000 §19.8：PADDING 帧（0x00）填充至 datagram 尾。
     * 仅在 pad_len>0 时生效；对服务端 H3 Initial(SH/ACK) 包填充至 1200B，
     * 其余调用（HS/1-RTT/自测）pad_len=0 保持原有行为。 */
    if (pad_len > 0) {
        if (p + pad_len > cap) return -1;
        memset(out + p, 0, pad_len); p += pad_len;
    }

    size_t total = 0;
    /* dump: 保护前的明文 AAD（pkt[0..pn_offset+pn_len)），必须在 HP apply 前抓取，
     * 否则 PN 与 b0 低 4 位已被 XOR，离线 GCM 重算 tag 无法对齐。 */
    size_t aad_dump_len = pn_offset + pn_len;
    uint8_t aad_dump[64];
    if (aad_dump_len > sizeof(aad_dump)) aad_dump_len = sizeof(aad_dump);
    memcpy(aad_dump, out, aad_dump_len);
    /* AAD 长度保持为 pn_offset+pn_len（仅头部，含 PN），payload 部分（含 PADDING）被 GCM 加密。 */
    if (nl_quic_protect_packet_ex(keys, pn, 1, out, pn_offset, pn_len, frames_len + pad_len, &total) != 0) return -1;
    *out_len = total;
    if (out_pn_offset) *out_pn_offset = pn_offset;
    if (out_aad && out_aad_len) {
        if (*out_aad_len < aad_dump_len) return -1;
        memcpy(out_aad, aad_dump, aad_dump_len);
        *out_aad_len = aad_dump_len;
    }
    return 0;
}

/* 解析长首部；pkt 可变（就地解保护）。
 * 不再依赖调用方传入的期望包号：解除首部保护后从包字节自解析实际包号，
 * 再以其构造 AEAD nonce，保证 Initial/Handshake 重传（包内 PN 为原始值）也能正确解密。
 * out_actual_pn 可选返回实际包号（供 ACK 帧 largest_acked 使用）。 */
static int pkt_parse_long(uint8_t* pkt, size_t len, const nl_quic_keys_t* keys, uint64_t expected_pn,
                         const uint8_t** frames, size_t* frames_len, uint8_t* out_type,
                         uint64_t* out_actual_pn) {
    (void)expected_pn; /* 已改用包内自解析实际 PN，入参保留仅为兼容旧调用签名 */
    if (len < 8) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: len=%zu < 8\n", len); fflush(stderr);
#endif
        return -1;
    }
    uint8_t b0 = pkt[0];
    if (!(b0 & 0x80)) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: b0=0x%02x not long header (bit7=0)\n", b0); fflush(stderr);
#endif
        return -2;
    }
    /* 长首 b0：type 在 bits5-4 (RFC 9000 §17.2)，与 aioquic (first_byte & 0x30) >> 4 对齐；
     * 旧实现 (b0 >> 2) & 0x03 读 bits3-2（pn_len 位段），type 识别错误。 */
    uint8_t type = (uint8_t)((b0 >> 4) & 0x03);
    size_t p = 1 + 4;
    if (p >= len) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: p=%zu >= len=%zu (b0+version)\n", p, len); fflush(stderr);
#endif
        return -3;
    }
    /* 长首部布局（RFC 9000 §17.2）：b0 + version(4) + DCID_len + DCID + SCID_len + SCID + ...
     * 与 pkt_build_long 写入顺序一致：先 DCID 字段，再 SCID 字段 */
    size_t dcid_len = pkt[p++]; p += dcid_len;
    if (p >= len) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: after DCID p=%zu >= len=%zu dcid_len=%zu\n", p, len, dcid_len); fflush(stderr);
#endif
        return -3;
    }
    size_t scid_len = pkt[p++]; p += scid_len;
    if (p >= len) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: after SCID p=%zu >= len=%zu scid_len=%zu\n", p, len, scid_len); fflush(stderr);
#endif
        return -3;
    }
    if (type == 0) {
        uint64_t tl = 0; size_t tn = nl_quic_varint_read(pkt + p, len - p, &tl); if (!tn) {
#ifdef NLH3_DBG
            fprintf(stderr, "[pkt_parse_long] FAIL: token varint read at p=%zu\n", p); fflush(stderr);
#endif
            return -3; } p += tn;
        if (p + tl > len) {
#ifdef NLH3_DBG
            fprintf(stderr, "[pkt_parse_long] FAIL: token overruns p+tl=%zu > len=%zu tl=%llu\n", p + tl, len, (unsigned long long)tl); fflush(stderr);
#endif
            return -3;
        }
        p += tl;
    }
    uint64_t length = 0;
    size_t n = nl_quic_varint_read(pkt + p, len - p, &length); if (!n) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: length varint read at p=%zu\n", p); fflush(stderr);
#endif
        return -3; } p += n;
    size_t pn_offset = p;

    /* 先解除首部保护（逐候选 pn_len + length 一致性校验），再按"包头 length 字段"计算密文长度并 AEAD 解密。
     * RFC 9001 §5.4.1：b0 的 pn_len 位被 HP 掩码覆盖，线缆 b0 不可信，
     * 因此必须用 length 字段逐候选（1..4）校验：解出的 b0 低 2 位必须等于 pn_len-1，
     * 且 length == pn_len + ct_len + 16。命中即回写 b0/PN 明文。
     * length 是权威值（RFC 9000 §12.3）：datagram 尾部可能带 PADDING 填充
     * （aioquic 把 Initial 填充到 1200B），若改用"datagram 实际包尾"计算 ct_len
     * 会把填充字节误当密文 → AEAD 失败 → 服务端解不出客户端 ClientHello → 永不回复，
     * 客户端 idle 超时。故必须用 length varint（pn_len + CRYPTO 密文 + 16B tag）。 */
    size_t pn_len = 0, plen = 0;
    int hprc = nl_quic_hp_unprotect_len(keys, pkt, pn_offset, 1, len, length, &pn_len);
    if (hprc != 0) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: HP unprotect rc=%d pn_offset=%zu len=%zu\n", hprc, pn_offset, len); fflush(stderr);
#endif
        return -5;
    }
#ifdef NLH3_DBG
    fprintf(stderr, "[pkt_parse_long] HP OK: pn_len=%zu actual_pn=0x%02x keys_hp=%02x%02x%02x%02x\n",
            pn_len, pn_len > 0 ? pkt[pn_offset] : 0,
            keys->hp[0], keys->hp[1], keys->hp[2], keys->hp[3]);
    fflush(stderr);
#endif
    if (length < pn_len + NL_QUIC_TAG_LEN) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: length=%llu < pn_len+tag=%zu+16\n", (unsigned long long)length, pn_len); fflush(stderr);
#endif
        return -4;
    }
    size_t ct_len = (size_t)length - pn_len - NL_QUIC_TAG_LEN;
    if (pn_offset + pn_len + ct_len + NL_QUIC_TAG_LEN > len) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: pn_offset+pn_len+ct_len+tag=%zu > len=%zu\n",
                pn_offset + pn_len + ct_len + NL_QUIC_TAG_LEN, len); fflush(stderr);
#endif
        return -4;
    }
    uint64_t actual_pn = 0;
    for (size_t i = 0; i < pn_len; i++)
        actual_pn = (actual_pn << 8) | (uint64_t)pkt[pn_offset + i];
    int aerc = nl_quic_unprotect_payload(keys, actual_pn, pkt, pn_offset, pn_len, ct_len, &plen);
    if (aerc != 0) {
#ifdef NLH3_DBG
        fprintf(stderr, "[pkt_parse_long] FAIL: AEAD decrypt rc=%d actual_pn=%llu ct_len=%zu keys_key=%02x%02x%02x%02x keys_iv=%02x%02x%02x%02x\n",
                aerc, (unsigned long long)actual_pn, ct_len,
                keys->key[0], keys->key[1], keys->key[2], keys->key[3],
                keys->iv[0], keys->iv[1], keys->iv[2], keys->iv[3]);
        fflush(stderr);
#endif
        return -5;
    }
#ifdef NLH3_DBG
    fprintf(stderr, "[pkt_parse_long] OK: type=%d actual_pn=%llu plen=%zu b0=0x%02x\n",
            type, (unsigned long long)actual_pn, plen, b0);
    fflush(stderr);
#endif
    *frames = pkt + pn_offset + pn_len;
    *frames_len = plen;
    if (out_type) *out_type = type;
    if (out_actual_pn) *out_actual_pn = actual_pn;
    return 0;
}

/* 扫描 datagram 中指定下标的长包（pkt+off 起），按 RFC 9000 §17.2 解 AAD 解密，
 * 返回该包在 datagram 内的总线上长度（length varint + pn_offset）：
 * off + pn_offset + length。返回 0 表示 off 处无法再解析出有效长包。
 * 仅用于 datagram 多包扫描；就地修改 pkt（HP 解保护）。 */
static size_t pkt_long_total_len(const uint8_t* pkt, size_t dlen, size_t off) {
    if (off >= dlen) return 0;
    uint8_t* p = (uint8_t*)pkt + off;
    size_t rem = dlen - off;
    if (rem < 8) return 0;
    uint8_t b0 = p[0];
    if (!(b0 & 0x80)) return 0;                       /* 非长首 */
    /* type 在 bits5-4，与 aioquic 对齐 */
    uint8_t type = (uint8_t)((b0 >> 4) & 0x03);
    size_t q = 1 + 4;
    if (q >= rem) return 0;
    size_t dcid_len = p[q++]; q += dcid_len;
    if (q >= rem) return 0;
    size_t scid_len = p[q++]; q += scid_len;
    if (q >= rem) return 0;
    if (type == 0) {                                    /* 读 token varint */
        uint64_t tl = 0; size_t tn = nl_quic_varint_read(p + q, rem - q, &tl);
        if (!tn) return 0; q += tn;
        if (q + tl > rem) return 0;
        q += tl;
    }
    uint64_t length = 0; size_t ln = nl_quic_varint_read(p + q, rem - q, &length);
    if (!ln) return 0; q += ln;
    /* 该包总长 = 到 length varint 的字节数 q + length（length 权威值） */
    size_t total = q + (size_t)length;
    if (total > rem) return 0;
    (void)p;
    return total;
}

/* 短首部（1-RTT）
 * RFC 9000 §17.2：
 *   0   1   2   3   4   5   6   7
 *  +---+---+---+---+---+---+---+---+
 *  | 0 | 1 | K | S | 0 | 0 | L | L |
 *  +---+---+---+---+---+---+---+---+
 *  bit7=0 (short), bit6=1 (fixed),
 *  bit5=key_phase, bit4=spinning,
 *  bits3-2=0 (reserved, HP 覆盖), bits1-0=pn_len-1
 *  b0 = 0x40 | (key_phase << 5) | (spinning << 4) | (pn_len - 1)
 *  本实现 key_phase=0, spinning=0。
 * HP 掩码宽度：mask[0] & 0x1f（低 5 位，RFC 9001 §5.4.2）。 */
static int pkt_build_short(uint64_t pn, size_t pn_len,
                           const uint8_t* frames, size_t frames_len,
                           const nl_quic_keys_t* keys,
                           const uint8_t* dcid, size_t dcid_len,
                           uint8_t* out, size_t cap, size_t* out_len) {
    size_t p = 0;
    if (p + 1 > cap) return -1;
    out[p++] = (uint8_t)(0x40 | (pn_len - 1));
    if (dcid_len > 0) {
        if (p + dcid_len > cap) return -1;
        memcpy(out + p, dcid, dcid_len); p += dcid_len;
    }
    size_t pn_offset = p;
    if (p + pn_len > cap) return -1;
    for (size_t i = 0; i < pn_len; i++) out[p++] = (uint8_t)(pn >> (8 * (pn_len - 1 - i)));
    if (p + frames_len > cap) return -1;
    memcpy(out + p, frames, frames_len); p += frames_len;

    size_t total = 0;
    if (nl_quic_protect_packet_ex(keys, pn, 0, out, pn_offset, pn_len, frames_len, &total) != 0) return -1;
    *out_len = total;
    return 0;
}

static int pkt_parse_short(uint8_t* pkt, size_t len, const nl_quic_keys_t* keys, uint64_t expected_pn,
                           const uint8_t** frames, size_t* frames_len, uint64_t* out_actual_pn,
                           size_t dcid_len) {
    (void)expected_pn; /* 已不使用：从包中自动解析 PN */
    if (len < 1 + dcid_len + 1 + NL_QUIC_TAG_LEN) return -1;
    /* 先解除首部保护：HP 掩首字节低 5 位（mask[0]&0x1f）与 PN 字节，
     * 解保护后 pkt[0] 还原为明文 b0（RFC 9000 短首 bit7=0, bit6=1）。
     * 1-RTT 短首包头布局：b0(1) + DCID(dcid_len) + PN(pn_len) + payload + tag。
     * 短首无 length 字段，由 total_len 定界密文。 */
    size_t pn_offset = 1 + dcid_len, pn_len = 0;
    if (nl_quic_hp_unprotect_len(keys, pkt, pn_offset, 0, len, 0, &pn_len) != 0) return -1;
    uint8_t b0 = pkt[0];
    if (b0 & 0x80) return -1;                     /* RFC 9000 §17.2：短首 bit7=0 */
    if (pn_offset + pn_len + NL_QUIC_TAG_LEN > len) return -1;
    size_t ct_len = len - pn_offset - pn_len - NL_QUIC_TAG_LEN;
    uint64_t actual_pn = 0;
    for (size_t i = 0; i < pn_len; i++)
        actual_pn = (actual_pn << 8) | pkt[pn_offset + i];
    if (nl_quic_unprotect_payload(keys, actual_pn, pkt, pn_offset, pn_len, ct_len, frames_len) != 0) return -1;
    *frames = pkt + pn_offset + pn_len;
    if (out_actual_pn) *out_actual_pn = actual_pn;
    return 0;
}

/* 1-RTT 短首部：真实客户端（aioquic 1.4.0 默认 connection_id_length=8）
 * 的 1-RTT 包 DCID 长度 = 8 字节（client 的 host_cid）。
 * 本实现发 0 长 SCID，但客户端仍用 8B DCID 匹配。
 * 服务端收到的 client 1-RTT 包有 8B DCID，服务端发出的 1-RTT 包
 * 需携带 client 的 8B host_cid 作为 DCID，客户端才能正确匹配。 */

/* ============================================================
 * 端到端自测：QUIC 握手 + H3 请求/响应
 * ============================================================ */
NL_QUICTLS_API int nl_quictls_quic_selftest(void) {
    uint8_t DCID[8]  = {1,2,3,4,5,6,7,8};
    uint8_t CSID[8]  = {9,9,9,9,9,9,9,9};   /* 客户端 SCID */
    uint8_t SSID[8]  = {7,7,7,7,7,7,7,7};   /* 服务端 SCID */

    nl_tls13_conn_t* cli = NULL;
    nl_tls13_conn_t* srv = NULL;
    int rc = 0;
    static char cert_pem[4096], key_pem[4096];

    /* Initial 密钥（由 DCID 派生，RFC 9001 §5.2） */
    nl_quic_keys_t ci_keys, si_keys;
    if (nl_quic_initial_keys(DCID, sizeof(DCID), &ci_keys, &si_keys) != 0) return 1;

    if (nl_tls13_gen_self_signed(cert_pem, sizeof(cert_pem), key_pem, sizeof(key_pem)) != 0) return 2;
    cli = nl_tls13_conn_new(0);
    srv = nl_tls13_conn_new(1);
    if (!cli || !srv) { rc = 3; goto done; }
    if (nl_tls13_conn_set_cert(srv, cert_pem, key_pem) != 0) { rc = 4; goto done; }
    if (nl_tls13_conn_set_ca(cli, cert_pem) != 0) { rc = 5; goto done; }

    /* ---- Initial：客户端 CH ---- */
    uint8_t ch[512]; size_t ch_len = 0;
    if (nl_tls13_client_hello(cli, ch, sizeof(ch), &ch_len) != 0) { rc = 6; goto done; }
    uint8_t frames[2048]; size_t f_len = 0;
    if (crypto_frame_write(frames, sizeof(frames), 0, ch, ch_len, &f_len) != 0) { rc = 7; goto done; }
    uint8_t pk1[2048]; size_t pk1_len = 0;
    if (pkt_build_long(0, DCID, sizeof(DCID), CSID, sizeof(CSID), 0, 2,
                       frames, f_len, &ci_keys, pk1, sizeof(pk1), &pk1_len, NULL, NULL, NULL, 0) != 0) { rc = 8; goto done; }

    /* 服务端收 Initial */
    {
        uint8_t buf[2048]; if (pk1_len > sizeof(buf)) { rc = 9; goto done; }
        memcpy(buf, pk1, pk1_len);
        const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
        if (pkt_parse_long(buf, pk1_len, &ci_keys, 0, &fr, &fl, &ty, NULL) != 0 || ty != 0) { rc = 10; goto done; }
        uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, c2 = 0;
        if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &c2) != 0 || off != 0) { rc = 11; goto done; }
        uint8_t rch[512]; if (cdl > sizeof(rch)) { rc = 12; goto done; }
        memcpy(rch, cd, cdl);

        size_t r = nl_tls13_server_handshake(srv, rch, cdl, frames, sizeof(frames), &f_len);
        if (r != 0) { rc = 13; goto done; }

        /* 拆分 SH（首条消息）与其余 */
        size_t sh_len = 0;
        if (nl_tls13_parse_handshake(frames, f_len, NULL, NULL, NULL, &sh_len) != 0) { rc = 14; goto done; }

        nl_quic_keys_t s_hr, s_hw;
        if (nl_tls13_conn_get_handshake_keys(srv, &s_hr, &s_hw) != 0) { rc = 15; goto done; }

        uint8_t cbuf[2048];

        /* 服务端 Initial：SH */
        size_t f2 = 0;
        if (crypto_frame_write(cbuf, sizeof(cbuf), 0, frames, sh_len, &f2) != 0) { rc = 16; goto done; }
        uint8_t pk2[1024]; size_t pk2_len = 0;
        if (pkt_build_long(0, CSID, sizeof(CSID), SSID, sizeof(SSID), 1, 2,
                           cbuf, f2, &si_keys, pk2, sizeof(pk2), &pk2_len, NULL, NULL, NULL, 0) != 0) { rc = 17; goto done; }

        /* 服务端 Handshake：其余（EE..Fin） */
        const uint8_t* rest = frames + sh_len;
        size_t rest_len = f_len - sh_len;
        size_t f3 = 0;
        if (crypto_frame_write(cbuf, sizeof(cbuf), 0, rest, rest_len, &f3) != 0) { rc = 18; goto done; }
        uint8_t pk3[2048]; size_t pk3_len = 0;
        if (pkt_build_long(2, CSID, sizeof(CSID), SSID, sizeof(SSID), 0, 2,
                           cbuf, f3, &s_hw, pk3, sizeof(pk3), &pk3_len, NULL, NULL, NULL, 0) != 0) { rc = 19; goto done; }

        /* 客户端收 Initial（SH） */
        uint8_t b2[1024]; if (pk2_len > sizeof(b2)) { rc = 20; goto done; }
        memcpy(b2, pk2, pk2_len);
        const uint8_t* fr2 = NULL; size_t fl2 = 0; uint8_t ty2 = 0;
        if (pkt_parse_long(b2, pk2_len, &si_keys, 1, &fr2, &fl2, &ty2, NULL) != 0) { rc = 21; goto done; }
        uint64_t o2 = 0; const uint8_t* cd2 = NULL; size_t cdl2 = 0, cc2 = 0;
        if (crypto_frame_read(fr2, fl2, &o2, &cd2, &cdl2, &cc2) != 0) { rc = 22; goto done; }
        if (nl_tls13_client_handshake_1(cli, cd2, cdl2) != 0) { rc = 23; goto done; }

        /* 客户端收 Handshake（其余） */
        nl_quic_keys_t c_hr, c_hw;
        if (nl_tls13_conn_get_handshake_keys(cli, &c_hr, &c_hw) != 0) { rc = 24; goto done; }
        uint8_t b3[2048]; if (pk3_len > sizeof(b3)) { rc = 25; goto done; }
        memcpy(b3, pk3, pk3_len);
        const uint8_t* fr3 = NULL; size_t fl3 = 0; uint8_t ty3 = 0;
        if (pkt_parse_long(b3, pk3_len, &c_hr, 0, &fr3, &fl3, &ty3, NULL) != 0) { rc = 26; goto done; }
        uint64_t o3 = 0; const uint8_t* cd3 = NULL; size_t cdl3 = 0, cc3 = 0;
        if (crypto_frame_read(fr3, fl3, &o3, &cd3, &cdl3, &cc3) != 0) { rc = 27; goto done; }
        uint8_t cf[128]; size_t cf_len = 0;
        if (nl_tls13_client_handshake_2(cli, cd3, cdl3, cf, sizeof(cf), &cf_len) != 0) { rc = 28; goto done; }
        if (!nl_tls13_conn_established(cli) || !nl_tls13_conn_peer_verified(cli)) { rc = 29; goto done; }

        /* 客户端 Handshake：client Finished */
        size_t f4 = 0;
        if (crypto_frame_write(frames, sizeof(frames), 0, cf, cf_len, &f4) != 0) { rc = 30; goto done; }
        uint8_t pk4[512]; size_t pk4_len = 0;
        if (pkt_build_long(2, CSID, sizeof(CSID), SSID, sizeof(SSID), 1, 2,
                           frames, f4, &c_hw, pk4, sizeof(pk4), &pk4_len, NULL, NULL, NULL, 0) != 0) { rc = 31; goto done; }

        /* 服务端收 Handshake（client Finished） */
        uint8_t b4[512]; if (pk4_len > sizeof(b4)) { rc = 32; goto done; }
        memcpy(b4, pk4, pk4_len);
        const uint8_t* fr4 = NULL; size_t fl4 = 0; uint8_t ty4 = 0;
        if (pkt_parse_long(b4, pk4_len, &s_hr, 1, &fr4, &fl4, &ty4, NULL) != 0) { rc = 33; goto done; }
        uint64_t o4 = 0; const uint8_t* cd4 = NULL; size_t cdl4 = 0, cc4 = 0;
        if (crypto_frame_read(fr4, fl4, &o4, &cd4, &cdl4, &cc4) != 0) { rc = 34; goto done; }
        if (nl_tls13_server_finish(srv, cd4, cdl4) != 0) { rc = 35; goto done; }
        if (!nl_tls13_conn_established(srv)) { rc = 36; goto done; }
    }

    /* ---- 1-RTT：H3 请求/响应 ---- */
    {
        nl_quic_keys_t c_read, c_write, s_read, s_write;
        if (nl_tls13_conn_get_1rtt_keys(cli, &c_read, &c_write) != 0) { rc = 37; goto done; }
        if (nl_tls13_conn_get_1rtt_keys(srv, &s_read, &s_write) != 0) { rc = 38; goto done; }

        /* 客户端：控制流 SETTINGS + 请求流 HEADERS(GET /) with FIN */
        uint8_t fbuf[1024]; size_t fp = 0, n = 0;

        /* SETTINGS 空 + STREAM(control=2) */
        uint8_t h3set[8]; size_t h3set_len = 0;
        if (h3_frame_write(h3set, sizeof(h3set), 0x04, NULL, 0, &h3set_len) != 0) { rc = 39; goto done; }
        uint8_t sf[32]; size_t sf_len = 0;
        if (stream_frame_write(sf, sizeof(sf), 2, h3set, h3set_len, 0, &sf_len) != 0) { rc = 40; goto done; }
        memcpy(fbuf + fp, sf, sf_len); fp += sf_len;

        /* 请求 HEADERS */
        uint8_t fields[512]; size_t fl_ = 0;
        int k;
        k = qpack_encode_field(fields + fl_, sizeof(fields) - fl_, ":method", "GET");       if (k < 0) { rc = 41; goto done; } fl_ += (size_t)k;
        k = qpack_encode_field(fields + fl_, sizeof(fields) - fl_, ":scheme", "https");      if (k < 0) { rc = 42; goto done; } fl_ += (size_t)k;
        k = qpack_encode_field(fields + fl_, sizeof(fields) - fl_, ":authority", "example.com"); if (k < 0) { rc = 43; goto done; } fl_ += (size_t)k;
        k = qpack_encode_field(fields + fl_, sizeof(fields) - fl_, ":path", "/index.html");  if (k < 0) { rc = 44; goto done; } fl_ += (size_t)k;
        k = qpack_encode_field(fields + fl_, sizeof(fields) - fl_, "user-agent", "NetLeaf-quictls/0.1"); if (k < 0) { rc = 45; goto done; } fl_ += (size_t)k;

        uint8_t h3hdr[600]; size_t h3hdr_len = 0;
        if (h3_frame_write(h3hdr, sizeof(h3hdr), 0x01, fields, fl_, &h3hdr_len) != 0) { rc = 46; goto done; }
        uint8_t sf2[700]; size_t sf2_len = 0;
        if (stream_frame_write(sf2, sizeof(sf2), 0, h3hdr, h3hdr_len, 1, &sf2_len) != 0) { rc = 47; goto done; }
        memcpy(fbuf + fp, sf2, sf2_len); fp += sf2_len;

        uint8_t pk5[1200]; size_t pk5_len = 0;
        if (pkt_build_short(0, 1, fbuf, fp, &c_write, NULL, 0, pk5, sizeof(pk5), &pk5_len) != 0) { rc = 48; goto done; }

        /* 服务端收 1-RTT，解析请求 */
        uint8_t b5[1200]; if (pk5_len > sizeof(b5)) { rc = 49; goto done; }
        memcpy(b5, pk5, pk5_len);
        const uint8_t* fr5 = NULL; size_t fl5 = 0;
        if (pkt_parse_short(b5, pk5_len, &s_read, 0, &fr5, &fl5, NULL, 0) != 0) { rc = 50; goto done; }

        char req_method[16] = {0}, req_path[128] = {0};
        int got_method = 0, got_path = 0;
        {
            size_t p = 0;
            while (p < fl5) {
                uint64_t sid = 0, off = 0; const uint8_t* sd = NULL; size_t sdl = 0; int fin = 0; size_t cons = 0;
                if (stream_frame_read(fr5 + p, fl5 - p, &sid, &off, &sd, &sdl, &fin, &cons) != 0) break;
                p += cons;
                if (sid == 2) continue;                 /* 控制流：忽略 SETTINGS */
                if (sid == 0) {                         /* 请求流 */
                    size_t hp = 0;
                    while (hp < sdl) {
                        uint64_t t = 0; const uint8_t* hd = NULL; size_t hdl = 0, hc = 0;
                        if (h3_frame_read(sd + hp, sdl - hp, &t, &hd, &hdl, &hc) != 0) break;
                        hp += hc;
                        if (t == 0x01) {                /* HEADERS */
                            h3_kv_t kvs[16]; int kc = 0;
                            if (qpack_decode(hd, hdl, kvs, 16, &kc) == 0) {
                                for (int i = 0; i < kc; i++) {
                                    if (strcmp(kvs[i].name, ":method") == 0) { snprintf(req_method, sizeof(req_method), "%s", kvs[i].value); got_method = 1; }
                                    if (strcmp(kvs[i].name, ":path") == 0) { snprintf(req_path, sizeof(req_path), "%s", kvs[i].value); got_path = 1; }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!got_method || !got_path || strcmp(req_method, "GET") != 0 || strcmp(req_path, "/index.html") != 0) { rc = 51; goto done; }

        /* 服务端：构造响应 HEADERS(:status 200) + DATA */
        const char* body = "Hello from NetLeaf HTTP/3!";
        uint8_t rfields[64]; size_t rf = 0;
        k = qpack_encode_field(rfields, sizeof(rfields), ":status", "200"); if (k < 0) { rc = 52; goto done; } rf += (size_t)k;
        uint8_t rhdr[80]; size_t rhdr_len = 0;
        if (h3_frame_write(rhdr, sizeof(rhdr), 0x01, rfields, rf, &rhdr_len) != 0) { rc = 53; goto done; }
        uint8_t rdata[64]; size_t rdata_len = 0;
        if (h3_frame_write(rdata, sizeof(rdata), 0x00, (const uint8_t*)body, strlen(body), &rdata_len) != 0) { rc = 54; goto done; }

        uint8_t rstream[256]; size_t rsp = 0;
        memcpy(rstream + rsp, rhdr, rhdr_len); rsp += rhdr_len;
        memcpy(rstream + rsp, rdata, rdata_len); rsp += rdata_len;
        uint8_t rsf[300]; size_t rsf_len = 0;
        if (stream_frame_write(rsf, sizeof(rsf), 0, rstream, rsp, 1, &rsf_len) != 0) { rc = 55; goto done; }

        uint8_t pk6[512]; size_t pk6_len = 0;
        if (pkt_build_short(0, 1, rsf, rsf_len, &s_write, NULL, 0, pk6, sizeof(pk6), &pk6_len) != 0) { rc = 56; goto done; }

        /* 客户端收 1-RTT 响应 */
        uint8_t b6[512]; if (pk6_len > sizeof(b6)) { rc = 57; goto done; }
        memcpy(b6, pk6, pk6_len);
        const uint8_t* fr6 = NULL; size_t fl6 = 0;
        if (pkt_parse_short(b6, pk6_len, &c_read, 0, &fr6, &fl6, NULL, 0) != 0) { rc = 58; goto done; }

        char resp_body[128] = {0};
        {
            uint64_t sid = 0, off = 0; const uint8_t* sd = NULL; size_t sdl = 0; int fin = 0; size_t cons = 0;
            if (stream_frame_read(fr6, fl6, &sid, &off, &sd, &sdl, &fin, &cons) != 0 || sid != 0) { rc = 59; goto done; }
            size_t hp = 0;
            while (hp < sdl) {
                uint64_t t = 0; const uint8_t* hd = NULL; size_t hdl = 0, hc = 0;
                if (h3_frame_read(sd + hp, sdl - hp, &t, &hd, &hdl, &hc) != 0) break;
                hp += hc;
                if (t == 0x00) { snprintf(resp_body, sizeof(resp_body), "%.*s", (int)hdl, (const char*)hd); }
            }
        }
        if (strcmp(resp_body, body) != 0) { rc = 60; goto done; }
    }

    /* 变长包号：短首部 pn=0x1234（pn_len=2）构建 → 解析（首部保护自动读取 pn_len） */
    {
        uint8_t fr[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
        uint8_t pk[64]; size_t pkl = 0;
        if (pkt_build_short(0x1234, 2, fr, 4, &ci_keys, NULL, 0, pk, sizeof(pk), &pkl) != 0) { rc = 70; goto done; }
        uint8_t buf[64]; memcpy(buf, pk, pkl);
        const uint8_t* ff = NULL; size_t fl = 0;
        if (pkt_parse_short(buf, pkl, &ci_keys, 0x1234, &ff, &fl, NULL, 0) != 0) { rc = 71; goto done; }
        if (fl != 4 || memcmp(ff, fr, 4) != 0) { rc = 72; goto done; }
    }

done:
    if (cli) nl_tls13_conn_free(cli);
    if (srv) nl_tls13_conn_free(srv);
    return rc;
}

/* ============================================================
 * C8-A：可运行 HTTP/3 服务端 + 客户端（UDP 回环）
 *
 * 复用本文件既有的静态编解码器（pkt_build_/pkt_parse_/crypto_frame_
 * stream_frame_/h3_frame_/qpack_ 系列）与 netleaf_quic.h 的 TLS 13 状态机公共 API，
 * 在 POSIX（socket/pthread）与 Windows（winsock2/线程 API）上通过薄可移植层实现同一套
 * 可端到端跑通的 happy-path：
 *   QUIC 握手（Initial CH/SH + Handshake 其余）+ 1-RTT 上一次 H3 GET 请求/响应。
 * 说明：假设传输有序无丢包（未实现 ACK/重传/拥塞/QPACK 动态表）。
 * ============================================================ */

/* 固定连接 ID（客户端 DCID / 客户端 SCID / 服务端 SCID）；Initial 密钥由 DCID 派生 */
#define NLH3_DCID_LEN 8
#define NLH3_MAX_PKT  4096
#define NLH3_MAX_CONNS 16   /* 服务端按对端地址分槽的连接表容量 */
#define NLH3_SLOT_IDLE_MS 15000  /* 连接槽空闲回收阈值（毫秒）：1-RTT 已建立连接的回收阈值 */
#define NLH3_HS_IDLE_MS  30000  /* 握手阶段槽位回收阈值：PTO 指数退避可达 ~30s，
                                  * 短于 PTO 最大间隔会导致握手中途 slot 被 reap，
                                  * 客户端 PTO 重传无法命中已有 slot → idle 超时 */

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  typedef SOCKET  nlh3_sock_t;
  typedef int     nlh3_socklen_t;
  typedef SSIZE_T nlh3_ssize_t;
  #define NLH3_INVALID_SOCK INVALID_SOCKET
  #define NLH3_SOCK_OK(s)   ((s) != INVALID_SOCKET)
  #define NLH3_CLOSE(s)     closesocket(s)
  #define NLH3_SLEEP_MS(ms) Sleep((DWORD)(ms))
  typedef HANDLE nlh3_thread_t;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <pthread.h>
  #include <time.h>
  #include <errno.h>
  typedef int       nlh3_sock_t;
  typedef socklen_t nlh3_socklen_t;
  typedef ssize_t   nlh3_ssize_t;
  #define NLH3_INVALID_SOCK (-1)
  #define NLH3_SOCK_OK(s)   ((s) >= 0)
  #define NLH3_CLOSE(s)     close(s)
  #define NLH3_SLEEP_MS(ms) do { struct timespec _ts; _ts.tv_sec=(ms)/1000; _ts.tv_nsec=((long)((ms)%1000))*1000000L; nanosleep(&_ts,NULL);} while(0)
  typedef pthread_t nlh3_thread_t;
#endif

static int nlh3_net_init(void) {
#ifdef _WIN32
    static int inited = 0;
    if (!inited) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return -1;
        inited = 1;
    }
#endif
    return 0;
}

static void nlh3_set_rcvtimeo(nlh3_sock_t fd, int ms) {
#ifdef _WIN32
    DWORD tmo = (DWORD)ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
#else
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/* 单调时钟（毫秒）：用于连接槽 idle 回收计时，避免系统时钟回拨影响超时判断 */
static uint64_t nlh3_now_ms(void) {
#ifdef _WIN32
    /* 64-bit QPC：高频单调，不受 SetTimerResolution 影响 */
    static LARGE_INTEGER freq = { 0 };
    static int inited = 0;
    if (!inited) { QueryPerformanceFrequency(&freq); inited = 1; }
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart * 1000ULL / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

#ifdef _WIN32
typedef DWORD (WINAPI *nlh3_thread_fn)(LPVOID);
static int nlh3_thread_create(nlh3_thread_t* t, nlh3_thread_fn fn, void* arg){ *t=CreateThread(NULL,0,fn,arg,0,NULL); return *t?0:-1; }
static void nlh3_thread_join(nlh3_thread_t t){ if(t){ WaitForSingleObject(t,INFINITE); CloseHandle(t);} }
#else
typedef void* (*nlh3_thread_fn)(void*);
static int nlh3_thread_create(nlh3_thread_t* t, nlh3_thread_fn fn, void* arg){ return pthread_create(t,NULL,fn,arg); }
static void nlh3_thread_join(nlh3_thread_t t){ pthread_join(t,NULL); }
#endif

static const uint8_t NLH3_DCID[NLH3_DCID_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8 };
static const uint8_t NLH3_CSID[NLH3_DCID_LEN] = { 9, 9, 9, 9, 9, 9, 9, 9 };
static const uint8_t NLH3_SSID[NLH3_DCID_LEN] = { 7, 7, 7, 7, 7, 7, 7, 7 };

/* 单条连接槽位：按对端地址（sockaddr_in）区分，保存该连接的握手/1-RTT 状态。
 * client_dcid 为该客户端在 Initial 中使用的 DCID（真实 QUIC 客户端自随机生成），
 * 服务端据此派生 Initial 密钥并在写侧 SH/Handshake 中回填该 DCID，保证与客户端一致。 */
typedef struct nlh3_conn_slot {
    int valid;
    struct sockaddr_in peer;
    uint64_t last_active_ms;   /* 最近一次活动（毫秒单调时钟），用于 idle 回收 */
    int idle_ms;              /* 该槽的空闲回收阈值（毫秒） */
    nl_tls13_conn_t* conn;    /* 该连接的 TLS 13 状态机 */
    nl_quic_keys_t ci_keys;   /* 客户端 Initial（服务端读 CH） */
    nl_quic_keys_t si_keys;   /* 服务端 Initial（服务端写 SH） */
    nl_quic_keys_t hs_read, hs_write;          /* 自身（服务端）侧 Handshake 读/写 */
    nl_quic_keys_t client_hs_read, client_hs_write;  /* 客户端侧 Handshake：读客户端 Finished / 写服务器 EE..Fin */
    nl_quic_keys_t rtt_read, rtt_write;        /* 自身（服务端）侧 1-RTT */
    nl_quic_keys_t client_rtt_read, client_rtt_write; /* 客户端侧 1-RTT */
    uint64_t rx_init_pn, rx_hs_pn, rx_rtt_pn;
    uint64_t tx_init_pn, tx_hs_pn, tx_rtt_pn;
    int acked;            /* 已对客户端发出初始段 ACK（防止 PTO 重传时重复回 ACK） */
    int hs_sent;         /* 首次 Initial 已回 SH+Handshake flight；后续 Initial 重传走缓存重放，
                          * 不再重跑 nl_tls13_server_handshake（避免每次生成新 ECDH keypair
                          * 导致 SH/密钥分叉，真实客户端因此 idle 超时） */
    uint8_t hs_flight[NLH3_MAX_PKT]; size_t hs_flight_len; /* 缓存的首次服务端 flight（SH + EE/Cert/CV/Fin） */
    uint8_t client_dcid[20];  /* 客户端 Initial 使用的 DCID（即 ODCID，用于派生 Initial 密钥） */
    size_t client_dcid_len;
    uint8_t client_scid[20];  /* 客户端 Initial 的 SCID = 客户端 host_cid；
                                * aioquic 客户端将响应包 DCID 与此值做匹配，
                                * 故服务端所有回复包必须用此值作 DCID。 */
    size_t client_scid_len;
    /* 首次 flight 使用的固定发送包号：PTO 重传必须用原始包号重放（RFC 9000 语义），
     * 不得再推进 tx_init_pn/tx_hs_pn，否则真实客户端的 ACK 范围与其重发包号错位、
     * 收不到覆盖 ACK 而持续 PTO → idle 超时。 */
    uint64_t flight_init_pn;   /* SH（Initial）flight 的发送包号，首次生成后固定 */
    uint64_t flight_hs_pn;     /* EE/Cert/CV/Fin（Handshake）flight 的发送包号，首次生成后固定 */
    int flight_gen_ok;         /* 已成功发送首次 flight（PN 已冻结）标志 */
    uint64_t client_ack_init;  /* 客户端 Initial 空间 ACK 覆盖到的最大包号 */
    uint64_t client_ack_hs;    /* 客户端 Handshake 空间 ACK 覆盖到的最大包号 */
    int established;          /* 握手完成、已取得 1-RTT 密钥标志 */
    int rtt_read_set;         /* rtt_read 密钥已派生（2a peek / 2b / fallback 置位），
                               * 用于 3 段放宽条件：early 1-RTT 短首包（H3 SETTINGS）
                               * 先于 client Finished 到达时也能解密。 */
    /* 1-RTT 去重与响应冻结：客户端 PTO 重传同包号 1-RTT 请求（SETTINGS/HEADERS），
     * 服务端必须重放相同包号的响应（RFC 9000：重传须用原始包号），不得推进 tx_rtt_pn，
     * 否则响应包号漂移，客户端 pkt_parse_short 用固定锚点解密错位 → 收不到 DATA（rc=29）。
     * 重复包号已处理过则不再回发新响应（仅重放原包号）。 */
    uint64_t rtt_last_actual_pn;  /* 已处理的最大 1-RTT 请求包号（去重锚点） */
    uint64_t rtt_tx_pn;          /* 1-RTT 响应冻结发送包号（首次固定为 0） */
    int rtt_replayed;           /* 已对首个 1-RTT 请求回发过响应（去重标志） */
    uint8_t rtt_resp_cache[NLH3_MAX_PKT]; size_t rtt_resp_cache_len;
    int rtt_resp_cached;        /* rtt_resp_cache 有效（可重放）标志 */
} nlh3_conn_slot_t;

struct nlh3_server {
    int port;
    int drop_first;          /* 自测：丢弃前 N 个收到的报文（模拟丢包，触发客户端重传） */
    char* cert_pem;
    char* key_pem;
    nlh3_handler_t handler;
    void* userdata;

    /* 连接表：按对端地址分槽（多连接） */
    nlh3_conn_slot_t slots[NLH3_MAX_CONNS];

    nlh3_sock_t fd;
    volatile int running;
    nlh3_thread_t thread;
    int thread_started;
};

static int nlh3_send_to(nlh3_server_t* s, const uint8_t* pkt, size_t len,
                        const struct sockaddr_in* peer) {
    nlh3_ssize_t r = sendto(s->fd, pkt, len, 0, (const struct sockaddr*)peer, sizeof(*peer));
    if (r != (nlh3_ssize_t)len) {
        fprintf(stderr, "[h3-srv] send_to FAIL rc=%d len=%zu peer=%d\n", (int)r, len, peer ? peer->sin_port : -1);
    } else {
        fprintf(stderr, "[h3-srv] send_to OK len=%zu\n", len);
    }
    fflush(stderr);
    return (r == (nlh3_ssize_t)len) ? 0 : -1;
}

/* PTO 重传：带超时（SO_RCVTIMEO）收包；若 recv 超时，则按顺序重发 p1/p2
 * （与首次发送时完全相同的字节、同包号、同密钥），最多重发 *retries_left 次后失败。
 * 返回 >0 表示收到的字节数；<=0 表示重传次数耗尽或套接字错误。 */
static nlh3_ssize_t nlh3_recv_pto(nlh3_sock_t fd, uint8_t* buf, size_t cap,
                             const uint8_t* p1, size_t l1,
                             const uint8_t* p2, size_t l2,
                             int* retries_left) {
    for (;;) {
        nlh3_ssize_t n = recv(fd, buf, cap, 0);
        if (n > 0) return n;
        if (n == 0) return -1;              /* UDP 下不应出现，视为失败 */
        if (*retries_left <= 0) return -1;  /* 重试耗尽 */
        (*retries_left)--;
        if (p1 && l1) { if (send(fd, p1, l1, 0) < 0) return -1; }
        if (p2 && l2) { if (send(fd, p2, l2, 0) < 0) return -1; }
    }
}

/* 按对端地址（IP+端口）在连接表中查找槽位 */
static nlh3_conn_slot_t* nlh3_slot_find(nlh3_server_t* s, const struct sockaddr_in* peer) {
    for (int i = 0; i < NLH3_MAX_CONNS; i++) {
        nlh3_conn_slot_t* sl = &s->slots[i];
        if (sl->valid &&
            sl->peer.sin_addr.s_addr == peer->sin_addr.s_addr &&
            sl->peer.sin_port == peer->sin_port) {
            return sl;
        }
    }
    return NULL;
}

/* 为新对端地址分配一个空闲槽位。若表已满则回收最早空闲超时（idle）的槽；
 * 仍无可用槽时回退淘汰“最久未活动”的槽（LRU），避免连接表耗尽后拒绝新连接。 */
static nlh3_conn_slot_t* nlh3_slot_alloc(nlh3_server_t* s, const struct sockaddr_in* peer) {
    uint64_t now = nlh3_now_ms();
    for (int i = 0; i < NLH3_MAX_CONNS; i++) {
        nlh3_conn_slot_t* sl = &s->slots[i];
        if (!sl->valid) {
            memset(sl, 0, sizeof(*sl));
            sl->valid = 1;
            sl->peer = *peer;
            sl->last_active_ms = now;
            sl->idle_ms = NLH3_SLOT_IDLE_MS;
            return sl;
        }
    }
    /* 表满：优先回收已 idle 超时的槽（握手阶段用更长阈值） */
    for (int i = 0; i < NLH3_MAX_CONNS; i++) {
        nlh3_conn_slot_t* sl = &s->slots[i];
        if (!sl->valid) continue;
        uint64_t threshold = (sl->conn && !sl->established)
            ? (uint64_t)NLH3_HS_IDLE_MS
            : (uint64_t)sl->idle_ms;
        if ((now - sl->last_active_ms) >= threshold) {
            nl_tls13_conn_free(sl->conn);
            memset(sl, 0, sizeof(*sl));
            sl->valid = 1;
            sl->peer = *peer;
            sl->last_active_ms = now;
            sl->idle_ms = NLH3_SLOT_IDLE_MS;
            return sl;
        }
    }
    /* 无 idle 超时槽：淘汰最久未活动的槽（LRU） */
    int lru = 0; uint64_t oldest = s->slots[0].last_active_ms;
    for (int i = 1; i < NLH3_MAX_CONNS; i++) {
        if (s->slots[i].last_active_ms < oldest) { oldest = s->slots[i].last_active_ms; lru = i; }
    }
    {
        nlh3_conn_slot_t* sl = &s->slots[lru];
        nl_tls13_conn_free(sl->conn);
        memset(sl, 0, sizeof(*sl));
        sl->valid = 1;
        sl->peer = *peer;
        sl->last_active_ms = now;
        sl->idle_ms = NLH3_SLOT_IDLE_MS;
        return sl;
    }
}

/* 周期性 idle 回收：释放超过 idle_ms 未活动的连接槽（含 conn 状态机），
 * 在每包处理入口调用，无需额外后台定时器。 */
static void nlh3_slot_reap_idle(nlh3_server_t* s) {
    uint64_t now = nlh3_now_ms();
    for (int i = 0; i < NLH3_MAX_CONNS; i++) {
        nlh3_conn_slot_t* sl = &s->slots[i];
        if (!sl->valid) continue;
        /* 握手阶段（有 conn 但未 established）使用更长的 idle 阈值，
         * 避免 PTO 指数退避间隔（~5s→30s）导致 slot 被 reap，
         * 客户端 PTO 重传因此无法命中 slot → idle 超时。 */
        uint64_t threshold = (sl->conn && !sl->established)
            ? (uint64_t)NLH3_HS_IDLE_MS
            : (uint64_t)sl->idle_ms;
        if ((now - sl->last_active_ms) >= threshold) {
            nl_tls13_conn_free(sl->conn);
            memset(sl, 0, sizeof(*sl));
        }
    }
}

/* 读取一条 QUIC 长首部数据包的 DCID（原始、未解保护的首部偏移是明文可读的：
 * 首字节 + version(4) + DCID_len + DCID + SCID_len + SCID）。用于在收到真实客户端的
 * Initial 前，先取出其 DCID 以派生 Initial 密钥。返回 0 成功；-1 非长首部/越界。 */
static int nlh3_read_dcid(const uint8_t* dg, size_t dlen, uint8_t* out, size_t cap, size_t* out_len) {
    if (dlen < 11) return -1;
    if (!(dg[0] & 0x80)) return -1;                       /* 必须长首部 */
    /* 兼容 QUIC v1 / v2：version 4 字节在偏移 1..4 处均为明文 */
    size_t p = 1 + 4;                                       /* 跳过 b0 + version */
    /* 长首部布局（RFC 9000 §17.2）：b0 + version(4) + DCID_len + DCID + SCID_len + SCID + ...
     * pkt_build_long 先写 DCID 字段再写 SCID 字段，解析顺序必须一致。 */
    if (p + 1 > dlen) return -1;
    size_t dcid_len = dg[p]; p += 1 + dcid_len;
    if (p + 1 > dlen) return -1;
    /* SCID 字段（仅为跳过，不参与 DCID 提取） */
    size_t scid_len = dg[p]; /* p += 1 + scid_len; */
    (void)scid_len;
    if (dcid_len > cap) return -1;
    memcpy(out, dg + p - dcid_len, dcid_len);
    *out_len = dcid_len;
    return 0;
}

/* 从入站 QUIC 长首部数据包中提取 SCID（Source Connection ID）。
 * 布局：b0 + version(4) + DCID_len + DCID + SCID_len + SCID。
 * 用于提取客户端 Initial 包的 SCID（= 客户端的 host_cid），
 * 服务端回复包必须以此 SCID 作为 DCID，否则 aioquic 客户端
 * 将响应包 DCID 与自身 host_cid 比对失败 → 丢弃所有回复包。
 * 返回 0 成功；-1 非长首部/越界；scid_len=0 时 out_scid 置空、*out_scid_len=0 仍成功。 */
static int nlh3_read_scid(const uint8_t* dg, size_t dlen,
                          uint8_t* out_scid, size_t cap, size_t* out_scid_len) {
    if (dlen < 6) return -1;
    if (!(dg[0] & 0x80)) return -1;                       /* 必须长首部 */
    size_t p = 1 + 4;                                       /* 跳过 b0 + version */
    if (p + 1 > dlen) return -1;
    size_t dcid_len = dg[p]; p += 1 + dcid_len;
    if (p >= dlen) return -1;
    size_t scid_len = dg[p]; p += 1;
    if (scid_len > 0) {
        if (p + scid_len > dlen) return -1;
        if (scid_len > cap) return -1;
        memcpy(out_scid, dg + p, scid_len);
    }
    *out_scid_len = scid_len;
    return 0;
}

/* 处理一条 UDP 报文：按对端地址定位槽位（不存在且为 Initial(CH) 时建槽），
 * 再按 Initial / Handshake / 1-RTT 顺序尝试解析。
 * 返回 1 表示已按某加密级别处理，0 表示不属于任何连接，-1 表示处理中出错。 */
static int nlh3_server_handle(nlh3_server_t* s, const uint8_t* dg, size_t dlen,
                              const struct sockaddr_in* peer) {
    uint8_t buf[NLH3_MAX_PKT];
    nlh3_conn_slot_t* sl;

    if (dlen > sizeof(buf)) return -1;
#ifdef NLH3_DBG
    {
        static int entry_cnt = 0;
        if (entry_cnt < 60) {
            entry_cnt++;
            uint64_t ms = nlh3_now_ms();
            fprintf(stderr, "[h3-srv] handle#%d t=%llu recvfrom b0=0x%02x dlen=%zu peer=%d.%d port=%d\n",
                    entry_cnt, (unsigned long long)ms, dg[0], dlen,
                    (int)((peer->sin_addr.s_addr >> 0) & 0xff),
                    (int)((peer->sin_addr.s_addr >> 8) & 0xff),
                    ntohs(peer->sin_port));
            fflush(stderr);
        }
    }
    /* 临时诊断：dump 客户端原始 datagram（完整字节 + 前 40B hex），供离线 GCM+HP 核验。 */
    {
        static int raw_dump_cnt = 0;
        FILE* rdf = fopen("f:/C-C++/NetLeaf/build_win/bin/dump/client_raw_datagrams.txt",
                          raw_dump_cnt == 0 ? "w" : "a");
        if (rdf) {
            fprintf(rdf, "[raw #%d] b0=0x%02x dlen=%zu\n", raw_dump_cnt, dg[0], dlen);
            for (size_t i = 0; i < dlen && i < 40; i++) fprintf(rdf, "%02x ", dg[i]);
            fprintf(rdf, "\n");
            fclose(rdf); raw_dump_cnt++;
        }
        /* 完整字节 dump（前 32 个 datagram） */
        if (raw_dump_cnt < 32) {
            char pbin[128];
            snprintf(pbin, sizeof(pbin), "f:/C-C++/NetLeaf/build_win/bin/dump/client_raw_%02d.bin", raw_dump_cnt);
            FILE* rbf = fopen(pbin, "wb");
            if (rbf) { fwrite(dg, 1, dlen, rbf); fclose(rbf); }
        }
    }
#endif

    /* 周期性 idle 回收：必须在 find 之前执行。
     * 若在 find 之后回收，可能 memset 掉刚命中的 slot（其 last_active_ms
     * 尚未刷新），导致 sl 仍指向被清零的 slot（valid=0, conn=NULL,
     * 全零 ci_keys），后续用零值密钥做 HP unprotect 必然失败（rc=-1），
     * 客户端 PTO 重传永远无法被正确解析 → idle 超时。 */
    nlh3_slot_reap_idle(s);

    sl = nlh3_slot_find(s, peer);
    if (sl) sl->last_active_ms = nlh3_now_ms();   /* 本包属于已有连接，刷新其活动时刻 */

    /* 1) 客户端 Initial：CRYPTO 内含 ClientHello（新连接在此建槽） */
    memcpy(buf, dg, dlen);
    {
        uint8_t dcid[20];
        size_t dcid_len = 0;
        int dcid_ok = nlh3_read_dcid(buf, dlen, dcid, sizeof(dcid), &dcid_len) == 0;

        nl_quic_keys_t ci, si;
        int have_keys = 0;
        if (sl) {
            ci = sl->ci_keys; si = sl->si_keys;
            have_keys = 1;
        } else if (dcid_ok && nl_quic_initial_keys(dcid, dcid_len, &ci, &si) == 0) {
            have_keys = 1;
        } else {
            /* 无法解析 DCID 或无已有连接：回退到本地自测用固定 DCID，
             * 保证自测路径仍可通过；真实客户端路径则走 dcid_ok 分支。 */
            if (!dcid_ok && nl_quic_initial_keys(NLH3_DCID, NLH3_DCID_LEN, &ci, &si) != 0)
                return -1;
            have_keys = 1;
            dcid_len = NLH3_DCID_LEN;
            memcpy(dcid, NLH3_DCID, dcid_len);
        }

        const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
        /* 按包首字节明文 type 分路（RFC 9000 §17.2：长首 b0 bits5-2 = type，
         * 不受首保护掩码影响（掩码仅覆盖低 4 位））：
         * - type=0 (Initial)：始终用 Initial 密钥 ci 解（含首包与任何 Initial 重传，
         *   PN 按该级别实际值）。即使 sl->conn 已建，客户端仍可能在 1-RTT 之前重传
         *   Initial 级的 CRYPTO 分段，必须用 ci 而非 hs_read。
         * - type=2 (Handshake)：用 HS 读密钥 sl->hs_read 解（client Finished / 握手密文）。
         * - 1-RTT 短首部（b0=0x40，bit7=0）：直接落入下方 1-RTT 路径（pkt_parse_short）。
         * 旧逻辑按 sl->conn 状态切密钥，会把 Initial 重传误交给 HS 密钥 → AEAD 失败。 */
        uint8_t probe_b0 = buf[0];
        /* 长首 b0：type 在 bits5-4 (RFC 9000 §17.2)，与 aioquic (first_byte & 0x30) >> 4 对齐 */
        uint8_t probe_type = (uint8_t)((probe_b0 >> 4) & 0x03);
        /* 临时诊断：验证 C 侧收到的 b0 字节值与 probe_type 计算是否一致 */
        if (probe_b0 & 0x80) {
            fprintf(stderr, "[probe-diag] buf[0]=0x%02x computed_type=%d bits54=%02x bits32=%02x bits10=%02x\n",
                    probe_b0, probe_type,
                    (probe_b0 & 0x30) >> 4, (probe_b0 >> 2) & 0x03, probe_b0 & 0x03);
            fflush(stderr);
        }
        /* 一次性 dump 客户端 Initial 原始包（用于离线校验 key 派生是否与 aioquic 一致） */
        static int dumped_init = 0;
        if (!dumped_init && probe_type == 0) {
            dumped_init = 1;
#ifdef NLH3_DBG
            {
                FILE* df = fopen("dump/first_initial.bin", "wb");
                if (df) { fwrite(buf, 1, dlen, df); fclose(df); }
                /* dump client Initial DCID so offline check can compare with server SH DCID */
                FILE* fdc = fopen("dump/cli_dcid.txt", "w");
                if (fdc) {
                    for (size_t i = 0; i < dcid_len; i++) fprintf(fdc, "%02x", dcid[i]);
                    fprintf(fdc, "\n"); fclose(fdc);
                }
                fprintf(stderr, "[h3-srv] dumped first Initial b0=0x%02x len=%zu dcid_len=%zu\n",
                        probe_b0, dlen, dcid_len);
                for (size_t i = 0; i < dlen && i < 40; i++) fprintf(stderr, "%02x ", buf[i]);
                fprintf(stderr, "\n"); fflush(stderr);
            }
#endif
        }
        int pprc = 0;
        uint64_t actual_rx_pn = 0;
        /* 判断是否为 1-RTT 短首部（b0=0x40，bit7=0，probe_type=1）：
         * aioquic 对 1-RTT 包用短首部（packet_builder.py L330-339），不发 1-RTT 长首（type=3）。
         * 当 sl->conn 存在且 rtt_read 已派生（rtt_read_set）且 probe_type==1 且 b0 的 bit7=0
         * （即 b0 的高位 nibble 是 0x4，非长首的 0xC/0xE），跳过长首解析，
         * 直接设 pprc=-1 让控制流落入下方 3 段（pkt_parse_short）。
         * 放宽条件：不要求 sl->established，使 early 1-RTT 包（H3 SETTINGS，先于
         * client Finished 到达）也能被解密。 */
        int is_short_1rtt = (probe_type == 1 && !(probe_b0 & 0x80) && sl && sl->conn &&
                             (sl->established || sl->rtt_read_set));
        if (is_short_1rtt) {
            pprc = -1;  /* 标记为非长首，让 2a/2b 条件不触发 */
            ty = 0;
        } else if (sl && sl->conn && probe_type == 2) {
            pprc = pkt_parse_long(buf, dlen, &sl->hs_read, sl->rx_hs_pn, &fr, &fl, &ty, &actual_rx_pn);
        } else if (sl && sl->conn && probe_type == 3 && (sl->established || sl->rtt_read_set)) {
            /* 1-RTT long header (type=3)：用 rtt_read 密钥解析。
             * aioquic 在收到 server Finished 后发 1-RTT CRYPTO（client Finished）
             * 可能仍用 long header（type=3），需 rtt_read 而非 hs_read/Initial。 */
            pprc = pkt_parse_long(buf, dlen, &sl->rtt_read, sl->rx_rtt_pn, &fr, &fl, &ty, &actual_rx_pn);
        } else {
            /* Initial（type=0）：首包 / 已建槽的 Initial 重传 都用 ci。
             * actual_rx_pn 记录包内自解析的实际包号，供 Initial ACK largest_acked 使用。 */
            pprc = pkt_parse_long(buf, dlen, &ci, sl ? sl->rx_init_pn : 0, &fr, &fl, &ty, &actual_rx_pn);
        }
#ifdef NLH3_DBG
        {
            int sidx = -1;
            for (int i = 0; i < NLH3_MAX_CONNS; i++)
                if (&s->slots[i] == sl) { sidx = i; break; }
            fprintf(stderr, "[h3-srv] b0=0x%02x probe_type=%d pprc=%d ty=%d slot=%d "
                    "conn=%d established=%d rx_init_pn=%llu rx_hs_pn=%llu\n",
                    probe_b0, probe_type, pprc, ty, sidx,
                    sl ? (int)(sl->conn != NULL) : 0, sl ? sl->established : 0,
                    (unsigned long long)(sl ? sl->rx_init_pn : 0),
                    (unsigned long long)(sl ? sl->rx_hs_pn : 0));
            fflush(stderr);
        }
#endif

        /* 2a) 客户端 Initial（ClientHello）：type=0，首次建槽 + 回 SH/Handshake。
         * aioquic 发 Initial 包时帧序列可能为 PADDING/NEW_CONNECTION_ID + CRYPTO + PADDING，
         * 需按帧逐个跳过非 CRYPTO 帧、收集所有 CRYPTO 帧数据。 */
        if (pprc == 0 && ty == 0) {
            /* 遍历 datagram 内所有帧，提取全部 CRYPTO 帧数据 */
            uint8_t cd[NLH3_MAX_PKT]; size_t cdl_total = 0;
            int cfr_ok = 0;
#ifdef NLH3_DBG
            fprintf(stderr, "[h3-srv] 2a entered: fr_len=%zu fr[0]=0x%02x fr[1]=0x%02x\n",
                    fl, fr[0], fr[1]);
            fflush(stderr);
#endif
            {
                size_t fp = 0;
                while (fp < fl) {
                    uint64_t ftype = 0;
                    size_t tn = nl_quic_varint_read(fr + fp, fl - fp, &ftype);
                    if (!tn) {
#ifdef NLH3_DBG
                        fprintf(stderr, "[h3-srv] 2a scan: fp=%zu varint_read failed\n", fp);
                        fflush(stderr);
#endif
                        break;
                    }
#ifdef NLH3_DBG
                    fprintf(stderr, "[h3-srv] 2a scan: fp=%zu ftype=0x%llx tn=%zu\n",
                            fp, (unsigned long long)ftype, tn);
                    fflush(stderr);
#endif
                    fp += tn;
                    if (ftype == 0x06) {  /* CRYPTO 帧 */
                        uint64_t cfoff = 0; const uint8_t* cfdata = NULL;
                        size_t cfdl = 0, cfcons = 0;
                        int cfr = crypto_frame_read(fr + fp - tn, fl - (fp - tn),
                                                     &cfoff, &cfdata, &cfdl, &cfcons);
                        if (cfr == -2) break;   /* 截断，等后续 datagram */
                        if (cfr == 0) {
                            if (cdl_total + cfdl > sizeof(cd)) break;
                            memcpy(cd + cdl_total, cfdata, cfdl);
                            cdl_total += cfdl;
                            fp += cfcons;
                            cfr_ok = 1;
                            continue;
                        }
                        break;
                    } else if (ftype == 0x01) {  /* PADDING：消费至 datagram 尾 */
                        break;
                    } else if (ftype == 0x00 || ftype == 0x18) { /* NEW_CONNECTION_ID */
                        /* ncn(2B) + new_cid_len(1B) + new_cid + retire_ad(1B varint) */
                        if (fp + 2 > fl) break;
                        fp += 2;                  /* sequence number (2B) */
                        if (fp >= fl) break;
                        uint8_t ncil = fr[fp++];
                        if (fp + ncil > fl) break;
                        fp += ncil;               /* new connection id */
                        uint64_t ra = 0; size_t rn = nl_quic_varint_read(fr + fp, fl - fp, &ra);
                        if (!rn) break;
                        fp += rn;
                    } else { /* 未知帧：无法安全跳过 */
                        break;
                    }
                }
            }
            if (cfr_ok && cdl_total > 0) {
                const uint8_t* cd_ptr = cd;
                uint8_t flight[NLH3_MAX_PKT]; size_t flight_len = 0;
                uint8_t cf[NLH3_MAX_PKT]; size_t cfl = 0;
                size_t sh_len = 0;
                int fresh = 0;
#ifdef NLH3_DBG
                fprintf(stderr, "[h3-srv] 2a CH found: cdl_total=%zu\n", cdl_total);
                fflush(stderr);
#endif
                if (!sl) {                        /* 新连接：建槽并保存 Initial 密钥 */
                    sl = nlh3_slot_alloc(s, peer);
                    if (!sl) return -1;
                    sl->ci_keys = ci; sl->si_keys = si;
                    sl->client_dcid_len = dcid_len;
                    memcpy(sl->client_dcid, dcid, dcid_len);
                    /* 提取客户端 Initial 包的 SCID（= 客户端 host_cid），
                     * 服务端所有回复包必须用此 SCID 作为 DCID，
                     * 否则 aioquic 客户端比对自身 host_cid 失败 → 丢包。 */
                    uint8_t scid_buf[20]; size_t scid_len = 0;
                    int scid_rc = nlh3_read_scid(dg, dlen, scid_buf, sizeof(scid_buf), &scid_len);
                    if (scid_rc == 0) {
                        sl->client_scid_len = scid_len;
                        if (scid_len > 0) memcpy(sl->client_scid, scid_buf, scid_len);
                    } else {
                        sl->client_scid_len = 0;
                    }
                    /* 调试：打印 SCID 提取结果 + datagram 前 30B，定位 CID 不匹配根因 */
                    fprintf(stderr, "[h3-srv] SCID extract: rc=%d scid_len=%zu scid=",
                            scid_rc, scid_len);
                    for (size_t i = 0; i < scid_len; i++) fprintf(stderr, "%02x", scid_buf[i]);
                    fprintf(stderr, " (dcid_len=%zu dcid=", dcid_len);
                    for (size_t i = 0; i < dcid_len; i++) fprintf(stderr, "%02x", dcid[i]);
                    fprintf(stderr, ")\n");
                    for (size_t i = 0; i < dlen && i < 30; i++) fprintf(stderr, " %02x", dg[i]);
                    fprintf(stderr, "\n"); fflush(stderr);
                } else if (!dcid_ok) {
                    dcid_len = sl->client_dcid_len;
                    memcpy(dcid, sl->client_dcid, dcid_len);
                }
                /* 幂等守卫：首次 Initial 已回过 SH+Handshake flight（sl->hs_sent）后，
                 * 后续 Initial 重传（PTO / 客户端重发）一律重放缓存 flight，
                 * 不再重跑 nl_tls13_server_handshake。否则每次重传都会生成新
                 * ECDH keypair，SH 字节随之变化、密钥派生分叉，真实客户端
                 * （aioquic）因看到的 ServerHello 不一致而 idle 超时。 */
                if (sl->hs_sent && sl->hs_flight_len > 0) {
                    memcpy(flight, sl->hs_flight, sl->hs_flight_len);
                    flight_len = sl->hs_flight_len;
                    if (nl_tls13_parse_handshake(flight, flight_len, NULL, NULL, NULL, &sh_len) != 0)
                        return -1;
                    /* 重传：保持首次密钥 sl->hs_read / sl->hs_write 不变，仅重放；
                     * 且发送包号沿用首次冻结的 flight_init_pn / flight_hs_pn（见下方 2b/2c）。 */
                } else {
                    fresh = 1;
                    if (!sl->conn) {
                        sl->conn = nl_tls13_conn_new(1);
                        if (!sl->conn) return -1;
                        if (nl_tls13_conn_set_cert(sl->conn, s->cert_pem, s->key_pem) != 0) return -1;
                        /* 注入客户端 ODCID（Initial 包中的 DCID），用于
                         * 0x00 initial_source_connection_id 传输参数 */
                        nl_tls13_conn_set_odcid(sl->conn, sl->client_dcid, sl->client_dcid_len);
                    }
                    {
                        int hrc = nl_tls13_server_handshake(sl->conn, cd_ptr, cdl_total, flight, sizeof(flight), &flight_len);
#ifdef NLH3_DBG
                        fprintf(stderr, "[h3-srv] 2a server_handshake rc=%d flight_len=%zu\n", hrc, flight_len);
                        fflush(stderr);
#endif
                        if (hrc != 0) return -1;
                    }
                    /* 服务端 Handshake 密钥：get_handshake_keys 按 is_server=1 映射
                     * read=客户端 c_hs 派生（sl->hs_read，解析 client Finished）、
                     * write=服务端 s_hs 派生（sl->hs_write，写服务器 EE/Cert/CV/Fin 包）。 */
                    {
                        int krc = nl_tls13_conn_get_handshake_keys(sl->conn, &sl->hs_read, &sl->hs_write);
#ifdef NLH3_DBG
                        fprintf(stderr, "[h3-srv] 2a get_handshake_keys rc=%d\n", krc);
                        fflush(stderr);
#endif
                        if (krc != 0) return -1;
                    }
                    /* 1-RTT 密钥：server_handshake 内部已派生 read1/write1（L968-974），
                     * 但此刻 conn 级 established 未置位（仅 server_finish 才置），
                     * 故 get_1rtt_keys（要求 established）会返回 -1。改用 peek 版本
                     * （不检查 established，密钥已在 server_handshake 内派生），使 client
                     * 的 1-RTT 长首包（H3 SETTINGS 流 2，先于 HS Finished 到达）可用
                     * rtt_read 解密；否则主分路 type=3 落入 else 用 Initial 密钥 → 解不开。
                     * 失败不致命：type=3 包仍会在后续 datagram 重试，最终由 2b 兜底。 */
                    {
                        int r1rc = nl_tls13_conn_peek_1rtt_keys(sl->conn, &sl->rtt_read, &sl->rtt_write);
#ifdef NLH3_DBG
                        fprintf(stderr, "[h3-srv] 2a peek_1rtt_keys rc=%d\n", r1rc);
                        fflush(stderr);
#endif
                        if (r1rc != 0) {
                            /* 非致命：1-RTT 密钥未就绪，后续 datagram 重试；
                             * 当前包继续发 SH/HS flight，不中断 2a 主流程。 */
                        } else {
                            sl->rx_rtt_pn = 0;
                            sl->rtt_read_set = 1;
                        }
                    }
                    if (nl_tls13_parse_handshake(flight, flight_len, NULL, NULL, NULL, &sh_len) != 0)
                        return -1;
                    /* 缓存首次 flight（SH + EE/Cert/CV/Fin），供后续重传重放 */
                    if (flight_len > 0 && flight_len <= sizeof(sl->hs_flight)) {
                        memcpy(sl->hs_flight, flight, flight_len);
                        sl->hs_flight_len = flight_len;
                    }
#ifdef NLH3_DBG
                    {
                        size_t d = sh_len < 200 ? sh_len : 200;
                        fprintf(stderr, "[h3-srv] TX SH raw sh_len=%zu bytes=", d);
                        for (size_t i = 0; i < d; i++) fprintf(stderr, "%02x ", flight[i]);
                        fprintf(stderr, "\n");
                    }
                    fflush(stderr);
#endif
                }
                /* 冻结首次 flight 发送包号：PTO 重传必须用原始包号重放（RFC 9000 语义），
                 * 否则客户端 ACK 范围与其重传包号错位，收不到覆盖 ACK 而持续 PTO → idle 超时。 */
                if (!sl->flight_gen_ok) {
                    sl->flight_init_pn = sl->tx_init_pn;
                    sl->flight_hs_pn = sl->tx_hs_pn;
                    sl->flight_gen_ok = 1;
                }

                /* 2b/2c 注释见下方 1056。 */
                /* ServerHello → Initial 包：DCID = 客户端 DCID，SCID 长度 0。
                 * 发送包号 = 冻结的 flight_init_pn（首次生成后不再推进）。
                 * RFC 9001 §4.1：QUIC carries TLS handshake data in CRYPTO frames，
                 * 所有 TLS 握手消息（含 ServerHello）都必须通过 CRYPTO 帧承载，
                 * 故 SH 明文须先包成 CRYPTO 帧再填入 Initial 包 payload。
                 * （旧实现误把 SH 明文直接当 payload，客户端 crypto_frame_read
                 *  期待 CRYPTO 帧解析失败 → 阶段 A 永远收不到 SH → rc=12，
                 *  aioquic 真实客户端同样无法解析。）
                 */
                /* k13sh: RFC 9000 §18.2 —— 收到含 Initial 包的 datagram 时，接收端
                 * 必须丢弃同 datagram 内的 Handshake 包并依赖重传。故服务端 C2 flight
                 * 的 SH(Initial 包) 与 EE/Cert/CV/Fin(Handshake 包) 必须分两个独立 datagram
                 * 发送，且 Handspace datagram 不得混入任何 Initial 包。
                 * datagram A：仅 SH(Initial)；datagram B：仅 EE/Cert/CV/Fin(Handshake)。 */
                /* RFC 9000 §14.3：Initial 包在 datagram 内必须被 PADDING 填充到至少 1200B，
                 * 否则接收方（aioquic）无法完成 loss detection 与 flight sizing，
                 * 会持续 PTO 重传 Initial 而不推进到 Handshake 空间。
                 * 本实现把 datagram A 固定填充到 1200B。 */
                {
                    uint8_t dgram[NLH3_MAX_PKT]; size_t dl = 0;
                    size_t sh_cf_len = 0;
                    if (crypto_frame_write(cf, sizeof(cf), 0, flight, sh_len, &sh_cf_len) != 0)
                        return -1;
                    /* 服务端→客户端 DCID = 客户端 SCID（客户端 Initial 里写的 SCID，
                     * 即客户端 host_cid）。aioquic 客户端 _host_cids 只含自身生成的
                     * SCID，ODCID 是独立随机值不在 _host_cids 中，故响应 DCID 必须
                     * 用客户端 SCID 才能通过 CID 匹配。
                     * 自测路径无 sl，dcid 即固定 NLH3_DCID，行为不变。 */
                    const uint8_t* r_dcid = dcid;
                    size_t r_dcid_len = dcid_len;
                    if (sl && sl->client_scid_len > 0) {
                        r_dcid = sl->client_scid;
                        r_dcid_len = sl->client_scid_len;
                    }
                    /* 头部精确长度（pkt_build_long 写入顺序，scid_len=0）：
                     * b0(1) + version(4) + dcid_len(1) + dcid(r_dcid_len) + scid_len(1) +
                     * token(varint, 0→1B) + length(varint, 值 < 16384→2B) + PN(2)
                     * 无 padding 的总 datagram 长 = (r_dcid_len+12) + sh_cf_len + NL_QUIC_TAG_LEN
                     * padding = 1200 - 无 padding 总长 */
                    size_t total_no_pad = r_dcid_len + 12 + sh_cf_len + NL_QUIC_TAG_LEN;
                    size_t pad = (total_no_pad < 1200) ? (1200 - total_no_pad) : 0;
                    if (pkt_build_long(0, r_dcid, r_dcid_len, NULL, 0,
                                       sl->flight_init_pn, 2, cf, sh_cf_len,
                                       &sl->si_keys, dgram, sizeof(dgram), &dl,
                                       NULL, NULL, NULL, pad) != 0)
                        return -1;
                    /* 调试：打印响应包实际 DCID */
                    fprintf(stderr, "[h3-srv] datagram A: DCID=");
                    for (size_t i = 0; i < r_dcid_len; i++) fprintf(stderr, "%02x", r_dcid[i]);
                    fprintf(stderr, " (len=%zu) scid_len=%zu\n", r_dcid_len, sl ? sl->client_scid_len : 0);
                    fflush(stderr);
#ifdef NLH3_DBG
                    if (dl < 1200)
                        fprintf(stderr, "[h3-srv] datagram A(SH init pn=%llu) pad=%zu total=%zu WARN < 1200B\n",
                                (unsigned long long)sl->flight_init_pn, pad, dl);
                    else
                        fprintf(stderr, "[h3-srv] datagram A(SH init pn=%llu) pad=%zu total=%zu OK (padded to 1200B)\n",
                                (unsigned long long)sl->flight_init_pn, pad, dl);
                    fflush(stderr);
                    NL_MKDIR("dump");
                    /* dump 实际发出的 datagram A（密封后的 Initial 包全字节）供 Python 独立重算比对 */
                    {
                        FILE* fda = fopen("dump/srv_c2_datagramA.bin", "wb");
                        if (fda) { fwrite(dgram, 1, dl, fda); fclose(fda); }
                    }
                    /* dump SH CRYPTO 帧（datagram A 里的 Initial 包明文）供离线 oracle 比对 */
                    {
                        FILE* fsh = fopen("dump/srv_sh_keydump.txt", "w");
                        if (fsh) {
                            fprintf(fsh, "dcid=");
                            for (size_t i = 0; i < dcid_len; i++) fprintf(fsh, "%02x", dcid[i]);
                            fprintf(fsh, "\npn=%llu\n", (unsigned long long)sl->flight_init_pn);
                            fprintf(fsh, "si_key=");
                            for (int i = 0; i < 16; i++) fprintf(fsh, "%02x", sl->si_keys.key[i]);
                            fprintf(fsh, "\nsi_iv=");
                            for (int i = 0; i < NL_QUIC_IV_LEN; i++) fprintf(fsh, "%02x", sl->si_keys.iv[i]);
                            fprintf(fsh, "\nsi_hp=");
                            for (int i = 0; i < 16; i++) fprintf(fsh, "%02x", sl->si_keys.hp[i]);
                            fprintf(fsh, "\npt_hex=");
                            for (size_t i = 0; i < sh_cf_len; i++) fprintf(fsh, "%02x", cf[i]);
                            fprintf(fsh, "\n");
                            fclose(fsh);
                        }
                    }
#endif
                    nlh3_send_to(s, dgram, dl, peer);
                }
                sl->tx_init_pn = sl->flight_init_pn + 1;

                /* EE/Cert/CV/Fin → Handshake 包：发送包号 = 冻结的 flight_hs_pn。
                 * 独立 datagram B 发送（见上方 k13sh 注释），与 SH 包严格分离。 */
                {
                    const uint8_t* rest = flight + sh_len;
                    size_t rest_len = flight_len - sh_len;
                    size_t hs_pn_off = 0;
                    uint8_t hs_aad[64] = {0}; size_t hs_aad_len = sizeof(hs_aad);
                    uint8_t hbuf[NLH3_MAX_PKT]; size_t hl = 0;
                    if (crypto_frame_write(cf, sizeof(cf), 0, rest, rest_len, &cfl) != 0)
                        return -1;
                    /* 与 datagram A 一致：DCID = 客户端 SCID（aioquic _host_cids 匹配） */
                    {
                        const uint8_t* h_dcid = dcid;
                        size_t h_dcid_len = dcid_len;
                        if (sl && sl->client_scid_len > 0) {
                            h_dcid = sl->client_scid;
                            h_dcid_len = sl->client_scid_len;
                        }
                        if (pkt_build_long(2, h_dcid, h_dcid_len, NULL, 0,
                                           sl->flight_hs_pn, 2, cf, cfl, &sl->hs_write,
                                           hbuf, sizeof(hbuf), &hl, &hs_pn_off,
                                           hs_aad, &hs_aad_len, 0) != 0)
                            return -1;
                    }
                    nlh3_send_to(s, hbuf, hl, peer);
#ifdef NLH3_DBG
                {
                    FILE* dfh = fopen("dump/srv_c2_hs_datagram.bin", "wb");
                    if (dfh) { fwrite(hbuf, 1, hl, dfh); fclose(dfh); }
                    fprintf(stderr, "[h3-srv] TX C2 datagram A(SH init pn=%llu) + datagram B(HS pn=%llu) hs_len=%zu raw=",
                            (unsigned long long)sl->flight_init_pn,
                            (unsigned long long)sl->flight_hs_pn, hl);
                    for (size_t i = 0; i < hl && i < 32; i++) fprintf(stderr, "%02x ", hbuf[i]);
                    fprintf(stderr, "\n");
                    /* 确定性 dump：服务端 hs_write(s_hs)+hs_read(c_hs) 密钥 + Handshake 包明文/密文/AAD，
                     * 供 Python 独立重算 c_hs/s_hs 并逐字节重建 Handshake 包比对。 */
                    FILE* fhk = fopen("dump/srv_hs_keydump.txt", "w");
                    if (fhk) {
                        fprintf(fhk, "hs_key=");
                        for (int i = 0; i < 16; i++) fprintf(fhk, "%02x", sl->hs_write.key[i]);
                        fprintf(fhk, "\nhs_iv=");
                        for (int i = 0; i < NL_QUIC_IV_LEN; i++) fprintf(fhk, "%02x", sl->hs_write.iv[i]);
                        fprintf(fhk, "\nhs_hp=");
                        for (int i = 0; i < 16; i++) fprintf(fhk, "%02x", sl->hs_write.hp[i]);
                        fprintf(fhk, "\nhs_pn=%llu\n", (unsigned long long)sl->flight_hs_pn);
                        fprintf(fhk, "hs_pn_offset=%zu\nhs_out_len=%zu\n", hs_pn_off, hl);
                        fprintf(fhk, "hs_pt_hex=");
                        for (size_t i = 0; i < cfl; i++) fprintf(fhk, "%02x", cf[i]);
                        fprintf(fhk, "\nhs_ct_hex=");
                        size_t hct_len = hl - hs_pn_off;
                        for (size_t i = hs_pn_off; i < hl; i++) fprintf(fhk, "%02x", hbuf[i]);
                        fprintf(fhk, "\nhs_aad_hex=");
                        for (size_t i = 0; i < hs_aad_len; i++) fprintf(fhk, "%02x", hs_aad[i]);
                        fprintf(fhk, "\n");
                        {
                            FILE* di = fopen("dump/first_initial.bin", "rb");
                            if (di) {
                                unsigned char ibuf[4096]; size_t il = fread(ibuf, 1, sizeof(ibuf), di);
                                fclose(di);
                                if (il > 0) {
                                    fprintf(fhk, "cli_init_len=%zu\ncli_init_hex=", il);
                                    for (size_t i = 0; i < il; i++) fprintf(fhk, "%02x", ibuf[i]);
                                    fprintf(fhk, "\n");
                                }
                            }
                        }
                        if (sl->hs_read.key_len > 0) {
                            fprintf(fhk, "chs_key=");
                            for (int i = 0; i < 16; i++) fprintf(fhk, "%02x", sl->hs_read.key[i]);
                            fprintf(fhk, "\nchs_iv=");
                            for (int i = 0; i < NL_QUIC_IV_LEN; i++) fprintf(fhk, "%02x", sl->hs_read.iv[i]);
                            fprintf(fhk, "\nchs_hp=");
                            for (int i = 0; i < 16; i++) fprintf(fhk, "%02x", sl->hs_read.hp[i]);
                            fprintf(fhk, "\n");
                        }
                        fclose(fhk);
                        fprintf(stderr, "[h3-srv] dumped hs_write keys to /tmp/srv_hs_keydump.txt\n");
                    }
                    fflush(stderr);
                }
#endif
                    sl->tx_hs_pn = sl->flight_hs_pn + 1;
                }
                /* Initial 级 ACK：largest_acked 用包内自解析的实际包号 actual_rx_pn
                 * （PTO 重传包的原始包号），在冻结包号上重新发送。
                 * 守卫改为“只在首次建槽后”：每次 PTO 重传都回一次新 ACK，
                 * 否则客户端收不到覆盖其重传包号的 ACK → 持续 PTO → idle 超时。 */
                {
                    uint8_t ackf[64]; size_t al = 0;
                    uint64_t largest = actual_rx_pn;
                    ack_frame_write(ackf, sizeof(ackf), largest, 0, &al);
                    if (al > 0) {
                        uint8_t aout[NLH3_MAX_PKT]; size_t aol = 0;
                        /* Initial ACK DCID：用客户端 SCID（aioquic _host_cids 匹配） */
                        const uint8_t* ack_dcid = dcid;
                        size_t ack_dcid_len = dcid_len;
                        if (sl && sl->client_scid_len > 0) {
                            ack_dcid = sl->client_scid;
                            ack_dcid_len = sl->client_scid_len;
                        }
                        /* Initial 级 ACK 包：RFC 9000 §14.3 要求 padding 至 1200B
                         * pn_len=2 时头部精确长 = (dcid_len+12) */
                        size_t total_no_pad = ack_dcid_len + 12 + al + NL_QUIC_TAG_LEN;
                        size_t pad = (total_no_pad < 1200) ? (1200 - total_no_pad) : 0;
                        if (pkt_build_long(0, ack_dcid, ack_dcid_len, NULL, 0,
                                           sl->flight_init_pn, 2, ackf, al, &sl->si_keys,
                                           aout, sizeof(aout), &aol, NULL, NULL, NULL, pad) == 0) {
                            nlh3_send_to(s, aout, aol, peer);
                        }
                    }
                    sl->acked = 1;
                }
                sl->hs_sent = 1;       /* 标记已回过 flight；后续 Initial 重传走缓存重放 */
                sl->rx_init_pn++;
                return 1;
            }
        }

        /* 2b) 客户端 Handshake（Finished）：type=2，用 Handshake 读密钥 */
        if (pprc == 0 && ty == 2 && sl && sl->conn && !sl->established) {
            uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, cons = 0;
            if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &cons) == 0 && off == 0) {
                if (nl_tls13_server_finish(sl->conn, cd, cdl) != 0) return -1;
                if (nl_tls13_conn_get_1rtt_keys(sl->conn, &sl->rtt_read, &sl->rtt_write) != 0)
                    return -1;
                sl->established = 1;
                sl->rtt_read_set = 1;
                /* 发送服务端 1-RTT CRYPTO（TLS FINISHED, msg_type=20）：
                  * 校验值 = finished_verify_data(finished_key(s_hs), Hash(transcript-so-far))，
                  * 与 nl_tls13_server_handshake 已发 FINISHED 一致。客户端收到后才进
                  * application-data，进而发 H3 请求数据帧。 */
                 {
                     uint8_t fmsg[40]; size_t fml = 0;
                     if (nl_tls13_server_finished_msg(sl->conn, fmsg, sizeof(fmsg), &fml) == 0 && fml > 0) {
                         uint8_t cfq[NLH3_MAX_PKT]; size_t cfl2 = 0;
                         if (crypto_frame_write(cfq, sizeof(cfq), 0, fmsg, fml, &cfl2) == 0) {
                             uint8_t cpo[NLH3_MAX_PKT]; size_t cpl = 0;
                             uint64_t pn2 = sl->tx_rtt_pn;
                             if (pkt_build_short(pn2, 1, cfq, cfl2, &sl->rtt_write,
                                                  sl->client_scid, sl->client_scid_len,
                                                  cpo, sizeof(cpo), &cpl) == 0) {
                                 nlh3_send_to(s, cpo, cpl, peer);
                                 sl->tx_rtt_pn = pn2 + 1;
                                 /* dump 1-RTT 密钥 + CRYPTO 明文/密文，供 Python 独立验证 */
                                 {
                                     FILE* frk = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_1rtt_keydump.txt", "w");
                                     if (frk) {
                                         fprintf(frk, "rtt_write_key=");
                                         for (int i = 0; i < 16; i++) fprintf(frk, "%02x", sl->rtt_write.key[i]);
                                         fprintf(frk, "\nrtt_write_iv=");
                                         for (int i = 0; i < 12; i++) fprintf(frk, "%02x", sl->rtt_write.iv[i]);
                                         fprintf(frk, "\nrtt_write_hp=");
                                         for (int i = 0; i < 16; i++) fprintf(frk, "%02x", sl->rtt_write.hp[i]);
                                         fprintf(frk, "\nrtt_pn=%llu\nrtt_dcid_len=%zu\nrtt_pkt_len=%zu\n",
                                                 (unsigned long long)pn2, sl->client_scid_len, cpl);
                                         fprintf(frk, "rtt_pkt_hex=");
                                         for (size_t i = 0; i < cpl; i++) fprintf(frk, "%02x", cpo[i]);
                                         fprintf(frk, "\nrtt_fmsg_hex=");
                                         for (size_t i = 0; i < fml; i++) fprintf(frk, "%02x", fmsg[i]);
                                         fprintf(frk, "\nrtt_cfq_hex=");
                                         for (size_t i = 0; i < cfl2; i++) fprintf(frk, "%02x", cfq[i]);
                                         fprintf(frk, "\n");
                                         fclose(frk);
                                     }
                                 }
#ifdef NLH3_DBG
                                fprintf(stderr, "[h3-srv] 1rtt TX srvFinished CRYPTO pn=%llu len=%zu fml=%zu\n",
                                        (unsigned long long)pn2, cpl, fml);
                                fflush(stderr);
#endif
                             }
                         }
                     }
                 }
                /* 回 Handshake 级 ACK：用 hs_write 加密（服务端→客户端的写侧密钥），
                 * largest_acked = 客户端 Handshake 最大已收包号（rx_hs_pn-1）。
                 * 该 ACK 本身作为一条新的服务端 Handshake 包发送，包号 = tx_hs_pn
                 * （首次 flight 结束后 tx_hs_pn 已冻结为 flight_hs_pn+1，不会漂移）。 */
                {
                    uint8_t ackf[64]; size_t al = 0;
                    uint64_t largest = sl->rx_hs_pn;   /* 客户端已收到的最大 HS 包号 */
                    ack_frame_write(ackf, sizeof(ackf), largest, 0, &al);
                    if (al > 0) {
                        uint8_t aout[NLH3_MAX_PKT]; size_t aol = 0;
                        /* HS ACK DCID：用客户端 SCID（aioquic _host_cids 匹配） */
                        const uint8_t* hs_ack_dcid = sl->client_scid;
                        size_t hs_ack_dcid_len = sl->client_scid_len;
                        if (pkt_build_long(2, hs_ack_dcid, hs_ack_dcid_len, NULL, 0,
                                           sl->tx_hs_pn, 2, ackf, al, &sl->hs_write,
                                           aout, sizeof(aout), &aol, NULL, NULL, NULL, 0) == 0) {
                            nlh3_send_to(s, aout, aol, peer);
                            sl->tx_hs_pn++;
                        }
                    }
                }
                sl->rx_hs_pn++;
                return 1;
            }
        }
    }

    if (!sl) return 0;

    /* 2) 服务端握手读密钥：client Finished（type=2）已在上方 2b 分支处理；
     * 此处保留作为 1-RTT 建立前的兜底（处理重传的 client Finished）。 */
    if (sl->conn && !sl->established) {
        memcpy(buf, dg, dlen);
        const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
#ifdef NLH3_DBG
        int pprc2 = pkt_parse_long(buf, dlen, &sl->hs_read, sl->rx_hs_pn, &fr, &fl, &ty, NULL);
        fprintf(stderr, "[h3-srv] fallback hs_read b0=0x%02x rc=%d ty=%d rx_hs_pn=%llu\n",
                dg[0], pprc2, ty, (unsigned long long)sl->rx_hs_pn);
        fflush(stderr);
#else
        int pprc2 = pkt_parse_long(buf, dlen, &sl->hs_read, sl->rx_hs_pn, &fr, &fl, &ty, NULL);
#endif
        if (pprc2 == 0 && ty == 2) {
            uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, cons = 0;
            if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &cons) == 0 && off == 0) {
                if (nl_tls13_server_finish(sl->conn, cd, cdl) != 0) return -1;
                if (nl_tls13_conn_get_1rtt_keys(sl->conn, &sl->rtt_read, &sl->rtt_write) != 0)
                    return -1;
                sl->established = 1;
                sl->rtt_read_set = 1;
                sl->rx_hs_pn++;
#ifdef NLH3_DBG
                fprintf(stderr, "[h3-srv] HANDSHAKE OK -> established\n");
                fflush(stderr);
#endif
                /* 同 2b 分支：发送 1-RTT CRYPTO（服务端 FINISHED），使 aioquic 客户端
                 * 完成 TLS 1-RTT 阶段并发起 H3 请求数据帧。 */
                {
                    uint8_t fmsg[40]; size_t fml = 0;
                    if (nl_tls13_server_finished_msg(sl->conn, fmsg, sizeof(fmsg), &fml) == 0 && fml > 0) {
                        uint8_t cfq[NLH3_MAX_PKT]; size_t cfl2 = 0;
                        if (crypto_frame_write(cfq, sizeof(cfq), 0, fmsg, fml, &cfl2) == 0) {
                            uint8_t cpo[NLH3_MAX_PKT]; size_t cpl = 0;
                            uint64_t pn2 = sl->tx_rtt_pn;
                            if (pkt_build_short(pn2, 1, cfq, cfl2, &sl->rtt_write,
                                                 sl->client_scid, sl->client_scid_len,
                                                 cpo, sizeof(cpo), &cpl) == 0) {
                                nlh3_send_to(s, cpo, cpl, peer);
                                sl->tx_rtt_pn = pn2 + 1;
                                /* dump 1-RTT 密钥 + CRYPTO 明文/密文，供 Python 独立验证 */
                                {
                                    FILE* frk = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_1rtt_keydump.txt", "w");
                                    if (frk) {
                                        fprintf(frk, "rtt_write_key=");
                                        for (int i = 0; i < 16; i++) fprintf(frk, "%02x", sl->rtt_write.key[i]);
                                        fprintf(frk, "\nrtt_write_iv=");
                                        for (int i = 0; i < 12; i++) fprintf(frk, "%02x", sl->rtt_write.iv[i]);
                                        fprintf(frk, "\nrtt_write_hp=");
                                        for (int i = 0; i < 16; i++) fprintf(frk, "%02x", sl->rtt_write.hp[i]);
                                        fprintf(frk, "\nrtt_pn=%llu\nrtt_dcid_len=%zu\nrtt_pkt_len=%zu\n",
                                                (unsigned long long)pn2, sl->client_scid_len, cpl);
                                        fprintf(frk, "rtt_pkt_hex=");
                                        for (size_t i = 0; i < cpl; i++) fprintf(frk, "%02x", cpo[i]);
                                        fprintf(frk, "\nrtt_fmsg_hex=");
                                        for (size_t i = 0; i < fml; i++) fprintf(frk, "%02x", fmsg[i]);
                                        fprintf(frk, "\nrtt_cfq_hex=");
                                        for (size_t i = 0; i < cfl2; i++) fprintf(frk, "%02x", cfq[i]);
                                        fprintf(frk, "\n");
                                        fclose(frk);
                                    }
                                }
#ifdef NLH3_DBG
                                fprintf(stderr, "[h3-srv] 1rtt TX srvFinished CRYPTO (fallback) pn=%llu len=%zu\n",
                                        (unsigned long long)pn2, cpl);
                                fflush(stderr);
#endif
                            }
                        }
                    }
                }
                return 1;
            }
        }
    }

    /* 3) 服务端 1-RTT 读密钥：STREAM 帧内的 H3 请求
     * aioquic 的 1-RTT 包用短首部（b0=0x40|...，见 packet_builder.py L330-339），
     * 不会发 1-RTT 长首（type=3）包。client 的 1-RTT CRYPTO 包（H3 SETTINGS 流 2）
     * 先于 client Finished（type=2）到达，此时 sl->established 尚未置位。
     * 放宽条件：只要 conn 存在且 rtt_read 已派生（2a peek / 2b / fallback get 置位
     * rtt_read_set）即可处理，不要求 sl->established，使 early 1-RTT 包能被解密。 */
    if (sl->conn && (sl->established || sl->rtt_read_set)) {
        memcpy(buf, dg, dlen);
        const uint8_t* fr = NULL; size_t fl = 0;
        /* aioquic 1.4.0 客户端 1-RTT 短首 DCID 长度固定 = 客户端 connection_id_length（默认 8B），
         * 与 C 侧 SCID 长度无关。C 侧解析 client 1-RTT 包时按 client_scid_len 跳过 DCID。 */
        uint64_t actual_pn = 0;
        int short_rc = pkt_parse_short(buf, dlen, &sl->rtt_read, sl->rx_rtt_pn, &fr, &fl, &actual_pn, sl->client_scid_len);
        int short_ok = (short_rc == 0) ? 1 : 0;
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-srv] 1rtt short b0=0x%02x rc=%d actual_pn=%llu rx_rtt_pn=%llu\n",
                dg[0], short_rc, (unsigned long long)actual_pn, (unsigned long long)sl->rx_rtt_pn);
        fflush(stderr);
#endif
        /* 诊断：dump 所有入站 datagram 的原始前 40B，确认 H3 请求数据帧落在哪个包 */
#ifdef NLH3_DBG
        {
            FILE* drg = fopen("dump/inbound_dgram.bin", "wb");
            if (drg) { fwrite(dg, 1, dlen > 64 ? 64 : dlen, drg); fclose(drg); }
        }
#endif
        if (short_ok) {
            char method[16] = {0}, path[512] = {0}, authority[256] = {0};
            int got_req = 0;
            int got_crypt = 0;
            size_t p = 0;

            /* 诊断：dump 解密后的明文帧 */
#ifdef NLH3_DBG
            {
                size_t dd = fl < 64 ? fl : 64;
                fprintf(stderr, "[h3-srv] 1rtt short decrypt OK fl=%zu bytes=", fl);
                for (size_t i = 0; i < dd; i++) fprintf(stderr, "%02x ", fr[i]);
                fprintf(stderr, "\n");
            }
#endif

            while (p < fl) {
                uint64_t sid = 0, soff = 0; const uint8_t* sd = NULL; size_t sdl = 0;
                int fin = 0; size_t cons = 0;
                if (stream_frame_read(fr + p, fl - p, &sid, &soff, &sd, &sdl, &fin, &cons) != 0) {
                    /* 非 STREAM 帧：回退 CRYPTO 帧解析（client 的 Finished 等握手密文
                     * 先于 1-RTT STREAM 到达时，在 early 1-RTT 短首包中携带 CRYPTO 帧）。
                     * 处理完 CRYPTO 帧后继续下一帧，不 break。 */
                    uint64_t coff = 0; const uint8_t* cdata = NULL;
                    size_t cdlen = 0, ccons = 0;
                    if (crypto_frame_read(fr + p, fl - p, &coff, &cdata, &cdlen, &ccons) != 0) {
#ifdef NLH3_DBG
                        /* 诊断：逐帧 dump datagram 明文，定位 H3 请求数据帧实际位置
                         * （STREAM frame 0x16 可能因变长字段读法不同而被误判 UNRECOGNIZED） */
                        fprintf(stderr, "[h3-srv] 1rtt frame@p=%zu: UNRECOGNIZED ftype=0x%02x (fl=%zu) dump=",
                                p, fr[p], fl);
                        for (size_t i = p; i < fl && i - p < 64; i++) fprintf(stderr, "%02x ", fr[i]);
                        fprintf(stderr, "\n");
                        fflush(stderr);
#endif
                        break;
                    }
                    p += ccons;
#ifdef NLH3_DBG
                    {
                        size_t dd = cdlen < 16 ? cdlen : 16;
                        fprintf(stderr, "[h3-srv] 1rtt frame CRYPTO off=%llu len=%zu bytes=",
                                (unsigned long long)coff, cdlen);
                        for (size_t i = 0; i < dd; i++) fprintf(stderr, "%02x ", cdata[i]);
                        fprintf(stderr, "\n");
                        fflush(stderr);
                    }
#endif
                    /* 收集 CRYPTO 数据，若 conn 未 established 则处理（client Finished） */
                    got_crypt = 1;
                    if (!sl->established && cdata && cdlen > 0) {
                        if (nl_tls13_server_finish(sl->conn, cdata, cdlen) == 0) {
                            sl->established = 1;
                            sl->rtt_read_set = 1;
#ifdef NLH3_DBG
                            fprintf(stderr, "[h3-srv] 3 early 1-RTT CRYPTO -> server_finish OK\n");
                            fflush(stderr);
#endif
                        }
                    }
                    continue;   /* CRYPTO 帧处理完，继续解析下一帧 */
                }
#ifdef NLH3_DBG
                {
                    size_t dd = sdl < 24 ? sdl : 24;
                    fprintf(stderr, "[h3-srv] 1rtt frame STREAM sid=%llu off=%llu dlen=%zu fin=%d cons=%zu bytes=",
                            (unsigned long long)sid, (unsigned long long)soff, sdl, fin, cons);
                    for (size_t i = 0; i < dd && sd; i++) fprintf(stderr, "%02x ", sd[i]);
                    fprintf(stderr, "\n");
                    fflush(stderr);
                }
#endif
                p += cons;
                /* 客户端侧流分配（aioquic 1.3.0 实测）：
                 * 双向流 0/4/8/... 为客户端发起；单向流 2/6/10/... 为客户端发起。
                 * H3 的 3 条控制流（CONTROL / QPACK ENCODER / QPACK DECODER）占用
                 * 单向流 2、6、10（_local_next_stream_id_uni=2，每次 +4）；
                 * H3 请求/响应走客户端首个可用双向流 = stream 0（_local_next_stream_id_bidi=0）。
                 * 故：奇数流 = 服务端发起（单向/双向），本侧无 H3 请求数据，跳过；
                 * sid==2 是客户端单向控制流（流类型 SETTINGS），非请求数据，跳过；
                 * FIN-only 帧（sdl==0）无数据可解，跳过。 */
                if ((sid & 0x01) != 0) continue;            /* 服务端发起流：跳过 */
                if (sid == 2) continue;                     /* 客户端单向 H3 控制流：跳过 */
                if (sdl == 0) continue;                     /* FIN-only：无数据，跳过 */
                got_req = 1;
                {
                    size_t hp = 0;
                    while (hp < sdl) {
                        uint64_t t = 0; const uint8_t* hd = NULL; size_t hdl = 0, hc = 0;
                        if (h3_frame_read(sd + hp, sdl - hp, &t, &hd, &hdl, &hc) != 0) break;
                        hp += hc;
                        if (t == 0x01) {          /* HEADERS → QPACK 字段行 */
                            h3_kv_t kvs[16]; int kc = 0;
                            if (qpack_decode(hd, hdl, kvs, 16, &kc) == 0) {
                                for (int i = 0; i < kc; i++) {
                                    if (strcmp(kvs[i].name, ":method") == 0)
                                        snprintf(method, sizeof(method), "%s", kvs[i].value);
                                    else if (strcmp(kvs[i].name, ":path") == 0)
                                        snprintf(path, sizeof(path), "%s", kvs[i].value);
                                    else if (strcmp(kvs[i].name, ":authority") == 0)
                                        snprintf(authority, sizeof(authority), "%s", kvs[i].value);
                                }
                            }
                        }
                    }
                }
            }

#ifdef NLH3_DBG
            fprintf(stderr, "[h3-srv] 1rtt short loop done got_req=%d got_crypt=%d method=%s path=%s\n",
                    got_req, got_crypt, method, path);
#endif
            if (got_req) {
                char* body = NULL; size_t blen = 0;
                if (s->handler) s->handler(method, path, authority, s->userdata, &body, &blen);

                /* 1-RTT 响应冻结 + 去重（RFC 9000 重传须用原始包号）：
                 * 首次请求：缓存 1-RTT 响应包（固定 rtt_tx_pn）与 rtt_resp_cache，发出。
                 * 重传（actual_pn 未推进）：重放缓存的同一响应包（同包号、同字节），
                 * 不再重新生成/推进 tx_rtt_pn，否则客户端固定锚点解密错位 → rc=29。 */
                if (sl->rtt_resp_cached && actual_pn <= sl->rtt_last_actual_pn) {
                    if (sl->rtt_resp_cache_len)
                        nlh3_send_to(s, sl->rtt_resp_cache, sl->rtt_resp_cache_len, peer);
#ifdef NLH3_DBG
                    fprintf(stderr, "[h3-srv] 1rtt dup replay actual_pn=%llu cached_len=%zu\n",
                            (unsigned long long)actual_pn, sl->rtt_resp_cache_len);
                    fflush(stderr);
#endif
                    free(body);
                    sl->rx_rtt_pn++;
                    return 1;
                }
                if (!sl->rtt_replayed) {
                    sl->rtt_tx_pn = sl->tx_rtt_pn;
                    sl->rtt_replayed = 1;
                }
                sl->rtt_last_actual_pn = actual_pn;
                {
                    uint8_t rstream[NLH3_MAX_PKT]; size_t rsp = 0;
                    uint8_t rfields[64]; size_t rf = 0;
                    int k = qpack_encode_field(rfields, sizeof(rfields), ":status", "200");
                    if (k > 0) rf = (size_t)k;
                    {
                        size_t dn = 0;
                        if (h3_frame_write(rstream, sizeof(rstream), 0x01, rfields, rf, &dn) == 0)
                            rsp = dn;
                    }
                    if (body && blen) {           /* DATA 帧 */
                        size_t dn = 0;
                        if (h3_frame_write(rstream + rsp, sizeof(rstream) - rsp, 0x00,
                                           (const uint8_t*)body, blen, &dn) == 0)
                            rsp += dn;
                    }
                    /* ACK 帧 largest_acked 用 actual_pn（客户端真实 1-RTT 包号） */
                    uint8_t ackf[64]; size_t ackl = 0;
                    ack_frame_write(ackf, sizeof(ackf), actual_pn, 0, &ackl);
                    uint8_t qrf[NLH3_MAX_PKT]; size_t qrf_len = 0;
                    uint8_t out2[NLH3_MAX_PKT]; size_t outl2 = 0;
                    {
                        size_t ap = 0;
                        if (ackl > 0) {
                            memcpy(qrf + ap, ackf, ackl);
                            ap += ackl;
                        }
                        if (stream_frame_write(qrf + ap, sizeof(qrf) - ap, 0, rstream, rsp, 1, &qrf_len) == 0) {
                            ap += qrf_len;
#ifdef NLH3_DBG
                            {
                                size_t dd = ap < 48 ? ap : 48;
                                fprintf(stderr, "[h3-srv] 1rtt qrf ap=%zu bytes=", ap);
                                for (size_t i = 0; i < dd; i++) fprintf(stderr, "%02x ", qrf[i]);
                                fprintf(stderr, "\n");
                            }
                            fflush(stderr);
#endif
                            if (pkt_build_short(sl->rtt_tx_pn, 2,
                                                qrf, ap, &sl->rtt_write,
                                                sl->client_scid, sl->client_scid_len,
                                                out2, sizeof(out2), &outl2) == 0) {
                                /* 缓存响应以便重传时重放同包号 */
                                memcpy(sl->rtt_resp_cache, out2, outl2);
                                sl->rtt_resp_cache_len = outl2;
                                sl->rtt_resp_cached = 1;
                                nlh3_send_to(s, out2, outl2, peer);
                                sl->tx_rtt_pn = sl->rtt_tx_pn + 1;
#ifdef NLH3_DBG
                                fprintf(stderr, "[h3-srv] 1rtt TX resp pn=%llu len=%zu actual_pn=%llu\n",
                                        (unsigned long long)sl->rtt_tx_pn, outl2,
                                        (unsigned long long)actual_pn);
                                fflush(stderr);
#endif
                            }
                        }
                    }
                }
                if (body) free(body);
            }
            sl->rx_rtt_pn++;
            return 1;
        }
    }

    return 0;
}

#ifdef _WIN32
static DWORD WINAPI nlh3_server_thread(LPVOID arg) {
#else
static void* nlh3_server_thread(void* arg) {
#endif
    nlh3_server_t* s = (nlh3_server_t*)arg;
#ifdef NLH3_DBG
    {
        uint64_t ms = nlh3_now_ms();
        fprintf(stderr, "[h3-srv] thread started t=%llu fd=%d running=%d\n", (unsigned long long)ms, (int)s->fd, s->running);
        fflush(stderr);
    }
#endif
    int thread_rcv_cnt = 0;
    while (s->running) {
        struct sockaddr_in peer; nlh3_socklen_t plen = sizeof(peer);
        uint8_t dg[NLH3_MAX_PKT];
        nlh3_ssize_t n = recvfrom(s->fd, dg, sizeof(dg), 0, (struct sockaddr*)&peer, &plen);
#ifdef NLH3_DBG
        if (n <= 0 && thread_rcv_cnt < 30) {
            thread_rcv_cnt++;
            uint64_t ms = nlh3_now_ms();
#ifdef _WIN32
            int sock_err = (int)WSAGetLastError();
#else
            int sock_err = errno;
#endif
            fprintf(stderr, "[h3-srv] thread recvfrom t=%llu n=%d errno=%d (timeout/error)\n", (unsigned long long)ms, (int)n, sock_err);
            fflush(stderr);
        }
#endif
        if (n <= 0) continue;                 /* 超时（SO_RCVTIMEO）或错误 */
        if (s->drop_first > 0) {              /* 自测：注入丢包，直接丢弃该报文 */
            s->drop_first--;
            continue;
        }
        nlh3_server_handle(s, dg, (size_t)n, &peer);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

NL_QUICTLS_API nlh3_server_t* nlh3_server_create(int port, const char* cert_pem, const char* key_pem) {
    if (port <= 0 || !cert_pem || !key_pem) return NULL;
    if (nlh3_net_init() != 0) return NULL;

    nlh3_server_t* s = (nlh3_server_t*)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->port = port;
    s->fd = NLH3_INVALID_SOCK;

    {
        size_t cl = strlen(cert_pem) + 1, kl = strlen(key_pem) + 1;
        s->cert_pem = (char*)malloc(cl);
        s->key_pem  = (char*)malloc(kl);
        if (!s->cert_pem || !s->key_pem) { nlh3_server_destroy(s); return NULL; }
        memcpy(s->cert_pem, cert_pem, cl);
        memcpy(s->key_pem, key_pem, kl);
    }

    {
        nlh3_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr;
        int one = 1;

        if (!NLH3_SOCK_OK(fd)) { nlh3_server_destroy(s); return NULL; }
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            NLH3_CLOSE(fd); nlh3_server_destroy(s); return NULL;
        }

        nlh3_set_rcvtimeo(fd, 200);
        s->fd = fd;
    }
    return s;
}

NL_QUICTLS_API void nlh3_server_set_handler(nlh3_server_t* s, nlh3_handler_t h, void* ud) {
    if (!s) return;
    s->handler = h;
    s->userdata = ud;
}

NL_QUICTLS_API int nlh3_server_start(nlh3_server_t* s) {
    if (!s || !NLH3_SOCK_OK(s->fd) || s->thread_started) return -1;
    s->running = 1;
    if (nlh3_thread_create(&s->thread, nlh3_server_thread, s) != 0) {
        s->running = 0;
        return -1;
    }
    s->thread_started = 1;
    return 0;
}

NL_QUICTLS_API void nlh3_server_stop(nlh3_server_t* s) {
    if (!s) return;
    s->running = 0;
    if (s->thread_started) {
        nlh3_thread_join(s->thread);
        s->thread_started = 0;
    }
}

NL_QUICTLS_API void nlh3_server_destroy(nlh3_server_t* s) {
    if (!s) return;
    nlh3_server_stop(s);
    if (NLH3_SOCK_OK(s->fd)) { NLH3_CLOSE(s->fd); s->fd = NLH3_INVALID_SOCK; }
    {
        int i;
        for (i = 0; i < NLH3_MAX_CONNS; i++) {
            if (s->slots[i].conn) {
                nl_tls13_conn_free(s->slots[i].conn);
                s->slots[i].conn = NULL;
            }
            s->slots[i].valid = 0;
        }
    }
    free(s->cert_pem);
    free(s->key_pem);
    free(s);
}

NL_QUICTLS_API int nlh3_client_request_ex(const char* host, int port, const char* method,
                                          const char* path, const char* ca_pem,
                                          char* out_body, size_t cap, size_t* out_len) {
    nlh3_sock_t fd = NLH3_INVALID_SOCK;
    nl_tls13_conn_t* c = NULL;
    int rc = 0;
    nl_quic_keys_t ci, si;
    uint8_t init_pkt[NLH3_MAX_PKT]; size_t init_plen = 0;  /* Initial(ClientHello)，用于 PTO 重发 */
    uint8_t fin_pkt[1024];          size_t fin_plen = 0;   /* client Finished，用于 PTO 重发 */
    uint8_t rtt_pkt[NLH3_MAX_PKT];  size_t rtt_plen = 0;   /* H3 1-RTT 请求，用于 PTO 重发 */

    if (out_len) *out_len = 0;
    if (!host || !path || !method) return 1;
    if (nlh3_net_init() != 0) return 32;

    if (nl_quic_initial_keys(NLH3_DCID, NLH3_DCID_LEN, &ci, &si) != 0) return 2;

    c = nl_tls13_conn_new(0);
    if (!c) return 3;

    /* ca_pem 非空 → 在 TLS 客户端连接上设置信任 CA（用于校验证书链） */
    if (ca_pem) {
        if (nl_tls13_conn_set_ca(c, ca_pem) != 0) { rc = 30; goto done; }
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (!NLH3_SOCK_OK(fd)) { rc = 4; goto done; }
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1)
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) { rc = 5; goto done; }
        nlh3_set_rcvtimeo(fd, 200);
    }

    /* Initial：ClientHello（字节保存在 init_pkt，超时后按同包号重发） */
    {
        uint8_t ch[NLH3_MAX_PKT]; size_t ch_len = 0;
        uint8_t fr[NLH3_MAX_PKT]; size_t fl = 0;
        if (nl_tls13_client_hello(c, ch, sizeof(ch), &ch_len) != 0) { rc = 6; goto done; }
        if (crypto_frame_write(fr, sizeof(fr), 0, ch, ch_len, &fl) != 0) { rc = 7; goto done; }
        if (pkt_build_long(0, NLH3_DCID, NLH3_DCID_LEN, NLH3_CSID, NLH3_DCID_LEN,
                           0, 2, fr, fl, &ci, init_pkt, sizeof(init_pkt), &init_plen,
                           NULL, NULL, NULL, 0) != 0) { rc = 8; goto done; }
        if (send(fd, init_pkt, init_plen, 0) < 0) { rc = 9; goto done; }
    }

    /* 接收服务端 flight：Initial(SH) + Handshake(EE/Cert/CV/Fin) */
    {
        uint8_t rest[NLH3_MAX_PKT]; size_t rest_len = 0;
        uint8_t pending[NLH3_MAX_PKT]; size_t pending_len = 0; int have_pending = 0;
        nl_quic_keys_t c_hs_read, c_hs_write;
        uint64_t rx_init = 0, rx_hs = 0;
        int got_sh = 0, got_rest = 0, have_hs = 0;
        int retries = 5;                  /* PTO 最大重传次数 */

        /* 阶段 A：等待 ServerHello（超时则重发同一个 Initial 包） */
        for (int iter = 0; iter < 64 && !got_sh; iter++) {
            uint8_t b[NLH3_MAX_PKT];
            nlh3_ssize_t n = nlh3_recv_pto(fd, b, sizeof(b), init_pkt, init_plen, NULL, 0, &retries);
            if (n <= 0) break;
            {
                uint8_t t[NLH3_MAX_PKT]; memcpy(t, b, (size_t)n);
                const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
                if (pkt_parse_long(t, (size_t)n, &si, rx_init, &fr, &fl, &ty, NULL) == 0 && ty == 0) {
                    uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, cons = 0;
                    if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &cons) == 0 && off == 0) {
                        if (nl_tls13_client_handshake_1(c, cd, cdl) != 0) { rc = 10; goto done; }
                        if (nl_tls13_conn_get_handshake_keys(c, &c_hs_read, &c_hs_write) != 0) { rc = 11; goto done; }
                        have_hs = 1; got_sh = 1; rx_init++;
                        /* k13sh: 扫描 datagram 剩余字节里的下一个 Handshake 包
                         * （服务端把 SH 与 EE/Cert/CV/Fin 分独立 datagram 发送，见 §18.2）。 */
                        {
                            size_t consumed = pkt_long_total_len(t, (size_t)n, 0);
                            if (consumed > 0 && consumed < (size_t)n) {
                                size_t off2 = consumed;
                                const uint8_t* fr2 = NULL; size_t fl2 = 0; uint8_t ty2 = 0;
                                int prc2 = pkt_parse_long(t + off2, (size_t)n - off2, &c_hs_read, rx_hs,
                                                          &fr2, &fl2, &ty2, NULL);
                                if (prc2 == 0 && ty2 == 2) {
                                    uint64_t off3 = 0; const uint8_t* cd2 = NULL; size_t cdl2 = 0, cons2 = 0;
                                    if (crypto_frame_read(fr2, fl2, &off3, &cd2, &cdl2, &cons2) == 0 &&
                                        off3 == 0 && cdl2 <= sizeof(rest)) {
                                        memcpy(rest, cd2, cdl2); rest_len = cdl2; got_rest = 1; rx_hs++;
                                    }
                                }
                            }
                        }
                        continue;
                    }
                }
            }
            if (!have_pending) {              /* 先到的 Handshake 包暂存 */
                memcpy(pending, b, (size_t)n); pending_len = (size_t)n; have_pending = 1;
            }
        }
        if (!got_sh) { rc = 12; goto done; }

        /* 阶段 B：处理暂存报文 + 等待 EE/Cert/CV/Fin */
        if (have_pending) {
            uint8_t t[NLH3_MAX_PKT]; memcpy(t, pending, pending_len);
            const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
            if (pkt_parse_long(t, pending_len, &c_hs_read, rx_hs, &fr, &fl, &ty, NULL) == 0 && ty == 2) {
                uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, cons = 0;
                if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &cons) == 0 && off == 0 &&
                    cdl <= sizeof(rest)) {
                    memcpy(rest, cd, cdl); rest_len = cdl; got_rest = 1; rx_hs++;
                }
            }
        }
        for (int iter = 0; iter < 64 && !got_rest; iter++) {
            uint8_t b[NLH3_MAX_PKT];
            nlh3_ssize_t n = nlh3_recv_pto(fd, b, sizeof(b), init_pkt, init_plen, NULL, 0, &retries);
            if (n <= 0) break;
            {
                uint8_t t[NLH3_MAX_PKT]; memcpy(t, b, (size_t)n);
                const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
                if (pkt_parse_long(t, (size_t)n, &c_hs_read, rx_hs, &fr, &fl, &ty, NULL) == 0 && ty == 2) {
                    uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, cons = 0;
                    if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &cons) == 0 && off == 0 &&
                        cdl <= sizeof(rest)) {
                        memcpy(rest, cd, cdl); rest_len = cdl; got_rest = 1; rx_hs++;
                    }
                }
            }
        }
        if (!got_rest) { rc = 13; goto done; }
        (void)have_hs;

        /* 处理 EE/Cert/CV/Fin，产出 client Finished 并经 Handshake 包发出 */
        {
            uint8_t cf[512]; size_t cf_len = 0;
            uint8_t fr[1024]; size_t fl = 0;
            if (nl_tls13_client_handshake_2(c, rest, rest_len, cf, sizeof(cf), &cf_len) != 0) { rc = 14; goto done; }
            if (!nl_tls13_conn_established(c)) { rc = 15; goto done; }
            /* ca_pem 非空 → 握手完成后要求对端证书已通过校验，否则视为失败 */
            if (ca_pem && nl_tls13_conn_peer_verified(c) != 1) { rc = 31; goto done; }
            if (crypto_frame_write(fr, sizeof(fr), 0, cf, cf_len, &fl) != 0) { rc = 16; goto done; }
            if (pkt_build_long(2, NLH3_DCID, NLH3_DCID_LEN, NLH3_CSID, NLH3_DCID_LEN,
                               0, 2, fr, fl, &c_hs_write, fin_pkt, sizeof(fin_pkt), &fin_plen,
                               NULL, NULL, NULL, 0) != 0) { rc = 17; goto done; }
            if (send(fd, fin_pkt, fin_plen, 0) < 0) { rc = 18; goto done; }
        }
    }

    /* 1-RTT：H3 请求（HEADERS :method/:scheme https/:authority host/:path，fin=1） */
    {
        nl_quic_keys_t c_rtt_read, c_rtt_write;
        uint8_t fbuf[NLH3_MAX_PKT]; size_t fp = 0;

        if (nl_tls13_conn_get_1rtt_keys(c, &c_rtt_read, &c_rtt_write) != 0) { rc = 19; goto done; }

        /* 控制流 SETTINGS（sid=2） */
        {
            uint8_t h3set[16]; size_t h3set_len = 0;
            uint8_t sf[32]; size_t sf_len = 0;
            if (h3_frame_write(h3set, sizeof(h3set), 0x04, NULL, 0, &h3set_len) == 0 &&
                stream_frame_write(sf, sizeof(sf), 2, h3set, h3set_len, 0, &sf_len) == 0) {
                memcpy(fbuf + fp, sf, sf_len); fp += sf_len;
            }
        }
        /* 请求流 HEADERS（sid=0，fin=1） */
        {
            uint8_t fields[640]; size_t fld = 0; int k;
            uint8_t h3hdr[768]; size_t h3hdr_len = 0;
            uint8_t sf2[896]; size_t sf2_len = 0;
            k = qpack_encode_field(fields + fld, sizeof(fields) - fld, ":method", method);
            if (k < 0) { rc = 20; goto done; } fld += (size_t)k;
            k = qpack_encode_field(fields + fld, sizeof(fields) - fld, ":scheme", "https");
            if (k < 0) { rc = 21; goto done; } fld += (size_t)k;
            k = qpack_encode_field(fields + fld, sizeof(fields) - fld, ":authority", host);
            if (k < 0) { rc = 22; goto done; } fld += (size_t)k;
            k = qpack_encode_field(fields + fld, sizeof(fields) - fld, ":path", path);
            if (k < 0) { rc = 23; goto done; } fld += (size_t)k;
            k = qpack_encode_field(fields + fld, sizeof(fields) - fld, "user-agent", "NetLeaf-quictls/0.1");
            if (k < 0) { rc = 24; goto done; } fld += (size_t)k;

            if (h3_frame_write(h3hdr, sizeof(h3hdr), 0x01, fields, fld, &h3hdr_len) != 0) { rc = 25; goto done; }
            if (stream_frame_write(sf2, sizeof(sf2), 0, h3hdr, h3hdr_len, 1, &sf2_len) != 0) { rc = 26; goto done; }
            memcpy(fbuf + fp, sf2, sf2_len); fp += sf2_len;
        }
        {
            if (pkt_build_short(0, 1,
                                     fbuf, fp, &c_rtt_write, NULL, 0, rtt_pkt, sizeof(rtt_pkt), &rtt_plen) != 0) { rc = 27; goto done; }
            if (send(fd, rtt_pkt, rtt_plen, 0) < 0) { rc = 28; goto done; }
        }

        /* 接收 1-RTT 响应，解析 DATA 帧 → out_body
         * 超时则重发 client Finished 与 H3 请求（同包号、同字节）后再等 */
        {
            int ok = 0;
            int rtt_retries = 5;
            for (int iter = 0; iter < 64 && !ok; iter++) {
                uint8_t b[NLH3_MAX_PKT];
                nlh3_ssize_t n = nlh3_recv_pto(fd, b, sizeof(b),
                                          fin_pkt, fin_plen, rtt_pkt, rtt_plen, &rtt_retries);
                if (n <= 0) break;
                {
                    uint8_t t[NLH3_MAX_PKT]; memcpy(t, b, (size_t)n);
                    const uint8_t* fr = NULL; size_t fl = 0;
                    int prc = pkt_parse_short(t, (size_t)n, &c_rtt_read, 0, &fr, &fl, NULL, 0);
#ifdef NLH3_DBG
                    fprintf(stderr, "[h3-cli] 1rtt recv n=%zd prc=%d fl=%zu ok=%d iter=%d\n",
                            n, prc, fl, ok, iter);
                    fflush(stderr);
#endif
                    if (prc != 0) continue;
                    {
                        size_t p = 0;
#ifdef NLH3_DBG
                        {
                            size_t dd = fl < 64 ? fl : 64;
                            fprintf(stderr, "[h3-cli] 1rtt resp frames fl=%zu bytes=", fl);
                            for (size_t i = 0; i < dd; i++) fprintf(stderr, "%02x ", fr[i]);
                            fprintf(stderr, "\n");
                        }
                        fflush(stderr);
#endif
                        while (p < fl) {
                            /* 读帧类型 */
                            uint64_t ftype = 0; size_t fn = nl_quic_varint_read(fr + p, fl - p, &ftype);
                            if (fn == 0) break;
                            size_t fsave = p; p += fn;
                            if ((ftype & 0xF8) != 0x08) {
                                /* 非 STREAM 帧：按各类型长度跳过 */
                                if (ftype == 0x02) {            /* ACK: largest + delay + range_count + [first width] + ranges */
                                    uint64_t v, cnt = 0; size_t m;
                                    m = nl_quic_varint_read(fr + p, fl - p, &v); if (!m) break; p += m;  /* largest_acked */
                                    m = nl_quic_varint_read(fr + p, fl - p, &v); if (!m) break; p += m;  /* ack_delay */
                                    m = nl_quic_varint_read(fr + p, fl - p, &cnt); if (!m) break; p += m; /* range_count */
                                    /* RFC 9000 §19.3：first range width 无条件存在（count=0 时也读 1 个） */
                                    m = nl_quic_varint_read(fr + p, fl - p, &v); if (!m) break; p += m; /* first width */
                                    for (uint64_t ri = 0; ri < cnt && p < fl; ri++) {
                                        m = nl_quic_varint_read(fr + p, fl - p, &v); if (!m) break; p += m; /* gap */
                                        m = nl_quic_varint_read(fr + p, fl - p, &v); if (!m) break; p += m; /* width */
                                    }
                                } else if (ftype == 0x04) {     /* CRYPTO: offset + len + data */
                                    uint64_t off, dl; size_t m;
                                    m = nl_quic_varint_read(fr + p, fl - p, &off); if (!m) break; p += m;
                                    m = nl_quic_varint_read(fr + p, fl - p, &dl);  if (!m) break; p += m;
                                    p += dl; if (p > fl) break;
                                } else if (ftype == 0x01 || ftype == 0x03) {
                                    /* PADDING(0x01) 填充到末尾 / PING(0x03) 无载荷 */
                                    if (ftype == 0x01) { p = fl; continue; }
                                } else {
                                    p = fl;                     /* 未知帧：跳至末尾 */
                                }
                                continue;
                            }
                            /* STREAM 帧：把已消费的 type 字节回写以便 stream_frame_read 解析 */
                            p = fsave;
                            uint64_t sid = 0, soff = 0; const uint8_t* sd = NULL; size_t sdl = 0;
                            int fin = 0; size_t cons = 0;
                            if (stream_frame_read(fr + p, fl - p, &sid, &soff, &sd, &sdl, &fin, &cons) != 0) break;
                            p += cons;
                            if (sid != 0) continue;
                            {
                                size_t hp = 0;
                                while (hp < sdl) {
                                    uint64_t ty = 0; const uint8_t* hd = NULL; size_t hdl = 0, hc = 0;
                                    if (h3_frame_read(sd + hp, sdl - hp, &ty, &hd, &hdl, &hc) != 0) break;
                                    hp += hc;
#ifdef NLH3_DBG
                                    fprintf(stderr, "[h3-cli] 1rtt h3frame ty=%llu hdl=%zu sdl=%zu hc=%zu\n",
                                            (unsigned long long)ty, hdl, sdl, hc);
#endif
                                    if (ty == 0x00) {         /* DATA */
                                        size_t copy = (hdl < cap) ? hdl : cap;
                                        if (out_body && copy) memcpy(out_body, hd, copy);
                                        if (out_len) *out_len = copy;
                                        ok = 1;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (!ok) { rc = 29; goto done; }
        }
    }

    rc = 0;
done:
    if (c) nl_tls13_conn_free(c);
    if (NLH3_SOCK_OK(fd)) NLH3_CLOSE(fd);
    return rc;
}

NL_QUICTLS_API int nlh3_client_request(const char* host, int port, const char* method,
                                       const char* path, char* out_body, size_t cap, size_t* out_len) {
    return nlh3_client_request_ex(host, port, method, path, NULL, out_body, cap, out_len);
}

static void nlh3_test_handler(const char* method, const char* path, const char* authority,
                              void* userdata, char** resp_body, size_t* resp_len) {
    static const char msg[] = "Hello h3";
    size_t n = sizeof(msg) - 1;
    char* b;
    (void)method; (void)path; (void)authority; (void)userdata;
    b = (char*)malloc(n + 1);
    if (b) { memcpy(b, msg, n); b[n] = '\0'; }
    *resp_body = b;
    *resp_len = b ? n : 0;
}

/* selftest：单个客户端线程的参数与入口 */
typedef struct nlh3_cli_arg {
    const char* ca_pem;
    int port;
    char body[64];
    size_t blen;
    int rc;
} nlh3_cli_arg_t;

#ifdef _WIN32
static DWORD WINAPI nlh3_selftest_client_thread(LPVOID arg) {
#else
static void* nlh3_selftest_client_thread(void* arg) {
#endif
    nlh3_cli_arg_t* a = (nlh3_cli_arg_t*)arg;
    a->blen = 0;
    a->body[0] = '\0';
    a->rc = nlh3_client_request_ex("127.0.0.1", a->port, "GET", "/hello", a->ca_pem,
                                   a->body, sizeof(a->body), &a->blen);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

NL_QUICTLS_API int nl_quictls_h3server_selftest(void) {
    static char cert_pem[4096];
    static char key_pem[4096];
    nlh3_server_t* s;
    nlh3_cli_arg_t args[2];
    nlh3_thread_t th[2];
    int i;

    if (nl_tls13_gen_self_signed(cert_pem, sizeof(cert_pem), key_pem, sizeof(key_pem)) != 0) return 1;

    s = nlh3_server_create(44333, cert_pem, key_pem);
    if (!s) return 2;

    nlh3_server_set_handler(s, nlh3_test_handler, NULL);
    if (nlh3_server_start(s) != 0) { nlh3_server_destroy(s); return 3; }

    NLH3_SLEEP_MS(150);

    /* 两个客户端并发：均以服务端自签证书作为 CA，通过证书校验 */
    memset(args, 0, sizeof(args));
    for (i = 0; i < 2; i++) { args[i].ca_pem = cert_pem; args[i].port = 44333; }

    for (i = 0; i < 2; i++) {
        if (nlh3_thread_create(&th[i], nlh3_selftest_client_thread, &args[i]) != 0) {
            int j;
            for (j = 0; j < i; j++) nlh3_thread_join(th[j]);
            nlh3_server_stop(s);
            nlh3_server_destroy(s);
            return 4;
        }
    }
    for (i = 0; i < 2; i++) nlh3_thread_join(th[i]);

    nlh3_server_stop(s);
    nlh3_server_destroy(s);

    for (i = 0; i < 2; i++) {
        if (args[i].rc != 0) return 5;
        if (args[i].blen != 8 || memcmp(args[i].body, "Hello h3", 8) != 0) return 6;
    }

    /* 丢包用例：服务端丢弃前 3 个收到的报文（触发客户端 PTO 重传），
     * 客户端仍能完成握手 + H3 请求/响应（证明重传生效） */
    {
        nlh3_server_t* s2;
        nlh3_cli_arg_t a2;
        nlh3_thread_t th2;

        s2 = nlh3_server_create(44334, cert_pem, key_pem);
        if (!s2) return 7;
        s2->drop_first = 3;                 /* 丢弃前 3 个收到的报文 */
        nlh3_server_set_handler(s2, nlh3_test_handler, NULL);
        if (nlh3_server_start(s2) != 0) { nlh3_server_destroy(s2); return 8; }

        NLH3_SLEEP_MS(150);

        memset(&a2, 0, sizeof(a2));
        a2.ca_pem = cert_pem;
        a2.port = 44334;
        if (nlh3_thread_create(&th2, nlh3_selftest_client_thread, &a2) != 0) {
            nlh3_server_stop(s2);
            nlh3_server_destroy(s2);
            return 9;
        }
        nlh3_thread_join(th2);

        nlh3_server_stop(s2);
        nlh3_server_destroy(s2);

        if (a2.rc != 0) return 10;
        if (a2.blen != 8 || memcmp(a2.body, "Hello h3", 8) != 0) return 11;
    }
    return 0;
}
