/**
 * @file netleaf_quictls.c
 * @brief 自研 QUIC-TLS（RFC 9001）扩展实现
 * @version 0.1.0
 *
 * 在 mbedTLS 原语（HMAC-SHA256 / AES-GCM / AES-ECB）之上自研实现：
 *   - HKDF-Extract / HKDF-Expand / HKDF-Expand-Label（RFC 8446 §7.1）
 *   - QUIC v1 Initial 密钥派生（RFC 9001 §5.2）
 *   - 长首部包 AEAD 保护 + 首部保护 / 解保护（RFC 9001 §5.3/§5.4）
 *
 * 设计范式参考 picotls / quicly（h2o）、ngtcp2、lsquic 的密钥调度与包保护分层。
 * 不依赖 mbedTLS 的 TLS 记录层（mbedTLS 3.6 无 QUIC API）。
 */

#include "netleaf_quictls.h"
#include "quictls_internal.h"

#include <string.h>

#include <mbedtls/md.h>
#include <mbedtls/aes.h>
#include <mbedtls/gcm.h>
#include <mbedtls/chacha20.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/cipher.h>

#define NL_QUIC_HASH_LEN 32   /* SHA-256 */

/* QUIC v1 Initial salt（RFC 9001 §5.2） */
static const uint8_t NL_QUIC_V1_SALT[20] = {
    0x38, 0x76, 0x2c, 0xf7, 0xf5, 0x59, 0x34, 0xb3, 0x4d, 0x17,
    0x9a, 0xe6, 0xa4, 0xc8, 0x0c, 0xad, 0xcc, 0xbb, 0x7f, 0x0a
};

/* ============================================================
 * HKDF（基于 HMAC-SHA256，避免依赖 MBEDTLS_HKDF_C）
 * ============================================================ */
int nl_quic__hkdf_extract(const uint8_t* salt, size_t salt_len,
                          const uint8_t* ikm, size_t ikm_len,
                          uint8_t out[NL_QUIC_HASH_LEN]) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    return mbedtls_md_hmac(md, salt, salt_len, ikm, ikm_len, out);
}

int nl_quic__hkdf_expand(const uint8_t* prk, size_t prk_len,
                         const uint8_t* info, size_t info_len,
                         uint8_t* out, size_t out_len) {
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return -1;
    if (out_len > 255u * NL_QUIC_HASH_LEN) return -1;

    uint8_t t[NL_QUIC_HASH_LEN];
    size_t  t_len = 0;
    size_t  pos = 0;
    size_t  n = (out_len + NL_QUIC_HASH_LEN - 1) / NL_QUIC_HASH_LEN;

    for (size_t i = 1; i <= n; i++) {
        uint8_t buf[NL_QUIC_HASH_LEN + 512 + 1];
        size_t  bl = 0;
        if (t_len) { memcpy(buf, t, t_len); bl += t_len; }
        if (info_len) {
            if (info_len > 512) return -1;
            memcpy(buf + bl, info, info_len); bl += info_len;
        }
        buf[bl++] = (uint8_t)i;
        if (mbedtls_md_hmac(md, prk, prk_len, buf, bl, t) != 0) return -1;
        t_len = NL_QUIC_HASH_LEN;

        size_t cpy = out_len - pos;
        if (cpy > NL_QUIC_HASH_LEN) cpy = NL_QUIC_HASH_LEN;
        memcpy(out + pos, t, cpy);
        pos += cpy;
    }
    return 0;
}

/* HKDF-Expand-Label：label 不含 "tls13 " 前缀，本函数自动补齐 */
int nl_quic__expand_label(const uint8_t* secret, size_t secret_len,
                          const char* label,
                          const uint8_t* context, size_t context_len,
                          uint8_t* out, size_t out_len) {
    if (!secret || !label) return -1;
    size_t lab_len = strlen(label);
    size_t full_len = 6 + lab_len;          /* "tls13 " + Label */
    if (full_len > 255 || context_len > 255 || out_len > 0xffff) return -1;

    uint8_t info[2 + 1 + 6 + 128 + 1 + 255];
    size_t  p = 0;
    info[p++] = (uint8_t)((out_len >> 8) & 0xff);
    info[p++] = (uint8_t)(out_len & 0xff);
    info[p++] = (uint8_t)full_len;
    memcpy(info + p, "tls13 ", 6); p += 6;
    memcpy(info + p, label, lab_len); p += lab_len;
    info[p++] = (uint8_t)context_len;
    if (context_len) { memcpy(info + p, context, context_len); p += context_len; }

    return nl_quic__hkdf_expand(secret, secret_len, info, p, out, out_len);
}

int nl_quic_hkdf_expand_label(const uint8_t* secret, size_t secret_len,
                              const char* label,
                              const uint8_t* context, size_t context_len,
                              uint8_t* out, size_t out_len) {
    return nl_quic__expand_label(secret, secret_len, label, context, context_len, out, out_len);
}

/* ============================================================
 * Initial 密钥派生（RFC 9001 §5.2）
 * ============================================================ */
static int quic_derive_keyset(const uint8_t* secret, size_t secret_len,
                              nl_quic_keys_t* out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (nl_quic__expand_label(secret, secret_len, "quic key", NULL, 0, out->key, 16) != 0) return -1;
    out->key_len = 16;
    if (nl_quic__expand_label(secret, secret_len, "quic iv", NULL, 0, out->iv, NL_QUIC_IV_LEN) != 0) return -1;
    if (nl_quic__expand_label(secret, secret_len, "quic hp", NULL, 0, out->hp, 16) != 0) return -1;
    out->hp_len = 16;
    out->is_chacha = 0;
    return 0;
}

