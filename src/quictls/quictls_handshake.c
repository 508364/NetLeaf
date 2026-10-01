/**
 * @file quictls_handshake.c
 * @brief 自研 QUIC-TLS：TLS 1.3 握手状态机（含证书认证）
 * @version 0.2.0
 *
 * 覆盖 TLS 1.3 握手核心路径：ClientHello/ServerHello（supported_versions /
 * key_share / signature_algorithms）、X25519 ECDHE、EncryptedExtensions、
 * Certificate + CertificateVerify（ECDSA P-256，签署 transcript）、Finished 校验，
 * 并结合 C4 密钥调度派生握手/1-RTT 密钥。
 *
 * 说明：握手消息以明文 CRYPTO 帧载荷在两端间传递；QUIC 包保护由底层
 * nl_quic_*_packet 完成。参考范式：picotls。
 */

#include "netleaf_quictls.h"
#include "quictls_internal.h"

#include <string.h>
#include <stdlib.h>

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pem.h>
#include <mbedtls/bignum.h>

#define QUIC_TLS13_X25519_GROUP          0x001d
#define QUIC_TLS13_SIG_ECDSA_P256_SHA256 0x0403

/* ============================================================
 * RNG（全局惰性初始化）
 * ============================================================ */
static mbedtls_entropy_context  g_entropy;
static mbedtls_ctr_drbg_context g_drbg;
static int g_rng_ready = 0;

static int tls13_rng_ready(void) {
    if (g_rng_ready) return 0;
    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_drbg);
    const char* pers = "netleaf_quictls";
    int rc = mbedtls_ctr_drbg_seed(&g_drbg, mbedtls_entropy_func, &g_entropy,
                                   (const unsigned char*)pers, strlen(pers));
    if (rc != 0) return -1;
    g_rng_ready = 1;
    return 0;
}

/* ============================================================
 * 连接结构
 * ============================================================ */
struct nl_tls13_conn {
    int    is_server;
    int    established;
    int    peer_verified;

    uint8_t random[32];
    char    alpn[16];            /* 协商出的 ALPN（H3 用 "h3"） */

    mbedtls_ecdh_context ecdh;
    int     ecdh_inited;
    uint8_t our_pub[64];
    size_t  our_pub_len;
    uint8_t shared[64];
    size_t  shared_len;

    mbedtls_sha256_context th;   /* transcript hash（运行中） */

    uint8_t c_hs[32], s_hs[32], master[32];
    uint8_t c_ap0[32], s_ap0[32];

    nl_quic_keys_t read1, write1;

    /* 证书 / 私钥（服务端为本方证书链 + 私钥；客户端为对端证书链） */
    mbedtls_x509_crt cert;
    int              cert_inited;
    mbedtls_pk_context key;
    int              key_inited;
    mbedtls_x509_crt ca;
    int              ca_inited;
};

/* ============================================================
 * 简易写缓冲
 * ============================================================ */
typedef struct { uint8_t* p; size_t cap; size_t len; int err; } wbuf_t;
static void w_u8(wbuf_t* w, uint8_t v)  { if (w->len + 1 > w->cap) { w->err = 1; return; } w->p[w->len++] = v; }
static void w_u16(wbuf_t* w, uint16_t v) { w_u8(w, (uint8_t)(v >> 8)); w_u8(w, (uint8_t)v); }
static void w_u24(wbuf_t* w, uint32_t v) { w_u8(w, (uint8_t)(v >> 16)); w_u8(w, (uint8_t)(v >> 8)); w_u8(w, (uint8_t)v); }
static void w_bytes(wbuf_t* w, const uint8_t* b, size_t n) {
    if (w->len + n > w->cap) { w->err = 1; return; }
    memcpy(w->p + w->len, b, n); w->len += n;
}

/* ============================================================
 * ECDHE（X25519）
 * ============================================================ */
static int tls13_ecdh_make(nl_tls13_conn_t* c) {
    mbedtls_ecdh_init(&c->ecdh);
    c->ecdh_inited = 1;
    if (mbedtls_ecdh_setup(&c->ecdh, MBEDTLS_ECP_DP_CURVE25519) != 0) return -1;
    size_t olen = 0;
    if (mbedtls_ecdh_make_public(&c->ecdh, &olen, c->our_pub, sizeof(c->our_pub),
                                 mbedtls_ctr_drbg_random, &g_drbg) != 0) return -1;
    c->our_pub_len = olen;
    return 0;
}

static int tls13_ecdh_calc(nl_tls13_conn_t* c, const uint8_t* peer_pub, size_t peer_len) {
    if (mbedtls_ecdh_read_public(&c->ecdh, peer_pub, peer_len) != 0) return -1;
    size_t olen = 0;
    if (mbedtls_ecdh_calc_secret(&c->ecdh, &olen, c->shared, sizeof(c->shared),
                                 mbedtls_ctr_drbg_random, &g_drbg) != 0) return -1;
    c->shared_len = olen;
    return 0;
}

