/**
 * @file quictls_tls.c
 * @brief 自研 QUIC-TLS：TLS 1.3 密钥调度 + 握手消息编解码（C3/C4）
 * @version 0.1.0
 *
 * 覆盖 RFC 8446 §7.1 密钥调度与 RFC 9001 §5.1 交通密钥 → QUIC 包保护密钥映射，
 * 以及 Finished / KeyUpdate / 握手消息（type+length+body）编解码。
 * 参考范式：picotls（TLS 1.3 密钥调度分层）、ngtcp2（QUIC 密钥映射）。
 */

#include "netleaf_quictls.h"
#include "quictls_internal.h"

#include <string.h>

#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

/* ============================================================
 * 基础原语
 * ============================================================ */
int nl_tls13_hkdf_extract(const uint8_t* salt, size_t salt_len,
                          const uint8_t* ikm, size_t ikm_len,
                          uint8_t out[NL_TLS13_HASH_LEN]) {
    return nl_quic__hkdf_extract(salt, salt_len, ikm, ikm_len, out);
}

int nl_tls13_sha256(const uint8_t* data, size_t len, uint8_t out[NL_TLS13_HASH_LEN]) {
    const uint8_t* p = data ? data : (const uint8_t*)"";
    return mbedtls_sha256(p, len, out, 0);
}

/* Derive-Secret(secret, label, transcript_hash)
 * context 直接使用调用方传入的 transcript 哈希（Hash(messages)） */
int nl_tls13_derive_secret(const uint8_t* secret, size_t secret_len,
                           const char* label,
                           const uint8_t* transcript_hash, size_t th_len,
                           uint8_t* out, size_t out_len) {
    return nl_quic__expand_label(secret, secret_len, label,
                                 transcript_hash, th_len, out, out_len);
}

/* ============================================================
 * 密钥调度：握手秘密 / master / 应用秘密
 * ============================================================ */
int nl_tls13_handshake_secrets(const uint8_t* ecdhe, size_t ecdhe_len,
                               const uint8_t* th_ch_sh, size_t th_len,
                               uint8_t c_hs[NL_TLS13_SECRET_LEN],
                               uint8_t s_hs[NL_TLS13_SECRET_LEN],
                               uint8_t master[NL_TLS13_SECRET_LEN]) {
    if (!ecdhe || ecdhe_len == 0 || !th_ch_sh) return -1;

    uint8_t zero[NL_TLS13_HASH_LEN];
    memset(zero, 0, sizeof(zero));

    /* Early Secret = HKDF-Extract(0, 0)（无 PSK） */
    uint8_t early[NL_TLS13_HASH_LEN];
    if (nl_quic__hkdf_extract(zero, sizeof(zero), zero, sizeof(zero), early) != 0) return -1;

    uint8_t empty_hash[NL_TLS13_HASH_LEN];
    if (nl_tls13_sha256(NULL, 0, empty_hash) != 0) return -1;

    /* derived = Derive-Secret(Early, "derived", "") */
    uint8_t derived1[NL_TLS13_HASH_LEN];
    if (nl_quic__expand_label(early, sizeof(early), "derived",
                              empty_hash, sizeof(empty_hash), derived1, sizeof(derived1)) != 0) return -1;

    /* Handshake Secret = HKDF-Extract(derived, ECDHE) */
    uint8_t hs[NL_TLS13_HASH_LEN];
    if (nl_quic__hkdf_extract(derived1, sizeof(derived1), ecdhe, ecdhe_len, hs) != 0) return -1;

    if (nl_quic__expand_label(hs, sizeof(hs), "c hs traffic", th_ch_sh, th_len,
                              c_hs, NL_TLS13_SECRET_LEN) != 0) return -1;
    if (nl_quic__expand_label(hs, sizeof(hs), "s hs traffic", th_ch_sh, th_len,
                              s_hs, NL_TLS13_SECRET_LEN) != 0) return -1;

    /* derived2 = Derive-Secret(Handshake, "derived", "") */
    uint8_t derived2[NL_TLS13_HASH_LEN];
    if (nl_quic__expand_label(hs, sizeof(hs), "derived",
                              empty_hash, sizeof(empty_hash), derived2, sizeof(derived2)) != 0) return -1;

    /* Master Secret = HKDF-Extract(derived2, 0) */
    if (nl_quic__hkdf_extract(derived2, sizeof(derived2), zero, sizeof(zero), master) != 0) return -1;
    return 0;
}