int nl_quic_initial_keys(const uint8_t* dcid, size_t dcid_len,
                         nl_quic_keys_t* client_keys, nl_quic_keys_t* server_keys) {
    if (!dcid || dcid_len == 0) return -1;

    uint8_t initial_secret[NL_QUIC_HASH_LEN];
    if (nl_quic__hkdf_extract(NL_QUIC_V1_SALT, sizeof(NL_QUIC_V1_SALT),
                          dcid, dcid_len, initial_secret) != 0) return -1;

    uint8_t csecret[NL_QUIC_HASH_LEN], ssecret[NL_QUIC_HASH_LEN];
    if (nl_quic__expand_label(initial_secret, NL_QUIC_HASH_LEN, "client in", NULL, 0,
                          csecret, NL_QUIC_HASH_LEN) != 0) return -1;
    if (nl_quic__expand_label(initial_secret, NL_QUIC_HASH_LEN, "server in", NULL, 0,
                          ssecret, NL_QUIC_HASH_LEN) != 0) return -1;

    if (client_keys && quic_derive_keyset(csecret, NL_QUIC_HASH_LEN, client_keys) != 0) return -1;
    if (server_keys && quic_derive_keyset(ssecret, NL_QUIC_HASH_LEN, server_keys) != 0) return -1;
    return 0;
}

/* ============================================================
 * 首部保护（AES-ECB）
 * ============================================================ */
static int quic_aes_ecb(const uint8_t* key, size_t key_len,
                        const uint8_t in[16], uint8_t out[16]) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, key, (unsigned)(key_len * 8));
    if (rc == 0) rc = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&aes);
    return rc;
}

/* ChaCha20 首部保护掩码（RFC 9001 §5.4.4）：counter=sample[0..3] LE，nonce=sample[4..15] */
static int quic_chacha20_hp_mask(const uint8_t* key, size_t key_len,
                                 const uint8_t sample[16], uint8_t out[5]) {
    if (key_len < 32) return -1;
    uint32_t counter = (uint32_t)sample[0] | ((uint32_t)sample[1] << 8) |
                       ((uint32_t)sample[2] << 16) | ((uint32_t)sample[3] << 24);
    uint8_t zeros[5];
    memset(zeros, 0, sizeof(zeros));
    uint8_t ks[5];
    if (mbedtls_chacha20_crypt(key, sample + 4, counter, sizeof(zeros), zeros, ks) != 0)
        return -1;
    memcpy(out, ks, 5);
    return 0;
}

/* 计算首部保护掩码（前 5 字节） */
static int quic_hp_mask(const nl_quic_keys_t* keys, const uint8_t sample[16], uint8_t mask[5]) {
    if (keys->is_chacha) {
        return quic_chacha20_hp_mask(keys->hp, keys->hp_len, sample, mask);
    }
    uint8_t full[16];
    if (quic_aes_ecb(keys->hp, keys->hp_len, sample, full) != 0) return -1;
    memcpy(mask, full, 5);
    return 0;
}

/* 施加/解除首部保护掩码（首字节低 4/5 位 + pn_len 个 PN 字节；异或自反） */
static int quic_hp_apply(const nl_quic_keys_t* keys,
                         uint8_t* pkt, size_t pn_offset, size_t pn_len,
                         size_t total_len, int long_header) {
    size_t sample_off = pn_offset + 4;
    if (sample_off + 16 > total_len) return -1;
    uint8_t mask[5];
    if (quic_hp_mask(keys, pkt + sample_off, mask) != 0) return -1;

    pkt[0] ^= (uint8_t)(mask[0] & (long_header ? 0x0f : 0x1f));
    for (size_t i = 0; i < pn_len; i++) pkt[pn_offset + i] ^= mask[1 + i];
    return 0;
}

/* 解除首部保护并自动读取 pn_len：先掩码首字节以还原 pn_len 位，再掩码 PN 字节 */
static int quic_hp_remove_auto(const nl_quic_keys_t* keys,
                               uint8_t* pkt, size_t pn_offset, size_t total_len,
                               int long_header, size_t* out_pn_len) {
    size_t sample_off = pn_offset + 4;
    if (sample_off + 16 > total_len) return -1;
    uint8_t mask[5];
    if (quic_hp_mask(keys, pkt + sample_off, mask) != 0) return -1;

    pkt[0] ^= (uint8_t)(mask[0] & (long_header ? 0x0f : 0x1f));
    size_t pn_len = (size_t)(pkt[0] & 0x03) + 1;
    for (size_t i = 0; i < pn_len; i++) pkt[pn_offset + i] ^= mask[1 + i];
    if (out_pn_len) *out_pn_len = pn_len;
    return 0;
}

static void quic_build_nonce(const nl_quic_keys_t* keys, uint64_t pn, uint8_t nonce[NL_QUIC_IV_LEN]) {
    memcpy(nonce, keys->iv, NL_QUIC_IV_LEN);
    for (int i = 0; i < 8; i++)
        nonce[NL_QUIC_IV_LEN - 1 - i] ^= (uint8_t)(pn >> (8 * i));
}

/* ============================================================
 * AEAD（AES-GCM / ChaCha20-Poly1305）
 * ============================================================ */