/* ============================================================
 * 握手消息构造
 * ============================================================ */
static int build_client_hello(nl_tls13_conn_t* c, uint8_t* out, size_t cap, size_t* out_len) {
    uint8_t body[300];
    wbuf_t w = { body, sizeof(body), 0, 0 };

    w_u16(&w, 0x0303);
    w_bytes(&w, c->random, 32);
    w_u8(&w, 0);
    w_u16(&w, 4);
    w_u8(&w, 0x13); w_u8(&w, 0x01);
    w_u8(&w, 0x13); w_u8(&w, 0x03);
    w_u8(&w, 1); w_u8(&w, 0);

    size_t ext_len_pos = w.len; w_u16(&w, 0);
    size_t ext_start = w.len;

    w_u16(&w, 0x002b); w_u16(&w, 2); w_u16(&w, 0x0304);        /* supported_versions */

    w_u16(&w, 0x0010); w_u16(&w, 5);                           /* ALPN：ProtocolNameList["h3"] */
    w_u16(&w, 3); w_u8(&w, 2); w_bytes(&w, (const uint8_t*)"h3", 2);

    w_u16(&w, 0x000d); w_u16(&w, 4); w_u16(&w, 2);             /* signature_algorithms */
    w_u16(&w, QUIC_TLS13_SIG_ECDSA_P256_SHA256);

    w_u16(&w, 0x0033);                                        /* key_share */
    w_u16(&w, (uint16_t)(2 + 2 + 2 + c->our_pub_len));
    w_u16(&w, (uint16_t)(2 + 2 + c->our_pub_len));
    w_u16(&w, QUIC_TLS13_X25519_GROUP);
    w_u16(&w, (uint16_t)c->our_pub_len);
    w_bytes(&w, c->our_pub, c->our_pub_len);

    uint16_t ext_len = (uint16_t)(w.len - ext_start);
    body[ext_len_pos]     = (uint8_t)(ext_len >> 8);
    body[ext_len_pos + 1] = (uint8_t)ext_len;

    if (w.err) return -1;
    return nl_tls13_encode_handshake(NL_TLS13_HS_CLIENT_HELLO, body, w.len, out, cap, out_len);
}

static int build_server_hello(nl_tls13_conn_t* c, uint8_t* out, size_t cap, size_t* out_len) {
    uint8_t body[256];
    wbuf_t w = { body, sizeof(body), 0, 0 };

    w_u16(&w, 0x0303);
    w_bytes(&w, c->random, 32);
    w_u8(&w, 0);
    w_u16(&w, 0x1301);
    w_u8(&w, 0);

    size_t ext_len_pos = w.len; w_u16(&w, 0);
    size_t ext_start = w.len;

    w_u16(&w, 0x002b); w_u16(&w, 2); w_u16(&w, 0x0304);

    w_u16(&w, 0x0010); w_u16(&w, 3);                           /* ALPN：选中 "h3" */
    w_u8(&w, 2); w_bytes(&w, (const uint8_t*)"h3", 2);

    w_u16(&w, 0x0033);
    w_u16(&w, (uint16_t)(2 + 2 + c->our_pub_len));
    w_u16(&w, QUIC_TLS13_X25519_GROUP);
    w_u16(&w, (uint16_t)c->our_pub_len);
    w_bytes(&w, c->our_pub, c->our_pub_len);

    uint16_t ext_len = (uint16_t)(w.len - ext_start);
    body[ext_len_pos]     = (uint8_t)(ext_len >> 8);
    body[ext_len_pos + 1] = (uint8_t)ext_len;

    if (w.err) return -1;
    return nl_tls13_encode_handshake(NL_TLS13_HS_SERVER_HELLO, body, w.len, out, cap, out_len);
}

static int build_encrypted_extensions(uint8_t* out, size_t cap, size_t* out_len) {
    uint8_t body[2];
    wbuf_t w = { body, sizeof(body), 0, 0 };
    w_u16(&w, 0);
    if (w.err) return -1;
    return nl_tls13_encode_handshake(NL_TLS13_HS_ENCRYPTED_EXTENSIONS, body, 2, out, cap, out_len);
}

static int build_certificate(nl_tls13_conn_t* c, uint8_t* out, size_t cap, size_t* out_len) {
    if (!c->cert_inited || c->cert.raw.len == 0) return -1;
    static uint8_t body[4096];
    wbuf_t w = { body, sizeof(body), 0, 0 };

    w_u8(&w, 0);                                   /* certificate_request_context len 0 */
    size_t list_pos = w.len; w_u24(&w, 0);         /* u24 占位 */
    size_t list_start = w.len;

    w_u24(&w, (uint32_t)c->cert.raw.len);          /* 条目：cert_data(u24+DER) */
    w_bytes(&w, c->cert.raw.p, c->cert.raw.len);
    w_u16(&w, 0);                                  /* entry extensions len 0 */

    size_t list_len = w.len - list_start;
    body[list_pos]     = (uint8_t)(list_len >> 16);
    body[list_pos + 1] = (uint8_t)(list_len >> 8);
    body[list_pos + 2] = (uint8_t)list_len;

    if (w.err) return -1;
    return nl_tls13_encode_handshake(NL_TLS13_HS_CERTIFICATE, body, w.len, out, cap, out_len);
}