int nl_tls13_application_secrets(const uint8_t* master, size_t master_len,
                                 const uint8_t* th_ch_sfin, size_t th_len,
                                 uint8_t c_ap0[NL_TLS13_SECRET_LEN],
                                 uint8_t s_ap0[NL_TLS13_SECRET_LEN]) {
    if (!master || !th_ch_sfin) return -1;
    if (nl_quic__expand_label(master, master_len, "c ap traffic", th_ch_sfin, th_len,
                              c_ap0, NL_TLS13_SECRET_LEN) != 0) return -1;
    if (nl_quic__expand_label(master, master_len, "s ap traffic", th_ch_sfin, th_len,
                              s_ap0, NL_TLS13_SECRET_LEN) != 0) return -1;
    return 0;
}

int nl_tls13_secret_to_quic_keys(const uint8_t* secret, size_t secret_len,
                                 unsigned cipher_suite, nl_quic_keys_t* out) {
    if (!secret || !out) return -1;
    memset(out, 0, sizeof(*out));

    size_t kl;
    int chacha = 0;
    switch (cipher_suite) {
        case NL_QUIC_CIPHER_CHACHA20_POLY1305_SHA256: kl = 32; chacha = 1; break;
        case NL_QUIC_CIPHER_AES256_GCM_SHA384:        kl = 32; chacha = 0; break;
        case NL_QUIC_CIPHER_AES128_GCM_SHA256:
        default:                                      kl = 16; chacha = 0; break;
    }

    if (nl_quic__expand_label(secret, secret_len, "quic key", NULL, 0, out->key, kl) != 0) return -1;
    out->key_len = kl;
    if (nl_quic__expand_label(secret, secret_len, "quic iv", NULL, 0, out->iv, NL_QUIC_IV_LEN) != 0) return -1;
    if (nl_quic__expand_label(secret, secret_len, "quic hp", NULL, 0, out->hp, kl) != 0) return -1;
    out->hp_len = kl;
    out->is_chacha = chacha;
    return 0;
}

/* ============================================================
 * Finished / KeyUpdate
 * ============================================================ */
int nl_tls13_finished_key(const uint8_t* base_secret, size_t len,
                          uint8_t out[NL_TLS13_HASH_LEN]) {
    if (!base_secret || !out) return -1;
    return nl_quic__expand_label(base_secret, len, "finished", NULL, 0, out, NL_TLS13_HASH_LEN);
}

int nl_tls13_finished_verify_data(const uint8_t* finished_key,
                                  const uint8_t* transcript_hash, size_t th_len,
                                  uint8_t out[NL_TLS13_HASH_LEN]) {
    if (!finished_key || !transcript_hash || !out) return -1;
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    /* verify_data = HMAC(finished_key, Transcript-Hash) */
    return mbedtls_md_hmac(md, finished_key, NL_TLS13_HASH_LEN,
                           transcript_hash, th_len, out);
}

int nl_tls13_update_traffic_secret(uint8_t secret[NL_TLS13_SECRET_LEN]) {
    if (!secret) return -1;
    uint8_t next[NL_TLS13_SECRET_LEN];
    if (nl_quic__expand_label(secret, NL_TLS13_SECRET_LEN, "traffic upd",
                              NULL, 0, next, sizeof(next)) != 0) return -1;
    memcpy(secret, next, sizeof(next));
    return 0;
}

/* ============================================================
 * 握手消息编解码（type(1) + length(3) + body）
 * ============================================================ */
int nl_tls13_encode_handshake(uint8_t msg_type,
                              const uint8_t* body, size_t body_len,
                              uint8_t* out, size_t cap, size_t* out_len) {
    if (!out || body_len > 0xffffffu) return -1;
    if (cap < 4 + body_len) return -1;
    out[0] = msg_type;
    out[1] = (uint8_t)((body_len >> 16) & 0xff);
    out[2] = (uint8_t)((body_len >> 8) & 0xff);
    out[3] = (uint8_t)(body_len & 0xff);
    if (body_len && body) memcpy(out + 4, body, body_len);
    if (out_len) *out_len = 4 + body_len;
    return 0;
}

int nl_tls13_parse_handshake(const uint8_t* data, size_t len,
                             uint8_t* msg_type,
                             const uint8_t** body, size_t* body_len,
                             size_t* consumed) {
    if (!data || len < 4) return -1;
    size_t blen = ((size_t)data[1] << 16) | ((size_t)data[2] << 8) | (size_t)data[3];
    if (4 + blen > len) return -1;
    if (msg_type) *msg_type = data[0];
    if (body) *body = data + 4;
    if (body_len) *body_len = blen;
    if (consumed) *consumed = 4 + blen;
    return 0;
}