static int quic_aead_seal(const nl_quic_keys_t* k, uint64_t pn,
                          const uint8_t* aad, size_t aad_len,
                          uint8_t* buf, size_t len, uint8_t tag[NL_QUIC_TAG_LEN]) {
    uint8_t nonce[NL_QUIC_IV_LEN];
    quic_build_nonce(k, pn, nonce);

    if (k->is_chacha) {
        if (k->key_len < 32) return -1;
        mbedtls_chachapoly_context cp;
        mbedtls_chachapoly_init(&cp);
        int rc = mbedtls_chachapoly_setkey(&cp, k->key);
        if (rc == 0)
            rc = mbedtls_chachapoly_encrypt_and_tag(&cp, len, nonce, aad, aad_len, buf, buf, tag);
        mbedtls_chachapoly_free(&cp);
        return rc;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, k->key,
                           (unsigned)(k->key_len * 8)) != 0) {
        mbedtls_gcm_free(&gcm);
        return -1;
    }
    int rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len,
                                       nonce, NL_QUIC_IV_LEN, aad, aad_len,
                                       buf, buf, NL_QUIC_TAG_LEN, tag);
    mbedtls_gcm_free(&gcm);
    return rc;
}

static int quic_aead_open(const nl_quic_keys_t* k, uint64_t pn,
                          const uint8_t* aad, size_t aad_len,
                          uint8_t* buf, size_t len, const uint8_t tag[NL_QUIC_TAG_LEN]) {
    uint8_t nonce[NL_QUIC_IV_LEN];
    quic_build_nonce(k, pn, nonce);

    if (k->is_chacha) {
        if (k->key_len < 32) return -1;
        mbedtls_chachapoly_context cp;
        mbedtls_chachapoly_init(&cp);
        int rc = mbedtls_chachapoly_setkey(&cp, k->key);
        if (rc == 0)
            rc = mbedtls_chachapoly_auth_decrypt(&cp, len, nonce, aad, aad_len, tag, buf, buf);
        mbedtls_chachapoly_free(&cp);
        return rc;
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, k->key,
                           (unsigned)(k->key_len * 8)) != 0) {
        mbedtls_gcm_free(&gcm);
        return -1;
    }
    int rc = mbedtls_gcm_auth_decrypt(&gcm, len, nonce, NL_QUIC_IV_LEN,
                                      aad, aad_len, tag, NL_QUIC_TAG_LEN, buf, buf);
    mbedtls_gcm_free(&gcm);
    return rc;
}

/* 分离式接口：先解除首部保护（自动 pn_len），再 AEAD 解密 */
int nl_quic_hp_unprotect(const nl_quic_keys_t* keys, uint8_t* pkt, size_t pn_offset,
                         int long_header, size_t total_len, size_t* out_pn_len) {
    if (!keys || !pkt) return -1;
    return quic_hp_remove_auto(keys, pkt, pn_offset, total_len, long_header, out_pn_len);
}

int nl_quic_unprotect_payload(const nl_quic_keys_t* keys, uint64_t pn,
                              uint8_t* pkt, size_t pn_offset, size_t pn_len,
                              size_t ct_len, size_t* plaintext_len) {
    if (!keys || !pkt) return -1;
    size_t hdr_len = pn_offset + pn_len;
    uint8_t* ct = pkt + hdr_len;
    uint8_t tag[NL_QUIC_TAG_LEN];
    memcpy(tag, ct + ct_len, NL_QUIC_TAG_LEN);
    if (quic_aead_open(keys, pn, pkt, hdr_len, ct, ct_len, tag) != 0) return -1;
    if (plaintext_len) *plaintext_len = ct_len;
    return 0;
}

/* ============================================================
 * 包保护 / 解保护（通用：长/短首部 × AES/ChaCha）
 * ============================================================ */
int nl_quic_protect_packet_ex(const nl_quic_keys_t* keys, uint64_t pn, int long_header,
                              uint8_t* pkt, size_t pn_offset, size_t pn_len,
                              size_t payload_len, size_t* out_len) {
    if (!keys || !pkt || pn_len == 0 || pn_len > 4) return -1;
    if (keys->key_len == 0 || keys->hp_len == 0) return -1;

    size_t hdr_len = pn_offset + pn_len;             /* AAD = 未保护首部（含 PN） */
    uint8_t* payload = pkt + hdr_len;
    uint8_t tag[NL_QUIC_TAG_LEN];

    if (quic_aead_seal(keys, pn, pkt, hdr_len, payload, payload_len, tag) != 0) return -1;
    memcpy(payload + payload_len, tag, NL_QUIC_TAG_LEN);

    size_t total = hdr_len + payload_len + NL_QUIC_TAG_LEN;
    if (quic_hp_apply(keys, pkt, pn_offset, pn_len, total, long_header) != 0) return -1;
    if (out_len) *out_len = total;
    return 0;
}

int nl_quic_unprotect_packet_ex(const nl_quic_keys_t* keys, uint64_t pn, int long_header,
                                uint8_t* pkt, size_t pn_offset, size_t pn_len,
                                size_t ct_len, size_t* plaintext_len) {
    if (!keys || !pkt || pn_len == 0 || pn_len > 4) return -1;
    if (keys->key_len == 0 || keys->hp_len == 0) return -1;

    size_t total = pn_offset + pn_len + ct_len + NL_QUIC_TAG_LEN;
    if (quic_hp_apply(keys, pkt, pn_offset, pn_len, total, long_header) != 0) return -1;

    size_t hdr_len = pn_offset + pn_len;
    uint8_t* ct = pkt + hdr_len;
    uint8_t tag[NL_QUIC_TAG_LEN];
    memcpy(tag, ct + ct_len, NL_QUIC_TAG_LEN);

    if (quic_aead_open(keys, pn, pkt, hdr_len, ct, ct_len, tag) != 0) return -1;
    if (plaintext_len) *plaintext_len = ct_len;
    return 0;
}