/* CertificateVerify 待签内容：64*0x20 || 上下文串 || 0x00 || Transcript-Hash */
static size_t cv_signed_content(int is_server, const uint8_t th[32], uint8_t* out) {
    size_t p = 0;
    memset(out, 0x20, 64); p = 64;
    const char* ctx = is_server ? "TLS 1.3, server CertificateVerify"
                                : "TLS 1.3, client CertificateVerify";
    size_t cl = strlen(ctx);
    memcpy(out + p, ctx, cl); p += cl;
    out[p++] = 0x00;
    memcpy(out + p, th, 32); p += 32;
    return p;
}

static int build_certificate_verify(nl_tls13_conn_t* c, const uint8_t th[32],
                                    uint8_t* out, size_t cap, size_t* out_len) {
    if (!c->key_inited) return -1;
    uint8_t content[160];
    size_t clen = cv_signed_content(c->is_server, th, content);
    uint8_t hash[32];
    if (nl_tls13_sha256(content, clen, hash) != 0) return -1;

    uint8_t sig[256];
    size_t sig_len = 0;
    if (mbedtls_pk_sign(&c->key, MBEDTLS_MD_SHA256, hash, 32, sig, sizeof(sig),
                        &sig_len, mbedtls_ctr_drbg_random, &g_drbg) != 0) return -1;

    uint8_t body[300];
    wbuf_t w = { body, sizeof(body), 0, 0 };
    w_u16(&w, QUIC_TLS13_SIG_ECDSA_P256_SHA256);
    w_u16(&w, (uint16_t)sig_len);
    w_bytes(&w, sig, sig_len);
    if (w.err) return -1;
    return nl_tls13_encode_handshake(NL_TLS13_HS_CERTIFICATE_VERIFY, body, w.len, out, cap, out_len);
}

static int parse_peer_key_share(const uint8_t* hs, size_t len,
                                uint8_t out_pub[64], size_t* out_pub_len, uint8_t* out_type) {
    uint8_t mt = 0;
    const uint8_t* b = NULL;
    size_t bl = 0, consumed = 0;
    if (nl_tls13_parse_handshake(hs, len, &mt, &b, &bl, &consumed) != 0) return -1;
    if (out_type) *out_type = mt;
    if (mt != NL_TLS13_HS_CLIENT_HELLO && mt != NL_TLS13_HS_SERVER_HELLO) return -1;
    if (bl < 2 + 32 + 1) return -1;

    size_t p = 2 + 32;
    size_t sid = b[p]; p += 1 + sid;
    if (p > bl) return -1;
    if (mt == NL_TLS13_HS_CLIENT_HELLO) {
        if (p + 2 > bl) return -1;
        size_t cs = ((size_t)b[p] << 8) | b[p + 1]; p += 2 + cs;
        if (p + 1 > bl) return -1;
        size_t cm = b[p]; p += 1 + cm;
    } else {
        p += 2;
        if (p + 1 > bl) return -1;
        size_t cm = b[p]; p += 1 + cm;
    }
    if (p + 2 > bl) return -1;
    size_t ext_total = ((size_t)b[p] << 8) | b[p + 1]; p += 2;
    size_t ext_end = p + ext_total;
    if (ext_end > bl) return -1;

    while (p + 4 <= ext_end) {
        uint16_t et = (uint16_t)((b[p] << 8) | b[p + 1]);
        uint16_t el = (uint16_t)((b[p + 2] << 8) | b[p + 3]);
        p += 4;
        if (p + el > ext_end) return -1;
        if (et == 0x0033) {
            const uint8_t* kd = b + p;
            size_t kr = el;
            if (mt == NL_TLS13_HS_CLIENT_HELLO) {
                if (kr < 2) return -1;
                kr = ((size_t)kd[0] << 8) | kd[1];
                kd += 2;
            }
            if (kr < 4) return -1;
            uint16_t grp = (uint16_t)((kd[0] << 8) | kd[1]);
            uint16_t klen = (uint16_t)((kd[2] << 8) | kd[3]);
            if (grp != QUIC_TLS13_X25519_GROUP) return -1;
            if (4 + klen > kr || klen > 64) return -1;
            memcpy(out_pub, kd + 4, klen);
            *out_pub_len = klen;
            return 0;
        }
        p += el;
    }
    return -1;
}