/* ============================================================
 * C3/C4 自测（结构一致性：派生链 / 密钥映射 / 往返 / KeyUpdate）
 * ============================================================ */
static int tls13_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    return memcmp(a, b, n) == 0;
}

int nl_quictls_tls13_selftest(void) {
    uint8_t ecdhe[32], th[32];
    for (size_t i = 0; i < 32; i++) { ecdhe[i] = (uint8_t)(0x01 + i); th[i] = (uint8_t)(0xa0 + i); }

    uint8_t c_hs[32], s_hs[32], master[32];
    if (nl_tls13_handshake_secrets(ecdhe, sizeof(ecdhe), th, sizeof(th),
                                   c_hs, s_hs, master) != 0) return 1;
    if (tls13_eq(c_hs, s_hs, 32)) return 2;                 /* c/s 应不同 */
    if (tls13_eq(c_hs, master, 32)) return 3;

    /* 握手秘密 → QUIC 密钥（AES128 / ChaCha）并做包保护往返 */
    nl_quic_keys_t ka, kc;
    if (nl_tls13_secret_to_quic_keys(c_hs, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &ka) != 0) return 4;
    if (ka.key_len != 16 || ka.is_chacha) return 5;
    if (nl_tls13_secret_to_quic_keys(c_hs, 32, NL_QUIC_CIPHER_CHACHA20_POLY1305_SHA256, &kc) != 0) return 6;
    if (kc.key_len != 32 || !kc.is_chacha) return 7;

    uint8_t pkt[128];
    memset(pkt, 0, sizeof(pkt));
    size_t po = 10, pl = 1;
    pkt[0] = 0xc3; pkt[po] = 0x03;
    size_t ph = po + pl;
    for (size_t i = 0; i < 24; i++) pkt[ph + i] = (uint8_t)(0x55 + i);
    uint8_t saved[24]; memcpy(saved, pkt + ph, 24);
    size_t tot = 0;
    if (nl_quic_protect_packet_ex(&kc, 3, 1, pkt, po, pl, 24, &tot) != 0) return 8;
    size_t pln = 0;
    if (nl_quic_unprotect_packet_ex(&kc, 3, 1, pkt, po, pl, 24, &pln) != 0) return 9;
    if (pln != 24 || !tls13_eq(pkt + ph, saved, 24)) return 10;

    /* 应用秘密 + Finished */
    uint8_t c_ap0[32], s_ap0[32];
    if (nl_tls13_application_secrets(master, sizeof(master), th, sizeof(th), c_ap0, s_ap0) != 0) return 11;
    if (tls13_eq(c_ap0, s_ap0, 32)) return 12;
    if (tls13_eq(c_ap0, c_hs, 32)) return 13;

    uint8_t fk[32], vd1[32], vd2[32];
    if (nl_tls13_finished_key(s_hs, 32, fk) != 0) return 14;
    if (nl_tls13_finished_verify_data(fk, th, 32, vd1) != 0) return 15;
    if (nl_tls13_finished_verify_data(fk, th, 32, vd2) != 0) return 16;
    if (!tls13_eq(vd1, vd2, 32)) return 17;                 /* 确定性 */

    /* KeyUpdate：秘密应改变且可复现 */
    uint8_t ap_copy[32]; memcpy(ap_copy, c_ap0, 32);
    if (nl_tls13_update_traffic_secret(ap_copy) != 0) return 18;
    if (tls13_eq(ap_copy, c_ap0, 32)) return 19;
    uint8_t ap_copy2[32]; memcpy(ap_copy2, c_ap0, 32);
    nl_tls13_update_traffic_secret(ap_copy2);
    if (!tls13_eq(ap_copy, ap_copy2, 32)) return 20;

    /* 握手消息编解码往返 */
    uint8_t body[5] = {1, 2, 3, 4, 5};
    uint8_t enc[32];
    size_t enc_len = 0;
    if (nl_tls13_encode_handshake(NL_TLS13_HS_FINISHED, body, sizeof(body), enc, sizeof(enc), &enc_len) != 0) return 21;
    if (enc_len != 9 || enc[0] != NL_TLS13_HS_FINISHED) return 22;
    uint8_t mt = 0; const uint8_t* pb = NULL; size_t pbl = 0, cons = 0;
    if (nl_tls13_parse_handshake(enc, enc_len, &mt, &pb, &pbl, &cons) != 0) return 23;
    if (mt != NL_TLS13_HS_FINISHED || pbl != sizeof(body) || cons != enc_len) return 24;
    if (!tls13_eq(pb, body, sizeof(body))) return 25;

    return 0;
}