/* 兼容旧接口：默认长首部 */
int nl_quic_protect_packet(const nl_quic_keys_t* keys, uint64_t pn,
                           uint8_t* pkt, size_t pn_offset, size_t pn_len,
                           size_t payload_len, size_t* out_len) {
    return nl_quic_protect_packet_ex(keys, pn, 1, pkt, pn_offset, pn_len, payload_len, out_len);
}

int nl_quic_unprotect_packet(const nl_quic_keys_t* keys, uint64_t pn,
                             uint8_t* pkt, size_t pn_offset, size_t pn_len,
                             size_t ct_len, size_t* plaintext_len) {
    return nl_quic_unprotect_packet_ex(keys, pn, 1, pkt, pn_offset, pn_len, ct_len, plaintext_len);
}

/* ============================================================
 * 包号编码 / 解码（RFC 9000 §17.1 / §A.3）
 * ============================================================ */
int nl_quic_pn_encode(uint64_t full_pn, uint64_t largest_acked,
                      uint8_t* out, size_t* out_len) {
    if (!out || !out_len) return -1;
    uint64_t range = (full_pn > largest_acked) ? (full_pn - largest_acked) : 1;
    size_t nlen;
    if (range < 0x80ULL)          nlen = 1;
    else if (range < 0x8000ULL)   nlen = 2;
    else if (range < 0x800000ULL) nlen = 3;
    else                          nlen = 4;
    for (size_t i = 0; i < nlen; i++)
        out[i] = (uint8_t)(full_pn >> (8 * (nlen - 1 - i)));
    *out_len = nlen;
    return 0;
}

uint64_t nl_quic_pn_decode(uint64_t largest_pn, uint64_t truncated, size_t pn_len) {
    uint64_t expected = largest_pn + 1;
    uint64_t pn_nbits = (uint64_t)pn_len * 8;
    uint64_t pn_win = 1ULL << pn_nbits;
    uint64_t pn_hwin = pn_win / 2;
    uint64_t pn_mask = pn_win - 1;
    uint64_t candidate = (expected & ~pn_mask) | truncated;

    if (candidate + pn_hwin <= expected && candidate + pn_win < (1ULL << 62))
        return candidate + pn_win;
    if (candidate > expected + pn_hwin && candidate >= pn_win)
        return candidate - pn_win;
    return candidate;
}

/* QUIC 密钥更新（RFC 9001 §6，标签 "quic ku"） */
int nl_quic_update_key_secret(uint8_t secret[32]) {
    if (!secret) return -1;
    uint8_t next[32];
    if (nl_quic__expand_label(secret, 32, "quic ku", NULL, 0, next, sizeof(next)) != 0) return -1;
    memcpy(secret, next, sizeof(next));
    return 0;
}

/* 1-RTT 密钥集（quic ku 密钥更新：secret 更新后重新派生 key/iv/hp） */
int nl_quic_keyset_init(nl_quic_keyset_t* ks, const uint8_t secret[32], unsigned suite) {
    if (!ks || !secret) return -1;
    memcpy(ks->secret, secret, 32);
    ks->suite = suite;
    return nl_tls13_secret_to_quic_keys(ks->secret, 32, suite, &ks->keys);
}

int nl_quic_keyset_key_update(nl_quic_keyset_t* ks) {
    if (!ks) return -1;
    if (nl_quic_update_key_secret(ks->secret) != 0) return -1;
    return nl_tls13_secret_to_quic_keys(ks->secret, 32, ks->suite, &ks->keys);
}

/* ============================================================
 * RFC 9001 Appendix A 自测
 * ============================================================ */
static int quic_eq(const uint8_t* a, const uint8_t* b, size_t n) {
    return memcmp(a, b, n) == 0;
}