/* 解析 ALPN 扩展（is_server_hello=0 取列表首项，=1 取单项） */
static int parse_alpn(const uint8_t* hs, size_t len, int is_server_hello,
                      char* out, size_t outcap) {
    uint8_t mt = 0;
    const uint8_t* b = NULL;
    size_t bl = 0, consumed = 0;
    if (nl_tls13_parse_handshake(hs, len, &mt, &b, &bl, &consumed) != 0) return -1;
    if (bl < 2 + 32 + 1) return -1;
    size_t p = 2 + 32;
    size_t sid = b[p]; p += 1 + sid;
    if (p > bl) return -1;
    if (mt == NL_TLS13_HS_CLIENT_HELLO) {
        if (p + 2 > bl) return -1;
        size_t cs = ((size_t)b[p] << 8) | b[p + 1]; p += 2 + cs;
        if (p + 1 > bl) return -1;
        size_t cm = b[p]; p += 1 + cm;
    } else {
        p += 2;
        if (p + 1 > bl) return -1;
        size_t cm = b[p]; p += 1 + cm;
    }
    if (p + 2 > bl) return -1;
    size_t ext_total = ((size_t)b[p] << 8) | b[p + 1]; p += 2;
    size_t ext_end = p + ext_total;
    if (ext_end > bl) return -1;
    while (p + 4 <= ext_end) {
        uint16_t et = (uint16_t)((b[p] << 8) | b[p + 1]);
        uint16_t el = (uint16_t)((b[p + 2] << 8) | b[p + 3]);
        p += 4;
        if (p + el > ext_end) return -1;
        if (et == 0x0010) {
            const uint8_t* d = b + p;
            size_t dl = el, q = 0;
            if (!is_server_hello) {                 /* CH：ProtocolNameList */
                if (dl < 2) return -1;
                size_t ll = ((size_t)d[0] << 8) | d[1];
                q = 2;
                if (q + ll > dl) return -1;
            }
            if (q >= dl) return -1;
            size_t plen = d[q++];
            if (q + plen > dl || plen >= outcap) return -1;
            memcpy(out, d + q, plen);
            out[plen] = '\0';
            return 0;
        }
        p += el;
    }
    return -1;
}

/* 客户端：解析服务端证书链（取首个证书 DER 入链） */
static int parse_certificate(nl_tls13_conn_t* c, const uint8_t* msg, size_t len) {
    uint8_t mt = 0;
    const uint8_t* b = NULL;
    size_t bl = 0, consumed = 0;
    if (nl_tls13_parse_handshake(msg, len, &mt, &b, &bl, &consumed) != 0) return -1;
    if (mt != NL_TLS13_HS_CERTIFICATE) return -1;
    if (bl < 1) return -1;
    size_t rc = b[0];
    size_t p = 1 + rc;
    if (p + 3 > bl) return -1;
    size_t list_len = ((size_t)b[p] << 16) | ((size_t)b[p + 1] << 8) | b[p + 2]; p += 3;
    if (p + list_len > bl || list_len < 3) return -1;
    size_t cert_len = ((size_t)b[p] << 16) | ((size_t)b[p + 1] << 8) | b[p + 2]; p += 3;
    if (p + cert_len > bl) return -1;

    if (!c->cert_inited) { mbedtls_x509_crt_init(&c->cert); c->cert_inited = 1; }
    if (mbedtls_x509_crt_parse_der(&c->cert, b + p, cert_len) != 0) return -1;
    return 0;
}

/* 客户端：校验 CertificateVerify 签名（对端证书公钥） */
static int verify_certificate_verify(nl_tls13_conn_t* c, const uint8_t* msg, size_t len,
                                     const uint8_t th[32]) {
    uint8_t mt = 0;
    const uint8_t* b = NULL;
    size_t bl = 0, consumed = 0;
    if (nl_tls13_parse_handshake(msg, len, &mt, &b, &bl, &consumed) != 0) return -30;
    if (mt != NL_TLS13_HS_CERTIFICATE_VERIFY || bl < 4) return -31;
    uint16_t scheme = (uint16_t)((b[0] << 8) | b[1]);
    uint16_t sig_len = (uint16_t)((b[2] << 8) | b[3]);
    if (scheme != QUIC_TLS13_SIG_ECDSA_P256_SHA256 || 4 + sig_len > bl) return -32;
    if (!c->cert_inited) return -33;

    uint8_t content[160];
    size_t clen = cv_signed_content(1, th, content);   /* 校验的是服务端 CV → server 上下文 */
    uint8_t hash[32];
    if (nl_tls13_sha256(content, clen, hash) != 0) return -34;
    if (mbedtls_pk_verify(&c->cert.pk, MBEDTLS_MD_SHA256, hash, 32, b + 4, sig_len) != 0) return -35;
    return 0;
}

/* 客户端：证书链校验（有 CA 时） */
static int verify_chain(nl_tls13_conn_t* c) {
    if (!c->ca_inited) { c->peer_verified = 0; return 0; }   /* 无 CA：跳过链校验 */
    uint32_t flags = 0;
    int rc = mbedtls_x509_crt_verify(&c->cert, &c->ca, NULL, NULL, &flags, NULL, NULL);
    if (rc != 0) return -1;
    c->peer_verified = 1;
    return 0;
}

/* ============================================================
 * transcript 辅助
 * ============================================================ */
