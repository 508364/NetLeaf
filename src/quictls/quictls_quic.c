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

#include "netleaf_quictls.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

/* 解析 CRYPTO 帧（datagram 起始）：返回 0 并给出 offset/data/len/consumed */
static int crypto_frame_read(const uint8_t* d, size_t len,
                             uint64_t* off, const uint8_t** data, size_t* dlen, size_t* consumed) {
    size_t p = 0, n;
    uint64_t type = 0;
    n = nl_quic_varint_read(d + p, len - p, &type); if (!n) return -1; p += n;
    if (type != 0x06) return -1;
    n = nl_quic_varint_read(d + p, len - p, off); if (!n) return -1; p += n;
    n = nl_quic_varint_read(d + p, len - p, &type); if (!n) return -1; p += n;   /* length */
    if (p + type > len) return -1;
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
    if ((t & 0xf8) != 0x08) return -1;
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
 * 数据包构建 / 解析
 * ============================================================ */
/* 长首部（type: 0=Initial, 2=Handshake） */
static int pkt_build_long(uint8_t type,
                          const uint8_t* dcid, size_t dcid_len,
                          const uint8_t* scid, size_t scid_len,
                          uint64_t pn, size_t pn_len,
                          const uint8_t* frames, size_t frames_len,
                          const nl_quic_keys_t* keys,
                          uint8_t* out, size_t cap, size_t* out_len) {
    size_t p = 0, n;
    if (p + 1 > cap) return -1;
    out[p++] = (uint8_t)(0xC0 | (type << 4) | (pn_len - 1));
    if (p + 4 > cap) return -1;
    out[p++] = 0; out[p++] = 0; out[p++] = 0; out[p++] = 1;   /* version 1 */
    if (p + 1 + dcid_len > cap) return -1;
    out[p++] = (uint8_t)dcid_len; memcpy(out + p, dcid, dcid_len); p += dcid_len;
    if (p + 1 + scid_len > cap) return -1;
    out[p++] = (uint8_t)scid_len; memcpy(out + p, scid, scid_len); p += scid_len;
    if (type == 0) { n = nl_quic_varint_write(out + p, cap - p, 0); if (!n) return -1; p += n; }  /* token */

    uint64_t length = (uint64_t)(pn_len + frames_len + NL_QUIC_TAG_LEN);
    n = nl_quic_varint_write(out + p, cap - p, length); if (!n) return -1; p += n;

    size_t pn_offset = p;
    if (p + pn_len > cap) return -1;
    for (size_t i = 0; i < pn_len; i++) out[p++] = (uint8_t)(pn >> (8 * (pn_len - 1 - i)));
    if (p + frames_len > cap) return -1;
    memcpy(out + p, frames, frames_len); p += frames_len;

    size_t total = 0;
    if (nl_quic_protect_packet_ex(keys, pn, 1, out, pn_offset, pn_len, frames_len, &total) != 0) return -1;
    *out_len = total;
    return 0;
}

/* 解析长首部；pkt 可变（就地解保护）。expected_pn 为有序场景下的期望包号。 */
static int pkt_parse_long(uint8_t* pkt, size_t len, const nl_quic_keys_t* keys, uint64_t expected_pn,
                          const uint8_t** frames, size_t* frames_len, uint8_t* out_type) {
    if (len < 8) return -1;
    uint8_t b0 = pkt[0];
    if (!(b0 & 0x80)) return -2;
    uint8_t type = (uint8_t)((b0 >> 4) & 0x03);
    size_t p = 1 + 4;
    if (p >= len) return -3;
    size_t dcid_len = pkt[p++]; p += dcid_len;
    if (p >= len) return -3;
    size_t scid_len = pkt[p++]; p += scid_len;
    if (type == 0) {
        uint64_t tl = 0; size_t n = nl_quic_varint_read(pkt + p, len - p, &tl); if (!n) return -3; p += n + (size_t)tl;
    }
    uint64_t length = 0;
    size_t n = nl_quic_varint_read(pkt + p, len - p, &length); if (!n) return -3; p += n;
    size_t pn_offset = p;

    /* 先解除首部保护（自动读取 pn_len），再据 length 计算密文长度并 AEAD 解密 */
    size_t pn_len = 0, plen = 0;
    if (nl_quic_hp_unprotect(keys, pkt, pn_offset, 1, len, &pn_len) != 0) return -5;
    if (length < pn_len + NL_QUIC_TAG_LEN) return -4;
    size_t ct_len = (size_t)length - pn_len - NL_QUIC_TAG_LEN;
    if (nl_quic_unprotect_payload(keys, expected_pn, pkt, pn_offset, pn_len, ct_len, &plen) != 0) return -5;

    *frames = pkt + pn_offset + pn_len;
    *frames_len = plen;
    if (out_type) *out_type = type;
    return 0;
}

/* 短首部（1-RTT，本实现 DCID 长度 0） */
static int pkt_build_short(uint64_t pn, size_t pn_len,
                           const uint8_t* frames, size_t frames_len,
                           const nl_quic_keys_t* keys,
                           uint8_t* out, size_t cap, size_t* out_len) {
    size_t p = 0;
    if (p + 1 > cap) return -1;
    out[p++] = (uint8_t)(0x40 | (pn_len - 1));
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
                           const uint8_t** frames, size_t* frames_len) {
    if (len < 2 + NL_QUIC_TAG_LEN) return -1;
    uint8_t b0 = pkt[0];
    if (b0 & 0x80) return -1;                     /* 应为短首部 */
    size_t pn_offset = 1, pn_len = 0, plen = 0;
    if (nl_quic_hp_unprotect(keys, pkt, pn_offset, 0, len, &pn_len) != 0) return -1;
    if (1 + pn_len + NL_QUIC_TAG_LEN > len) return -1;
    size_t ct_len = len - 1 - pn_len - NL_QUIC_TAG_LEN;
    if (nl_quic_unprotect_payload(keys, expected_pn, pkt, pn_offset, pn_len, ct_len, &plen) != 0) return -1;
    *frames = pkt + 1 + pn_len;
    *frames_len = plen;
    return 0;
}

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
    if (pkt_build_long(0, DCID, sizeof(DCID), CSID, sizeof(CSID), 0, 1,
                       frames, f_len, &ci_keys, pk1, sizeof(pk1), &pk1_len) != 0) { rc = 8; goto done; }

    /* 服务端收 Initial */
    {
        uint8_t buf[2048]; if (pk1_len > sizeof(buf)) { rc = 9; goto done; }
        memcpy(buf, pk1, pk1_len);
        const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
        if (pkt_parse_long(buf, pk1_len, &ci_keys, 0, &fr, &fl, &ty) != 0 || ty != 0) { rc = 10; goto done; }
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
        if (pkt_build_long(0, CSID, sizeof(CSID), SSID, sizeof(SSID), 1, 1,
                           cbuf, f2, &si_keys, pk2, sizeof(pk2), &pk2_len) != 0) { rc = 17; goto done; }

        /* 服务端 Handshake：其余（EE..Fin） */
        const uint8_t* rest = frames + sh_len;
        size_t rest_len = f_len - sh_len;
        size_t f3 = 0;
        if (crypto_frame_write(cbuf, sizeof(cbuf), 0, rest, rest_len, &f3) != 0) { rc = 18; goto done; }
        uint8_t pk3[2048]; size_t pk3_len = 0;
        if (pkt_build_long(2, CSID, sizeof(CSID), SSID, sizeof(SSID), 0, 1,
                           cbuf, f3, &s_hw, pk3, sizeof(pk3), &pk3_len) != 0) { rc = 19; goto done; }

        /* 客户端收 Initial（SH） */
        uint8_t b2[1024]; if (pk2_len > sizeof(b2)) { rc = 20; goto done; }
        memcpy(b2, pk2, pk2_len);
        const uint8_t* fr2 = NULL; size_t fl2 = 0; uint8_t ty2 = 0;
        if (pkt_parse_long(b2, pk2_len, &si_keys, 1, &fr2, &fl2, &ty2) != 0) { rc = 21; goto done; }
        uint64_t o2 = 0; const uint8_t* cd2 = NULL; size_t cdl2 = 0, cc2 = 0;
        if (crypto_frame_read(fr2, fl2, &o2, &cd2, &cdl2, &cc2) != 0) { rc = 22; goto done; }
        if (nl_tls13_client_handshake_1(cli, cd2, cdl2) != 0) { rc = 23; goto done; }

        /* 客户端收 Handshake（其余） */
        nl_quic_keys_t c_hr, c_hw;
        if (nl_tls13_conn_get_handshake_keys(cli, &c_hr, &c_hw) != 0) { rc = 24; goto done; }
        uint8_t b3[2048]; if (pk3_len > sizeof(b3)) { rc = 25; goto done; }
        memcpy(b3, pk3, pk3_len);
        const uint8_t* fr3 = NULL; size_t fl3 = 0; uint8_t ty3 = 0;
        if (pkt_parse_long(b3, pk3_len, &c_hr, 0, &fr3, &fl3, &ty3) != 0) { rc = 26; goto done; }
        uint64_t o3 = 0; const uint8_t* cd3 = NULL; size_t cdl3 = 0, cc3 = 0;
        if (crypto_frame_read(fr3, fl3, &o3, &cd3, &cdl3, &cc3) != 0) { rc = 27; goto done; }
        uint8_t cf[128]; size_t cf_len = 0;
        if (nl_tls13_client_handshake_2(cli, cd3, cdl3, cf, sizeof(cf), &cf_len) != 0) { rc = 28; goto done; }
        if (!nl_tls13_conn_established(cli) || !nl_tls13_conn_peer_verified(cli)) { rc = 29; goto done; }

        /* 客户端 Handshake：client Finished */
        size_t f4 = 0;
        if (crypto_frame_write(frames, sizeof(frames), 0, cf, cf_len, &f4) != 0) { rc = 30; goto done; }
        uint8_t pk4[512]; size_t pk4_len = 0;
        if (pkt_build_long(2, CSID, sizeof(CSID), SSID, sizeof(SSID), 1, 1,
                           frames, f4, &c_hw, pk4, sizeof(pk4), &pk4_len) != 0) { rc = 31; goto done; }

        /* 服务端收 Handshake（client Finished） */
        uint8_t b4[512]; if (pk4_len > sizeof(b4)) { rc = 32; goto done; }
        memcpy(b4, pk4, pk4_len);
        const uint8_t* fr4 = NULL; size_t fl4 = 0; uint8_t ty4 = 0;
        if (pkt_parse_long(b4, pk4_len, &s_hr, 1, &fr4, &fl4, &ty4) != 0) { rc = 33; goto done; }
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
        if (pkt_build_short(0, 1, fbuf, fp, &c_write, pk5, sizeof(pk5), &pk5_len) != 0) { rc = 48; goto done; }

        /* 服务端收 1-RTT，解析请求 */
        uint8_t b5[1200]; if (pk5_len > sizeof(b5)) { rc = 49; goto done; }
        memcpy(b5, pk5, pk5_len);
        const uint8_t* fr5 = NULL; size_t fl5 = 0;
        if (pkt_parse_short(b5, pk5_len, &s_read, 0, &fr5, &fl5) != 0) { rc = 50; goto done; }

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
        if (pkt_build_short(0, 1, rsf, rsf_len, &s_write, pk6, sizeof(pk6), &pk6_len) != 0) { rc = 56; goto done; }

        /* 客户端收 1-RTT 响应 */
        uint8_t b6[512]; if (pk6_len > sizeof(b6)) { rc = 57; goto done; }
        memcpy(b6, pk6, pk6_len);
        const uint8_t* fr6 = NULL; size_t fl6 = 0;
        if (pkt_parse_short(b6, pk6_len, &c_read, 0, &fr6, &fl6) != 0) { rc = 58; goto done; }

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
        if (pkt_build_short(0x1234, 2, fr, 4, &ci_keys, pk, sizeof(pk), &pkl) != 0) { rc = 70; goto done; }
        uint8_t buf[64]; memcpy(buf, pk, pkl);
        const uint8_t* ff = NULL; size_t fl = 0;
        if (pkt_parse_short(buf, pkl, &ci_keys, 0x1234, &ff, &fl) != 0) { rc = 71; goto done; }
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
 * stream_frame_/h3_frame_/qpack_ 系列）与 netleaf_quictls.h 的 TLS 13 状态机公共 API，
 * 在 POSIX（socket/pthread）与 Windows（winsock2/线程 API）上通过薄可移植层实现同一套
 * 可端到端跑通的 happy-path：
 *   QUIC 握手（Initial CH/SH + Handshake 其余）+ 1-RTT 上一次 H3 GET 请求/响应。
 * 说明：假设传输有序无丢包（未实现 ACK/重传/拥塞/QPACK 动态表）。
 * ============================================================ */

/* 固定连接 ID（客户端 DCID / 客户端 SCID / 服务端 SCID）；Initial 密钥由 DCID 派生 */
#define NLH3_DCID_LEN 8
#define NLH3_MAX_PKT  4096
#define NLH3_MAX_CONNS 16   /* 服务端按对端地址分槽的连接表容量 */

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

/* 单条连接槽位：按对端地址（sockaddr_in）区分，保存该连接的握手/1-RTT 状态 */
typedef struct nlh3_conn_slot {
    int valid;
    struct sockaddr_in peer;
    nl_tls13_conn_t* conn;    /* 该连接的 TLS 13 状态机 */
    nl_quic_keys_t ci_keys;   /* 客户端 Initial（服务端读 CH） */
    nl_quic_keys_t si_keys;   /* 服务端 Initial（服务端写 SH） */
    nl_quic_keys_t hs_read, hs_write;
    nl_quic_keys_t rtt_read, rtt_write;
    uint64_t rx_init_pn, rx_hs_pn, rx_rtt_pn;
    uint64_t tx_init_pn, tx_hs_pn, tx_rtt_pn;
    int established;          /* 握手完成、已取得 1-RTT 密钥标志 */
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

/* 为新对端地址分配一个空闲槽位 */
static nlh3_conn_slot_t* nlh3_slot_alloc(nlh3_server_t* s, const struct sockaddr_in* peer) {
    for (int i = 0; i < NLH3_MAX_CONNS; i++) {
        nlh3_conn_slot_t* sl = &s->slots[i];
        if (!sl->valid) {
            memset(sl, 0, sizeof(*sl));
            sl->valid = 1;
            sl->peer = *peer;
            return sl;
        }
    }
    return NULL;
}

/* 处理一条 UDP 报文：按对端地址定位槽位（不存在且为 Initial(CH) 时建槽），
 * 再按 Initial / Handshake / 1-RTT 顺序尝试解析。
 * 返回 1 表示已按某加密级别处理，0 表示不属于任何连接，-1 表示处理中出错。 */
static int nlh3_server_handle(nlh3_server_t* s, const uint8_t* dg, size_t dlen,
                              const struct sockaddr_in* peer) {
    uint8_t buf[NLH3_MAX_PKT];
    nlh3_conn_slot_t* sl;

    if (dlen > sizeof(buf)) return -1;

    sl = nlh3_slot_find(s, peer);

    /* 1) 客户端 Initial：CRYPTO 内含 ClientHello（新连接在此建槽） */
    memcpy(buf, dg, dlen);
    {
        nl_quic_keys_t ci, si;
        if (sl) {
            ci = sl->ci_keys; si = sl->si_keys;
        } else if (nl_quic_initial_keys(NLH3_DCID, NLH3_DCID_LEN, &ci, &si) != 0) {
            return -1;
        }
        const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
        if (pkt_parse_long(buf, dlen, &ci, sl ? sl->rx_init_pn : 0, &fr, &fl, &ty) == 0 && ty == 0) {
            uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, cons = 0;
            if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &cons) == 0 && off == 0) {
                uint8_t flight[NLH3_MAX_PKT]; size_t flight_len = 0;
                uint8_t cf[NLH3_MAX_PKT]; size_t cfl = 0;
                uint8_t out[NLH3_MAX_PKT]; size_t outl = 0;
                size_t sh_len = 0;

                if (!sl) {                        /* 新连接：建槽并保存 Initial 密钥 */
                    sl = nlh3_slot_alloc(s, peer);
                    if (!sl) return -1;
                    sl->ci_keys = ci; sl->si_keys = si;
                }
                if (!sl->conn) {
                    sl->conn = nl_tls13_conn_new(1);
                    if (!sl->conn) return -1;
                    if (nl_tls13_conn_set_cert(sl->conn, s->cert_pem, s->key_pem) != 0) return -1;
                }
                if (nl_tls13_server_handshake(sl->conn, cd, cdl, flight, sizeof(flight), &flight_len) != 0)
                    return -1;
                if (nl_tls13_conn_get_handshake_keys(sl->conn, &sl->hs_read, &sl->hs_write) != 0)
                    return -1;
                if (nl_tls13_parse_handshake(flight, flight_len, NULL, NULL, NULL, &sh_len) != 0)
                    return -1;

                /* ServerHello → Initial 包 */
                if (crypto_frame_write(cf, sizeof(cf), 0, flight, sh_len, &cfl) != 0) return -1;
                if (pkt_build_long(0, NLH3_CSID, NLH3_DCID_LEN, NLH3_SSID, NLH3_DCID_LEN,
                                   sl->tx_init_pn, 1, cf, cfl, &sl->si_keys,
                                   out, sizeof(out), &outl) != 0) return -1;
                nlh3_send_to(s, out, outl, peer);
                sl->tx_init_pn++;

                /* EE/Cert/CV/Fin → Handshake 包 */
                {
                    const uint8_t* rest = flight + sh_len;
                    size_t rest_len = flight_len - sh_len;
                    if (crypto_frame_write(cf, sizeof(cf), 0, rest, rest_len, &cfl) != 0) return -1;
                    if (pkt_build_long(2, NLH3_CSID, NLH3_DCID_LEN, NLH3_SSID, NLH3_DCID_LEN,
                                       sl->tx_hs_pn, 1, cf, cfl, &sl->hs_write,
                                       out, sizeof(out), &outl) != 0) return -1;
                    nlh3_send_to(s, out, outl, peer);
                    sl->tx_hs_pn++;
                }
                sl->rx_init_pn++;
                return 1;
            }
        }
    }

    if (!sl) return 0;

    /* 2) 服务端握手读密钥：CRYPTO 内含 client Finished */
    if (sl->conn) {
        memcpy(buf, dg, dlen);
        const uint8_t* fr = NULL; size_t fl = 0; uint8_t ty = 0;
        if (pkt_parse_long(buf, dlen, &sl->hs_read, sl->rx_hs_pn, &fr, &fl, &ty) == 0 && ty == 2) {
            uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, cons = 0;
            if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &cons) == 0 && off == 0) {
                if (nl_tls13_server_finish(sl->conn, cd, cdl) != 0) return -1;
                if (nl_tls13_conn_get_1rtt_keys(sl->conn, &sl->rtt_read, &sl->rtt_write) != 0)
                    return -1;
                sl->established = 1;
                sl->rx_hs_pn++;
                return 1;
            }
        }
    }

    /* 3) 服务端 1-RTT 读密钥：STREAM 帧内的 H3 请求 */
    if (sl->conn && sl->established) {
        memcpy(buf, dg, dlen);
        const uint8_t* fr = NULL; size_t fl = 0;
        if (pkt_parse_short(buf, dlen, &sl->rtt_read, sl->rx_rtt_pn, &fr, &fl) == 0) {
            char method[16] = {0}, path[512] = {0}, authority[256] = {0};
            int got_req = 0;
            size_t p = 0;

            while (p < fl) {
                uint64_t sid = 0, soff = 0; const uint8_t* sd = NULL; size_t sdl = 0;
                int fin = 0; size_t cons = 0;
                if (stream_frame_read(fr + p, fl - p, &sid, &soff, &sd, &sdl, &fin, &cons) != 0) break;
                p += cons;
                if (sid == 2) continue;           /* 控制流：忽略 SETTINGS */
                if (sid != 0) continue;           /* 仅处理请求流 0 */
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

            if (got_req) {
                char* body = NULL; size_t blen = 0;
                if (s->handler) s->handler(method, path, authority, s->userdata, &body, &blen);

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
                    {
                        uint8_t rsf[NLH3_MAX_PKT]; size_t rsf_len = 0;
                        uint8_t out[NLH3_MAX_PKT]; size_t outl = 0;
                        if (stream_frame_write(rsf, sizeof(rsf), 0, rstream, rsp, 1, &rsf_len) == 0 &&
                            pkt_build_short(sl->tx_rtt_pn, 1, rsf, rsf_len, &sl->rtt_write,
                                            out, sizeof(out), &outl) == 0) {
                            nlh3_send_to(s, out, outl, peer);
                            sl->tx_rtt_pn++;
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
    while (s->running) {
        struct sockaddr_in peer; nlh3_socklen_t plen = sizeof(peer);
        uint8_t dg[NLH3_MAX_PKT];
        nlh3_ssize_t n = recvfrom(s->fd, dg, sizeof(dg), 0, (struct sockaddr*)&peer, &plen);
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
                           0, 1, fr, fl, &ci, init_pkt, sizeof(init_pkt), &init_plen) != 0) { rc = 8; goto done; }
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
                if (pkt_parse_long(t, (size_t)n, &si, rx_init, &fr, &fl, &ty) == 0 && ty == 0) {
                    uint64_t off = 0; const uint8_t* cd = NULL; size_t cdl = 0, cons = 0;
                    if (crypto_frame_read(fr, fl, &off, &cd, &cdl, &cons) == 0 && off == 0) {
                        if (nl_tls13_client_handshake_1(c, cd, cdl) != 0) { rc = 10; goto done; }
                        if (nl_tls13_conn_get_handshake_keys(c, &c_hs_read, &c_hs_write) != 0) { rc = 11; goto done; }
                        have_hs = 1; got_sh = 1; rx_init++;
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
            if (pkt_parse_long(t, pending_len, &c_hs_read, rx_hs, &fr, &fl, &ty) == 0 && ty == 2) {
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
                if (pkt_parse_long(t, (size_t)n, &c_hs_read, rx_hs, &fr, &fl, &ty) == 0 && ty == 2) {
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
                               0, 1, fr, fl, &c_hs_write, fin_pkt, sizeof(fin_pkt), &fin_plen) != 0) { rc = 17; goto done; }
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
            if (pkt_build_short(0, 1, fbuf, fp, &c_rtt_write, rtt_pkt, sizeof(rtt_pkt), &rtt_plen) != 0) { rc = 27; goto done; }
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
                    if (pkt_parse_short(t, (size_t)n, &c_rtt_read, 0, &fr, &fl) != 0) continue;
                    {
                        size_t p = 0;
                        while (p < fl) {
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