int nl_quic_rfc9001_selftest(void) {
    /* A.1 客户端初始密钥向量（DCID = 8394c8f03e515708） */
    static const uint8_t dcid[8] = {0x83, 0x94, 0xc8, 0xf0, 0x3e, 0x51, 0x57, 0x08};
    static const uint8_t exp_ck[16] = {0x1f,0x36,0x96,0x13,0xdd,0x76,0xd5,0x46,0x77,0x30,0xef,0xcb,0xe3,0xb1,0xa2,0x2d};
    static const uint8_t exp_civ[12] = {0xfa,0x04,0x4b,0x2f,0x42,0xa3,0xfd,0x3b,0x46,0xfb,0x25,0x5c};
    static const uint8_t exp_chp[16] = {0x9f,0x50,0x44,0x9e,0x04,0xa0,0xe8,0x10,0x28,0x3a,0x1e,0x99,0x33,0xad,0xed,0xd2};
    static const uint8_t exp_sk[16] = {0xcf,0x3a,0x53,0x31,0x65,0x3c,0x36,0x4c,0x88,0xf0,0xf3,0x79,0xb6,0x06,0x7e,0x37};
    static const uint8_t exp_siv[12] = {0x0a,0xc1,0x49,0x3c,0xa1,0x90,0x58,0x53,0xb0,0xbb,0xa0,0x3e};
    static const uint8_t exp_shp[16] = {0xc2,0x06,0xb8,0xd9,0xb9,0xf0,0xf3,0x76,0x44,0x43,0x0b,0x49,0x0e,0xea,0xa3,0x14};

    nl_quic_keys_t ck, sk;
    if (nl_quic_initial_keys(dcid, sizeof(dcid), &ck, &sk) != 0) return 1;

    if (!quic_eq(ck.key, exp_ck, 16)) return 2;
    if (!quic_eq(ck.iv,  exp_civ, NL_QUIC_IV_LEN)) return 3;
    if (!quic_eq(ck.hp,  exp_chp, 16)) return 4;
    if (!quic_eq(sk.key, exp_sk, 16)) return 5;
    if (!quic_eq(sk.iv,  exp_siv, NL_QUIC_IV_LEN)) return 6;
    if (!quic_eq(sk.hp,  exp_shp, 16)) return 7;

    /* 保护 → 解保护 往返自洽 */
    uint8_t pkt[128];
    memset(pkt, 0, sizeof(pkt));
    size_t pn_offset = 10, pn_len = 1;
    pkt[0] = 0xc3;                                   /* 长首部首字节 */
    for (size_t i = 1; i < pn_offset; i++) pkt[i] = (uint8_t)(0x40 + i);
    pkt[pn_offset] = 0x02;                           /* PN = 2 */
    size_t ph = pn_offset + pn_len;
    for (size_t i = 0; i < 20; i++) pkt[ph + i] = (uint8_t)(0xa0 + i);   /* 明文 */
    uint8_t orig[20];
    memcpy(orig, pkt + ph, 20);

    size_t total = 0;
    if (nl_quic_protect_packet(&ck, 2, pkt, pn_offset, pn_len, 20, &total) != 0) return 8;
    if (total != ph + 20 + NL_QUIC_TAG_LEN) return 9;
    if (pkt[0] == 0xc3) return 10;                   /* 首部应已被保护 */

    size_t plen = 0;
    if (nl_quic_unprotect_packet(&ck, 2, pkt, pn_offset, pn_len, 20, &plen) != 0) return 11;
    if (plen != 20) return 12;
    if (!quic_eq(pkt + ph, orig, 20)) return 13;
    if (pkt[0] != 0xc3) return 14;                   /* 首部应被还原 */

    /* C2：短首部往返（long_header=0，首字节按低 5 位掩码） */
    uint8_t sp[128];
    memset(sp, 0, sizeof(sp));
    size_t spo = 1;                                  /* 短首部：首字节后即 PN */
    sp[0] = 0x43;
    sp[spo] = 0x07;                                  /* PN = 7 */
    size_t sph = spo + 1;
    for (size_t i = 0; i < 24; i++) sp[sph + i] = (uint8_t)(0x10 + i);
    uint8_t sorig[24];
    memcpy(sorig, sp + sph, 24);
    size_t stotal = 0;
    if (nl_quic_protect_packet_ex(&ck, 7, 0, sp, spo, 1, 24, &stotal) != 0) return 15;
    size_t splen = 0;
    if (nl_quic_unprotect_packet_ex(&ck, 7, 0, sp, spo, 1, 24, &splen) != 0) return 16;
    if (splen != 24 || !quic_eq(sp + sph, sorig, 24)) return 17;
    if (sp[0] != 0x43) return 18;

    /* C2：ChaCha20-Poly1305 往返（is_chacha=1，key/hp 32 字节） */
    nl_quic_keys_t ck2;
    memset(&ck2, 0, sizeof(ck2));
    for (size_t i = 0; i < 32; i++) { ck2.key[i] = (uint8_t)(0x30 + i); ck2.hp[i] = (uint8_t)(0x60 + i); }
    for (size_t i = 0; i < NL_QUIC_IV_LEN; i++) ck2.iv[i] = (uint8_t)(0x90 + i);
    ck2.key_len = 32; ck2.hp_len = 32; ck2.is_chacha = 1;
    uint8_t cp[128];
    memset(cp, 0, sizeof(cp));
    size_t cpo = 10;
    cp[0] = 0xc3;
    cp[cpo] = 0x01;
    size_t cph = cpo + 1;
    for (size_t i = 0; i < 32; i++) cp[cph + i] = (uint8_t)(0x22 + i);
    uint8_t corig[32];
    memcpy(corig, cp + cph, 32);
    size_t ctotal = 0;
    if (nl_quic_protect_packet_ex(&ck2, 1, 1, cp, cpo, 1, 32, &ctotal) != 0) return 19;
    size_t cplen = 0;
    if (nl_quic_unprotect_packet_ex(&ck2, 1, 1, cp, cpo, 1, 32, &cplen) != 0) return 20;
    if (cplen != 32 || !quic_eq(cp + cph, corig, 32)) return 21;

    /* C2：包号编码↔解码往返（RFC 9000 §17.1 / §A.3） */
    for (uint64_t full = 1; full < 200000; full += 12345) {
        uint8_t pnb[4];
        size_t nlen = 0;
        if (nl_quic_pn_encode(full, full - 1, pnb, &nlen) != 0) return 22;
        uint64_t trunc = 0;
        for (size_t i = 0; i < nlen; i++) trunc = (trunc << 8) | pnb[i];
        if (nl_quic_pn_decode(full - 1, trunc, nlen) != full) return 23;
    }

    /* 外部 KAT：RFC 9001 A.1 中间秘密（initial / client / server initial secret） */
    {
        static const uint8_t exp_initial[32] = {
            0x7d,0xb5,0xdf,0x06,0xe7,0xa6,0x9e,0x43,0x24,0x96,0xad,0xed,0xb0,0x08,0x51,0x92,
            0x35,0x95,0x22,0x15,0x96,0xae,0x2a,0xe9,0xfb,0x81,0x15,0xc1,0xe9,0xed,0x0a,0x44};
        static const uint8_t exp_cis[32] = {
            0xc0,0x0c,0xf1,0x51,0xca,0x5b,0xe0,0x75,0xed,0x0e,0xbf,0xb5,0xc8,0x03,0x23,0xc4,
            0x2d,0x6b,0x7d,0xb6,0x78,0x81,0x28,0x9a,0xf4,0x00,0x8f,0x1f,0x6c,0x35,0x7a,0xea};
        static const uint8_t exp_sis[32] = {
            0x3c,0x19,0x98,0x28,0xfd,0x13,0x9e,0xfd,0x21,0x6c,0x15,0x5a,0xd8,0x44,0xcc,0x81,
            0xfb,0x82,0xfa,0x8d,0x74,0x46,0xfa,0x7d,0x78,0xbe,0x80,0x3a,0xcd,0xda,0x95,0x1b};
        uint8_t isec[32], cis[32], sis[32];
        if (nl_quic__hkdf_extract(NL_QUIC_V1_SALT, sizeof(NL_QUIC_V1_SALT), dcid, sizeof(dcid), isec) != 0) return 30;
        if (!quic_eq(isec, exp_initial, 32)) return 31;
        if (nl_quic__expand_label(isec, 32, "client in", NULL, 0, cis, 32) != 0) return 32;
        if (nl_quic__expand_label(isec, 32, "server in", NULL, 0, sis, 32) != 0) return 33;
        if (!quic_eq(cis, exp_cis, 32)) return 34;
        if (!quic_eq(sis, exp_sis, 32)) return 35;
    }

    /* 外部 KAT：RFC 9001 A.5 ChaCha20-Poly1305 短首部包 */
    {
        static const uint8_t a5_secret[32] = {
            0x9a,0xc3,0x12,0xa7,0xf8,0x77,0x46,0x8e,0xbe,0x69,0x42,0x27,0x48,0xad,0x00,0xa1,
            0x54,0x43,0xf1,0x82,0x03,0xa0,0x7d,0x60,0x60,0xf6,0x88,0xf3,0x0f,0x21,0x63,0x2b};
        static const uint8_t exp_key[32] = {
            0xc6,0xd9,0x8f,0xf3,0x44,0x1c,0x3f,0xe1,0xb2,0x18,0x20,0x94,0xf6,0x9c,0xaa,0x2e,
            0xd4,0xb7,0x16,0xb6,0x54,0x88,0x96,0x0a,0x7a,0x98,0x49,0x79,0xfb,0x23,0xe1,0xc8};
        static const uint8_t exp_iv[12] = {0xe0,0x45,0x9b,0x34,0x74,0xbd,0xd0,0xe4,0x4a,0x41,0xc1,0x44};
        static const uint8_t exp_hp[32] = {
            0x25,0xa2,0x82,0xb9,0xe8,0x2f,0x06,0xf2,0x1f,0x48,0x89,0x17,0xa4,0xfc,0x8f,0x1b,
            0x73,0x57,0x36,0x85,0x60,0x85,0x97,0xd0,0xef,0xcb,0x07,0x6b,0x0a,0xb7,0xa7,0xa4};
        static const uint8_t exp_ku[32] = {
            0x12,0x23,0x50,0x47,0x55,0x03,0x6d,0x55,0x63,0x42,0xee,0x93,0x61,0xd2,0x53,0x42,
            0x1a,0x82,0x6c,0x9e,0xcd,0xf3,0xc7,0x14,0x86,0x84,0xb3,0x6b,0x71,0x48,0x81,0xf9};
        static const uint8_t exp_pkt[21] = {
            0x4c,0xfe,0x41,0x89,0x65,0x5e,0x5c,0xd5,0x5c,0x41,0xf6,0x90,0x80,0x57,0x5d,0x79,
            0x99,0xc2,0x5a,0x5b,0xfb};

        nl_quic_keys_t a5;
        if (nl_tls13_secret_to_quic_keys(a5_secret, 32, NL_QUIC_CIPHER_CHACHA20_POLY1305_SHA256, &a5) != 0) return 36;
        if (a5.key_len != 32 || !a5.is_chacha) return 37;
        if (!quic_eq(a5.key, exp_key, 32)) return 38;
        if (!quic_eq(a5.iv, exp_iv, 12)) return 39;
        if (!quic_eq(a5.hp, exp_hp, 32)) return 40;

        uint8_t ku[32];
        memcpy(ku, a5_secret, 32);
        if (nl_quic_update_key_secret(ku) != 0) return 41;
        if (!quic_eq(ku, exp_ku, 32)) return 42;

        /* 密钥集：quic ku 更新 → secret 变为 ku、密钥随之改变 */
        {
            nl_quic_keyset_t ks;
            if (nl_quic_keyset_init(&ks, a5_secret, NL_QUIC_CIPHER_CHACHA20_POLY1305_SHA256) != 0) return 50;
            if (!quic_eq(ks.keys.key, exp_key, 32)) return 51;
            if (nl_quic_keyset_key_update(&ks) != 0) return 52;
            if (!quic_eq(ks.secret, exp_ku, 32)) return 53;
            if (quic_eq(ks.keys.key, exp_key, 32)) return 54;   /* 更新后密钥必须改变 */
        }

        /* 保护：header 4200bff4 + 明文 01，PN=654360564（长 3 字节） */
        uint8_t a5pkt[32];
        memset(a5pkt, 0, sizeof(a5pkt));
        a5pkt[0] = 0x42; a5pkt[1] = 0x00; a5pkt[2] = 0xbf; a5pkt[3] = 0xf4;
        a5pkt[4] = 0x01;
        size_t a5total = 0;
        if (nl_quic_protect_packet_ex(&a5, 654360564ULL, 0, a5pkt, 1, 3, 1, &a5total) != 0) return 43;
        if (a5total != 21) return 44;
        if (!quic_eq(a5pkt, exp_pkt, 21)) return 45;

        /* 解保护还原 */
        size_t a5plen = 0;
        if (nl_quic_unprotect_packet_ex(&a5, 654360564ULL, 0, a5pkt, 1, 3, 1, &a5plen) != 0) return 46;
        if (a5plen != 1 || a5pkt[4] != 0x01 || a5pkt[0] != 0x42) return 47;
    }

    /* 外部 KAT：RFC 9001 A.3 服务端 Initial（AES 长首部，pn=1，pn_len=2） */
    {
        static const uint8_t a3pkt[135] = {
            0xcf,0x00,0x00,0x00,0x01,0x00,0x08,0xf0,0x67,0xa5,0x50,0x2a,0x42,0x62,0xb5,0x00,
            0x40,0x75,0xc0,0xd9,0x5a,0x48,0x2c,0xd0,0x99,0x1c,0xd2,0x5b,0x0a,0xac,0x40,0x6a,
            0x58,0x16,0xb6,0x39,0x41,0x00,0xf3,0x7a,0x1c,0x69,0x79,0x75,0x54,0x78,0x0b,0xb3,
            0x8c,0xc5,0xa9,0x9f,0x5e,0xde,0x4c,0xf7,0x3c,0x3e,0xc2,0x49,0x3a,0x18,0x39,0xb3,
            0xdb,0xcb,0xa3,0xf6,0xea,0x46,0xc5,0xb7,0x68,0x4d,0xf3,0x54,0x8e,0x7d,0xde,0xb9,
            0xc3,0xbf,0x9c,0x73,0xcc,0x3f,0x3b,0xde,0xd7,0x4b,0x56,0x2b,0xfb,0x19,0xfb,0x84,
            0x02,0x2f,0x8e,0xf4,0xcd,0xd9,0x37,0x95,0xd7,0x7d,0x06,0xed,0xbb,0x7a,0xaf,0x2f,
            0x58,0x89,0x18,0x50,0xab,0xbd,0xca,0x3d,0x20,0x39,0x8c,0x27,0x64,0x56,0xcb,0xc4,
            0x21,0x58,0x40,0x7d,0xd0,0x74,0xee
        };
        static const uint8_t a3exp[99] = {
            0x02,0x00,0x00,0x00,0x00,0x06,0x00,0x40,0x5a,0x02,0x00,0x00,0x56,0x03,0x03,0xee,
            0xfc,0xe7,0xf7,0xb3,0x7b,0xa1,0xd1,0x63,0x2e,0x96,0x67,0x78,0x25,0xdd,0xf7,0x39,
            0x88,0xcf,0xc7,0x98,0x25,0xdf,0x56,0x6d,0xc5,0x43,0x0b,0x9a,0x04,0x5a,0x12,0x00,
            0x13,0x01,0x00,0x00,0x2e,0x00,0x33,0x00,0x24,0x00,0x1d,0x00,0x20,0x9d,0x3c,0x94,
            0x0d,0x89,0x69,0x0b,0x84,0xd0,0x8a,0x60,0x99,0x3c,0x14,0x4e,0xca,0x68,0x4d,0x10,
            0x81,0x28,0x7c,0x83,0x4d,0x53,0x11,0xbc,0xf3,0x2b,0xb9,0xda,0x1a,0x00,0x2b,0x00,
            0x02,0x03,0x04
        };
        uint8_t b3[135];
        memcpy(b3, a3pkt, sizeof(b3));
        size_t plen3 = 0;
        if (nl_quic_unprotect_packet_ex(&sk, 1, 1, b3, 18, 2, 99, &plen3) != 0) return 60;
        if (plen3 != 99) return 61;
        if (!quic_eq(b3 + 18 + 2, a3exp, 99)) return 62;
    }

    /* 外部 KAT：RFC 9001 A.2 客户端 Initial（AES 长首部，pn=2，pn_len=4，1162B 载荷） */
    {
        static const uint8_t a2hdr[22] = {
            0xc3,0x00,0x00,0x00,0x01,0x08,0x83,0x94,0xc8,0xf0,0x3e,0x51,0x57,0x08,0x00,0x00,
            0x44,0x9e,0x00,0x00,0x00,0x02
        };
        static const uint8_t a2crypto[245] = {
            0x06,0x00,0x40,0xf1,0x01,0x00,0x00,0xed,0x03,0x03,0xeb,0xf8,0xfa,0x56,0xf1,0x29,
            0x39,0xb9,0x58,0x4a,0x38,0x96,0x47,0x2e,0xc4,0x0b,0xb8,0x63,0xcf,0xd3,0xe8,0x68,
            0x04,0xfe,0x3a,0x47,0xf0,0x6a,0x2b,0x69,0x48,0x4c,0x00,0x00,0x04,0x13,0x01,0x13,
            0x02,0x01,0x00,0x00,0xc0,0x00,0x00,0x00,0x10,0x00,0x0e,0x00,0x00,0x0b,0x65,0x78,
            0x61,0x6d,0x70,0x6c,0x65,0x2e,0x63,0x6f,0x6d,0xff,0x01,0x00,0x01,0x00,0x00,0x0a,
            0x00,0x08,0x00,0x06,0x00,0x1d,0x00,0x17,0x00,0x18,0x00,0x10,0x00,0x07,0x00,0x05,
            0x04,0x61,0x6c,0x70,0x6e,0x00,0x05,0x00,0x05,0x01,0x00,0x00,0x00,0x00,0x00,0x33,
            0x00,0x26,0x00,0x24,0x00,0x1d,0x00,0x20,0x93,0x70,0xb2,0xc9,0xca,0xa4,0x7f,0xba,
            0xba,0xf4,0x55,0x9f,0xed,0xba,0x75,0x3d,0xe1,0x71,0xfa,0x71,0xf5,0x0f,0x1c,0xe1,
            0x5d,0x43,0xe9,0x94,0xec,0x74,0xd7,0x48,0x00,0x2b,0x00,0x03,0x02,0x03,0x04,0x00,
            0x0d,0x00,0x10,0x00,0x0e,0x04,0x03,0x05,0x03,0x06,0x03,0x02,0x03,0x08,0x04,0x08,
            0x05,0x08,0x06,0x00,0x2d,0x00,0x02,0x01,0x01,0x00,0x1c,0x00,0x02,0x40,0x01,0x00,
            0x39,0x00,0x32,0x04,0x08,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x05,0x04,0x80,
            0x00,0xff,0xff,0x07,0x04,0x80,0x00,0xff,0xff,0x08,0x01,0x10,0x01,0x04,0x80,0x00,
            0x75,0x30,0x09,0x01,0x10,0x0f,0x08,0x83,0x94,0xc8,0xf0,0x3e,0x51,0x57,0x08,0x06,
            0x04,0x80,0x00,0xff,0xff
        };
        static const uint8_t a2exp_hdr[22] = {
            0xc0,0x00,0x00,0x00,0x01,0x08,0x83,0x94,0xc8,0xf0,0x3e,0x51,0x57,0x08,0x00,0x00,
            0x44,0x9e,0x7b,0x9a,0xec,0x34
        };
        static const uint8_t a2exp_sample[16] = {
            0xd1,0xb1,0xc9,0x8d,0xd7,0x68,0x9f,0xb8,0xec,0x11,0xd2,0x42,0xb1,0x23,0xdc,0x9b
        };
        uint8_t pkt[1200];
        memset(pkt, 0, sizeof(pkt));           /* 载荷余下部分为 PADDING(0x00) */
        memcpy(pkt, a2hdr, 22);
        memcpy(pkt + 22, a2crypto, 245);
        size_t total = 0;
        if (nl_quic_protect_packet_ex(&ck, 2, 1, pkt, 18, 4, 1162, &total) != 0) return 70;
        if (total != 1200) return 71;
        if (!quic_eq(pkt, a2exp_hdr, 22)) return 72;
        if (!quic_eq(pkt + 22, a2exp_sample, 16)) return 73;
    }

    return 0;
}