static void th_update(nl_tls13_conn_t* c, const uint8_t* data, size_t len) {
    mbedtls_sha256_update(&c->th, data, len);
}
static void th_snapshot(nl_tls13_conn_t* c, uint8_t out[32]) {
    mbedtls_sha256_context tmp;
    mbedtls_sha256_clone(&tmp, &c->th);
    mbedtls_sha256_finish(&tmp, out);
    mbedtls_sha256_free(&tmp);
}
static int build_finished(uint8_t msg_type, const uint8_t* base_secret,
                          const uint8_t th[32], uint8_t* out, size_t cap, size_t* out_len) {
    uint8_t fk[32], vd[32];
    if (nl_tls13_finished_key(base_secret, 32, fk) != 0) return -1;
    if (nl_tls13_finished_verify_data(fk, th, 32, vd) != 0) return -1;
    return nl_tls13_encode_handshake(msg_type, vd, 32, out, cap, out_len);
}
static void random_fill(uint8_t out[32]) {
    mbedtls_ctr_drbg_random(&g_drbg, out, 32);
}

/* ============================================================
 * 生命周期 / 证书配置
 * ============================================================ */
nl_tls13_conn_t* nl_tls13_conn_new(int is_server) {
    nl_tls13_conn_t* c = (nl_tls13_conn_t*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->is_server = is_server ? 1 : 0;
    if (tls13_rng_ready() != 0) { free(c); return NULL; }
    mbedtls_sha256_init(&c->th);
    return c;
}

void nl_tls13_conn_free(nl_tls13_conn_t* c) {
    if (!c) return;
    if (c->ecdh_inited) mbedtls_ecdh_free(&c->ecdh);
    mbedtls_sha256_free(&c->th);
    if (c->cert_inited) mbedtls_x509_crt_free(&c->cert);
    if (c->key_inited)  mbedtls_pk_free(&c->key);
    if (c->ca_inited)   mbedtls_x509_crt_free(&c->ca);
    free(c);
}

int nl_tls13_conn_set_cert(nl_tls13_conn_t* c, const char* cert_pem, const char* key_pem) {
    if (!c || !cert_pem || !key_pem) return -1;
    if (tls13_rng_ready() != 0) return -1;
    if (!c->cert_inited) { mbedtls_x509_crt_init(&c->cert); c->cert_inited = 1; }
    if (!c->key_inited)  { mbedtls_pk_init(&c->key); c->key_inited = 1; }
    if (mbedtls_x509_crt_parse(&c->cert, (const unsigned char*)cert_pem,
                               strlen(cert_pem) + 1) != 0) return -1;
    if (mbedtls_pk_parse_key(&c->key, (const unsigned char*)key_pem, strlen(key_pem) + 1,
                             NULL, 0, mbedtls_ctr_drbg_random, &g_drbg) != 0) return -1;
    return 0;
}

int nl_tls13_conn_set_ca(nl_tls13_conn_t* c, const char* ca_pem) {
    if (!c || !ca_pem) return -1;
    if (!c->ca_inited) { mbedtls_x509_crt_init(&c->ca); c->ca_inited = 1; }
    return mbedtls_x509_crt_parse(&c->ca, (const unsigned char*)ca_pem, strlen(ca_pem) + 1);
}

int nl_tls13_conn_peer_verified(const nl_tls13_conn_t* c) {
    return c ? c->peer_verified : 0;
}

/* ============================================================
 * 握手流程
 * ============================================================ */
int nl_tls13_client_hello(nl_tls13_conn_t* c, uint8_t* out, size_t cap, size_t* out_len) {
    if (!c || c->is_server) return -1;
    if (tls13_ecdh_make(c) != 0) return -1;
    random_fill(c->random);
    mbedtls_sha256_starts(&c->th, 0);
    size_t n = 0;
    if (build_client_hello(c, out, cap, &n) != 0) return -1;
    th_update(c, out, n);
    if (out_len) *out_len = n;
    return 0;
}

int nl_tls13_server_handshake(nl_tls13_conn_t* c,
                              const uint8_t* ch, size_t ch_len,
                              uint8_t* out, size_t cap, size_t* out_len) {
    if (!c || !c->is_server || !ch) return -1;
    if (!c->cert_inited || !c->key_inited) return -1;      /* 服务端必须有证书 */

    mbedtls_sha256_starts(&c->th, 0);
    th_update(c, ch, ch_len);

    uint8_t peer_pub[64];
    size_t peer_pub_len = 0;
    if (parse_peer_key_share(ch, ch_len, peer_pub, &peer_pub_len, NULL) != 0) return -1;
    if (tls13_ecdh_make(c) != 0) return -1;
    if (tls13_ecdh_calc(c, peer_pub, peer_pub_len) != 0) return -1;
    random_fill(c->random);
    memcpy(c->alpn, "h3", 3);          /* H3：服务端协商 ALPN = "h3" */

    size_t off = 0, n = 0;
    uint8_t th[32];

    if (build_server_hello(c, out + off, cap - off, &n) != 0) return -1;
    th_update(c, out + off, n); off += n;

    th_snapshot(c, th);                                     /* Hash(CH..SH) */
    if (nl_tls13_handshake_secrets(c->shared, c->shared_len, th, 32,
                                   c->c_hs, c->s_hs, c->master) != 0) return -1;

    if (build_encrypted_extensions(out + off, cap - off, &n) != 0) return -1;
    th_update(c, out + off, n); off += n;

    if (build_certificate(c, out + off, cap - off, &n) != 0) return -1;
    th_update(c, out + off, n); off += n;

    th_snapshot(c, th);                                     /* Hash(CH..Cert) */
    if (build_certificate_verify(c, th, out + off, cap - off, &n) != 0) return -1;
    th_update(c, out + off, n); off += n;

    th_snapshot(c, th);                                     /* Hash(CH..CV) */
    if (build_finished(NL_TLS13_HS_FINISHED, c->s_hs, th, out + off, cap - off, &n) != 0) return -1;
    th_update(c, out + off, n); off += n;

    th_snapshot(c, th);                                     /* Hash(CH..serverFin) */
    if (nl_tls13_application_secrets(c->master, 32, th, 32, c->c_ap0, c->s_ap0) != 0) return -1;

    if (nl_tls13_secret_to_quic_keys(c->c_ap0, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &c->read1) != 0) return -1;
    if (nl_tls13_secret_to_quic_keys(c->s_ap0, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &c->write1) != 0) return -1;

    if (out_len) *out_len = off;
    return 0;
}

/* 阶段 1：处理 ServerHello（导出握手密钥；此后可用 Handshake 级 QUIC 密钥） */
int nl_tls13_client_handshake_1(nl_tls13_conn_t* c, const uint8_t* sh, size_t sh_len) {
    if (!c || c->is_server || !sh) return -1;
    uint8_t th[32];
    size_t n = 0;
    if (nl_tls13_parse_handshake(sh, sh_len, NULL, NULL, NULL, &n) != 0) return -11;
    th_update(c, sh, n);
    (void)parse_alpn(sh, sh_len, 1, c->alpn, sizeof(c->alpn));   /* 客户端：读取服务端 ALPN */
    uint8_t peer_pub[64];
    size_t peer_pub_len = 0;
    if (parse_peer_key_share(sh, sh_len, peer_pub, &peer_pub_len, NULL) != 0) return -12;
    if (tls13_ecdh_calc(c, peer_pub, peer_pub_len) != 0) return -13;
    th_snapshot(c, th);                                     /* Hash(CH..SH) */
    if (nl_tls13_handshake_secrets(c->shared, c->shared_len, th, 32,
                                   c->c_hs, c->s_hs, c->master) != 0) return -14;
    return 0;
}

/* 阶段 2：处理 EE/Cert/CV/serverFinished，产出 client Finished */
int nl_tls13_client_handshake_2(nl_tls13_conn_t* c, const uint8_t* in, size_t in_len,
                                uint8_t* out, size_t cap, size_t* out_len) {
    if (!c || c->is_server || !in) return -1;
    size_t p = 0, n = 0;
    uint8_t th[32];

    /* EncryptedExtensions */
    if (nl_tls13_parse_handshake(in + p, in_len - p, NULL, NULL, NULL, &n) != 0) return -15;
    th_update(c, in + p, n); p += n;

    /* Certificate */
    if (parse_certificate(c, in + p, in_len - p) != 0) return -16;
    if (nl_tls13_parse_handshake(in + p, in_len - p, NULL, NULL, NULL, &n) != 0) return -17;
    th_update(c, in + p, n); p += n;
    if (verify_chain(c) != 0) return -18;

    /* CertificateVerify */
    th_snapshot(c, th);                                     /* Hash(CH..Cert) */
    {
        int r = verify_certificate_verify(c, in + p, in_len - p, th);
        if (r != 0) return r;
    }
    if (nl_tls13_parse_handshake(in + p, in_len - p, NULL, NULL, NULL, &n) != 0) return -20;
    th_update(c, in + p, n); p += n;

    /* server Finished（覆盖 Transcript-Hash(CH..CV)） */
    {
        const uint8_t* fb = NULL; size_t fbl = 0, cons = 0; uint8_t mt = 0;
        if (nl_tls13_parse_handshake(in + p, in_len - p, &mt, &fb, &fbl, &cons) != 0) return -21;
        if (mt != NL_TLS13_HS_FINISHED || fbl != 32) return -22;
        th_snapshot(c, th);                                 /* Hash(CH..CV) */
        uint8_t fk[32], expect[32];
        if (nl_tls13_finished_key(c->s_hs, 32, fk) != 0) return -23;
        if (nl_tls13_finished_verify_data(fk, th, 32, expect) != 0) return -24;
        if (memcmp(expect, fb, 32) != 0) return -25;
        th_update(c, in + p, cons); p += cons;
    }

    th_snapshot(c, th);                                     /* Hash(CH..serverFin) */
    if (nl_tls13_application_secrets(c->master, 32, th, 32, c->c_ap0, c->s_ap0) != 0) return -26;

    if (build_finished(NL_TLS13_HS_FINISHED, c->c_hs, th, out, cap, &n) != 0) return -27;
    th_update(c, out, n);

    if (nl_tls13_secret_to_quic_keys(c->s_ap0, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &c->read1) != 0) return -28;
    if (nl_tls13_secret_to_quic_keys(c->c_ap0, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &c->write1) != 0) return -29;

    c->established = 1;
    if (out_len) *out_len = n;
    return 0;
}

/* 便捷：一次性处理完整服务端 flight（SH + 其余） */
int nl_tls13_client_handshake(nl_tls13_conn_t* c,
                              const uint8_t* in, size_t in_len,
                              uint8_t* out, size_t cap, size_t* out_len) {
    if (!c || c->is_server || !in) return -1;
    size_t n = 0;
    if (nl_tls13_parse_handshake(in, in_len, NULL, NULL, NULL, &n) != 0) return -11;
    int r = nl_tls13_client_handshake_1(c, in, n);
    if (r != 0) return r;
    return nl_tls13_client_handshake_2(c, in + n, in_len - n, out, cap, out_len);
}

int nl_tls13_server_finish(nl_tls13_conn_t* c, const uint8_t* in, size_t in_len) {
    if (!c || !c->is_server || !in) return -1;
    uint8_t mt = 0;
    const uint8_t* fb = NULL;
    size_t fbl = 0, cons = 0;
    if (nl_tls13_parse_handshake(in, in_len, &mt, &fb, &fbl, &cons) != 0) return -1;
    if (mt != NL_TLS13_HS_FINISHED || fbl != 32) return -1;

    uint8_t th[32];
    th_snapshot(c, th);                                     /* Hash(CH..serverFin) */
    uint8_t fk[32], expect[32];
    if (nl_tls13_finished_key(c->c_hs, 32, fk) != 0) return -1;
    if (nl_tls13_finished_verify_data(fk, th, 32, expect) != 0) return -1;
    if (memcmp(expect, fb, 32) != 0) return -1;

    th_update(c, in, cons);
    c->established = 1;
    return 0;
}

int nl_tls13_conn_established(const nl_tls13_conn_t* c) {
    return c ? c->established : 0;
}

const char* nl_tls13_conn_alpn(const nl_tls13_conn_t* c) {
    return (c && c->alpn[0]) ? c->alpn : "";
}

int nl_tls13_conn_get_1rtt_keys(const nl_tls13_conn_t* c,
                                nl_quic_keys_t* read_keys, nl_quic_keys_t* write_keys) {
    if (!c || !c->established) return -1;
    if (read_keys)  *read_keys  = c->read1;
    if (write_keys) *write_keys = c->write1;
    return 0;
}

/* 握手级 QUIC 密钥（SH/CH 后即可用；QUIC 用于 Handshake 包） */
int nl_tls13_conn_get_handshake_keys(const nl_tls13_conn_t* c,
                                     nl_quic_keys_t* read_keys, nl_quic_keys_t* write_keys) {
    if (!c) return -1;
    nl_quic_keys_t ck, sk;
    if (nl_tls13_secret_to_quic_keys(c->c_hs, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &ck) != 0) return -1;
    if (nl_tls13_secret_to_quic_keys(c->s_hs, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &sk) != 0) return -1;
    if (c->is_server) {
        if (read_keys)  *read_keys  = ck;   /* 服务端读客户端握手密钥 */
        if (write_keys) *write_keys = sk;
    } else {
        if (read_keys)  *read_keys  = sk;   /* 客户端读服务端握手密钥 */
        if (write_keys) *write_keys = ck;
    }
    return 0;
}

/* ============================================================
 * 自签证书生成（测试/示例）
 * ============================================================ */
int nl_tls13_gen_self_signed(char* cert_pem, size_t cert_cap,
                             char* key_pem, size_t key_cap) {
    if (!cert_pem || !key_pem) return -1;
    if (tls13_rng_ready() != 0) return -1;

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) { mbedtls_pk_free(&key); return -1; }
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                            mbedtls_ctr_drbg_random, &g_drbg) != 0) { mbedtls_pk_free(&key); return -1; }

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    mbedtls_mpi_lset(&serial, 1);

    int rc = 0;
    do {
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        if (mbedtls_x509write_crt_set_subject_name(&crt, "CN=NetLeaf QUIC-TLS") != 0) { rc = -1; break; }
        if (mbedtls_x509write_crt_set_issuer_name(&crt, "CN=NetLeaf QUIC-TLS") != 0) { rc = -1; break; }
        if (mbedtls_x509write_crt_set_serial(&crt, &serial) != 0) { rc = -1; break; }
        if (mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20360101000000") != 0) { rc = -1; break; }
        if (mbedtls_x509write_crt_set_basic_constraints(&crt, 1, 0) != 0) { rc = -1; break; }

        unsigned char der[2048];
        int der_len = mbedtls_x509write_crt_der(&crt, der, sizeof(der),
                                                mbedtls_ctr_drbg_random, &g_drbg);
        if (der_len < 0) { rc = -1; break; }
        unsigned char* der_p = der + (sizeof(der) - (size_t)der_len);

        size_t olen = 0;
        if (mbedtls_pem_write_buffer("-----BEGIN CERTIFICATE-----\n",
                                     "-----END CERTIFICATE-----\n",
                                     der_p, (size_t)der_len,
                                     (unsigned char*)cert_pem, cert_cap, &olen) != 0) { rc = -1; break; }
        if (mbedtls_pk_write_key_pem(&key, (unsigned char*)key_pem, key_cap) != 0) { rc = -1; break; }
    } while (0);

    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    return rc;
}

/* ============================================================
 * 自测：客户端 ↔ 服务端 端到端握手（含证书认证，进程内）
 * ============================================================ */
int nl_quictls_handshake_selftest(void) {
    static char cert_pem[4096];
    static char key_pem[4096];
    uint8_t buf_ch[512], buf_sf[2048], buf_cf[256];
    size_t ch_len = 0, sf_len = 0, cf_len = 0;

    if (nl_tls13_gen_self_signed(cert_pem, sizeof(cert_pem), key_pem, sizeof(key_pem)) != 0) return 1;

    nl_tls13_conn_t* cli = nl_tls13_conn_new(0);
    nl_tls13_conn_t* srv = nl_tls13_conn_new(1);
    if (!cli || !srv) { nl_tls13_conn_free(cli); nl_tls13_conn_free(srv); return 2; }

    int rc = 0;
    if (nl_tls13_conn_set_cert(srv, cert_pem, key_pem) != 0) { rc = 3; goto done; }
    if (nl_tls13_conn_set_ca(cli, cert_pem) != 0) { rc = 4; goto done; }

    if (nl_tls13_client_hello(cli, buf_ch, sizeof(buf_ch), &ch_len) != 0) { rc = 5; goto done; }
    if (ch_len < 4 || buf_ch[0] != NL_TLS13_HS_CLIENT_HELLO) { rc = 6; goto done; }

    if (nl_tls13_server_handshake(srv, buf_ch, ch_len, buf_sf, sizeof(buf_sf), &sf_len) != 0) { rc = 7; goto done; }

    if (nl_tls13_client_handshake(cli, buf_sf, sf_len, buf_cf, sizeof(buf_cf), &cf_len) != 0) { rc = 8; goto done; }
    if (!nl_tls13_conn_established(cli)) { rc = 9; goto done; }
    if (!nl_tls13_conn_peer_verified(cli)) { rc = 10; goto done; }   /* 证书链 + CV 校验通过 */
    if (strcmp(nl_tls13_conn_alpn(cli), "h3") != 0) { rc = 30; goto done; }   /* ALPN 协商 h3 */

    if (nl_tls13_server_finish(srv, buf_cf, cf_len) != 0) { rc = 11; goto done; }
    if (!nl_tls13_conn_established(srv)) { rc = 12; goto done; }
    if (strcmp(nl_tls13_conn_alpn(srv), "h3") != 0) { rc = 31; goto done; }

    /* 两端 1-RTT 密钥应互补一致 */
    nl_quic_keys_t cr, cw, sr, sw;
    if (nl_tls13_conn_get_1rtt_keys(cli, &cr, &cw) != 0) { rc = 13; goto done; }
    if (nl_tls13_conn_get_1rtt_keys(srv, &sr, &sw) != 0) { rc = 14; goto done; }
    if (memcmp(cr.key, sw.key, 16) != 0 || memcmp(cr.iv, sw.iv, 12) != 0) { rc = 15; goto done; }
    if (memcmp(cw.key, sr.key, 16) != 0 || memcmp(cw.iv, sr.iv, 12) != 0) { rc = 16; goto done; }

    /* 用协商出的 1-RTT 密钥做一次短首部包保护往返（客户端写 → 服务端读） */
    {
        uint8_t pkt[128];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x43; pkt[1] = 0x05;
        for (size_t i = 0; i < 20; i++) pkt[2 + i] = (uint8_t)(0x70 + i);
        uint8_t orig[20];
        memcpy(orig, pkt + 2, 20);
        size_t tot = 0, pln = 0;
        if (nl_quic_protect_packet_ex(&cw, 5, 0, pkt, 1, 1, 20, &tot) != 0) { rc = 17; goto done; }
        if (nl_quic_unprotect_packet_ex(&sr, 5, 0, pkt, 1, 1, 20, &pln) != 0) { rc = 18; goto done; }
        if (pln != 20 || memcmp(pkt + 2, orig, 20) != 0) { rc = 19; goto done; }
    }

done:
    nl_tls13_conn_free(cli);
    nl_tls13_conn_free(srv);
    return rc;
}