/* ============================================================
 * 聚合自测入口
 * ============================================================ */
int nl_quictls_selftest_all(void) {
    int failures = 0;
    if (nl_quic_rfc9001_selftest()      != 0) failures++;
    if (nl_quictls_tls13_selftest()     != 0) failures++;
    if (nl_quictls_handshake_selftest() != 0) failures++;
    if (nl_quictls_quic_selftest()      != 0) failures++;
    if (nl_quictls_recovery_selftest()  != 0) failures++;
    if (nl_quictls_flowcontrol_selftest() != 0) failures++;
    if (nl_quictls_qpack_selftest()     != 0) failures++;
    if (nl_quictls_huffman_selftest()   != 0) failures++;
    if (nl_quictls_qpack_stream_selftest() != 0) failures++;
    if (nl_quictls_qpack_link_selftest() != 0) failures++;
    if (nl_quictls_h3server_selftest()  != 0) failures++;
    return failures;
}

/* ============================================================
 * 模块 / 扩展接入
 * ============================================================ */
NL_MODULE_DEFINE_LAZY(
    NL_MODULE_QUICTLS, quictls, NL_QUICTLS_VERSION,
    NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_ASYNC | NL_CAP_PLATFORM_ALL |
        NL_CAP_EXT_SYSTEM | NL_CAP_QUIC,
    1, 1, 1,
    nl_quictls_init, NULL, nl_quictls_is_available, nl_quictls_version,
    "Self-implemented QUIC-TLS (RFC 9001) packet protection for HTTP/3",
    "NetLeaf Team", NULL, NULL);

int nl_quictls_init(void) {
    return nl_module_register(NL_MODULE_GET_INFO(quictls));
}

int nl_quictls_is_available(void) {
    return 1;
}

const char* nl_quictls_version(void) {
    return NL_QUICTLS_VERSION;
}

NL_EXTENSION_DEFINE(quictls,
    "QUIC-TLS Extension",
    NL_QUICTLS_VERSION,
    "NetLeaf",
    "Self-implemented QUIC-TLS (RFC 9001) extension: HKDF key schedule and "
    "QUIC packet protection (AEAD + header protection) built on mbedTLS "
    "primitives, targeting HTTP/3 over QUIC. Reference: picotls/quicly, "
    "ngtcp2/nghttp3, lsquic.",
    "all",
    NL_CAP_QUIC,
    nl_quictls_init,
    NULL,
    nl_quictls_is_available,
    nl_quictls_version
);

nl_extension_info_t* nl_quictls_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(quictls);
}
