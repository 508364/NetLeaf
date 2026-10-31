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

#include "netleaf_quic.h"
#include "quictls_internal.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/sha256.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pem.h>
#include <mbedtls/bignum.h>

#define QUIC_TLS13_X25519_GROUP          0x001d   /* X25519 group ID（RFC 8446 附录 A：值 29） */
#define TLS13_GRP_X25519                 0x001d
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
#define NL_TLS13_TP_MAX 256

struct nl_tls13_conn {
    int    is_server;
    int    established;
    int    peer_verified;

    uint8_t random[32];
    char    alpn[16];            /* 协商出的 ALPN（H3 用 "h3"） */

    mbedtls_ecdh_context ecdh;
    int     ecdh_inited;
    uint8_t our_pub[72];       /* mbedtls make_public 原始 ECPoint 编码（1 长度字节 + 数据）；
                                  P-521 未压缩=68，取 72 留余量 */
    size_t  our_pub_len;
    uint8_t raw_pub[72];       /* 归一化后的 RFC 8446 key_share 编码（Mont=原始 N 字节 / ECC=压缩点） */
    size_t  raw_pub_len;
    uint16_t sel_grp;           /* 本端协商出的 selected_group（写 key_share 用，默认 X25519） */
    uint8_t shared[64];
    size_t  shared_len;

    mbedtls_sha256_context th;   /* transcript hash（运行中） */

    uint8_t c_hs[32], s_hs[32], master[32];
    uint8_t c_ap0[32], s_ap0[32];
    uint8_t server_fin_th[32];   /* Hash(CH..CV..SFin) 快照（不含 CFin），供 server_finished_msg 使用 */

    nl_quic_keys_t read1, write1;

    /* 证书 / 私钥（服务端为本方证书链 + 私钥；客户端为对端证书链） */
    mbedtls_x509_crt cert;
    int              cert_inited;
    mbedtls_pk_context key;
    int              key_inited;
    mbedtls_x509_crt ca;
    int              ca_inited;

    /* QUIC 传输参数（RFC 9000 §18 / TLS 扩展 0x0039）：
     * 必须出现在 ClientHello 与 ServerHello 中，aioquic 等真实 QUIC 栈会
     * 校验其存在；缺失将导致对端拒绝 ServerHello、无法推进握手。 */
    uint8_t  tp[NL_TLS13_TP_MAX];
    size_t   tp_len;

    /* 客户端 ODCID（initial_source_connection_id，TLS 传输参数 0x00）：
     * 服务端在 nl_tls13_server_handshake 前通过 nl_tls13_conn_set_odcid 注入，
     * tp_build_minimal 据此填充 initial_source_connection_id 传输参数。 */
    uint8_t  odcid[20];
    size_t   odcid_len;
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
static uint16_t ecdh_to_group(mbedtls_ecp_group_id grp);
static int  is_montgomery_grp(mbedtls_ecp_group_id g);
static int  normalize_pub(mbedtls_ecp_group_id grp, const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t* out_len);
static int  denormalize_pub(mbedtls_ecp_group_id grp, const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t* out_len);

static int tls13_ecdh_make(nl_tls13_conn_t* c, mbedtls_ecp_group_id grp) {
    mbedtls_ecdh_init(&c->ecdh);
    c->ecdh_inited = 1;
    int s1 = mbedtls_ecdh_setup(&c->ecdh, grp);
#ifdef NLH3_DBG
    fprintf(stderr, "[h3-tls] ecdh_make: setup grp=%d rc=%d\n", (int)grp, s1); fflush(stderr);
#endif
    if (s1 != 0) return -1;
    size_t olen = 0;
    if (is_montgomery_grp(grp)) {
        /* Mont（X25519/X448）：make_public 输出即原始 N 字节 key_share，
         * normalize 剥掉 1 个长度字节即可 */
        int s2 = mbedtls_ecdh_make_public(&c->ecdh, &olen, c->our_pub, sizeof(c->our_pub),
                                         mbedtls_ctr_drbg_random, &g_drbg);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ecdh_make: make_public(mont) grp=%d rc=%d olen=%zu\n",
                (int)grp, s2, olen); fflush(stderr);
#endif
        if (s2 != 0) return -1;
        c->our_pub_len = olen;
        int s3 = normalize_pub(grp, c->our_pub, c->our_pub_len, c->raw_pub, &c->raw_pub_len);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ecdh_make: normalize(mont) rc=%d raw_len=%zu\n", s3, c->raw_pub_len); fflush(stderr);
#endif
        if (s3 != 0) return -1;
        return 0;
    }
    /* ECC（P-256/384/521）：make_public 输出 ECPoint 形式（1 长度字节 + X9.62 点），
     * X9.62 点即 RFC 8446 ECDH key_share 编码（未压缩 65/97/133B）。
     * 直接剥掉 1 个长度字节写入 raw_pub，保持点数据（our_pub_len 复用其值）。 */
    {
        int s2 = mbedtls_ecdh_make_public(&c->ecdh, &olen, c->our_pub, sizeof(c->our_pub),
                                         mbedtls_ctr_drbg_random, &g_drbg);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ecdh_make: make_public(ecc) grp=%d rc=%d olen=%zu\n",
                (int)grp, s2, olen); fflush(stderr);
#endif
        if (s2 != 0) return -1;
        if (olen < 2) return -1;            /* ECPoint：1 长度字节 + 至少 1 数据字节 */
        c->our_pub_len = olen;
        c->raw_pub_len = olen - 1;          /* 剥长度字节 */
        memcpy(c->raw_pub, c->our_pub + 1, olen - 1);
    }
    return 0;
}

static int tls13_ecdh_calc(nl_tls13_conn_t* c, const uint8_t* peer_pub, size_t peer_len,
                           mbedtls_ecp_group_id grp) {
    /* 若未初始化则按本组 setup；已初始化则保留 make 阶段生成的私钥 d，
     * 不再 free+重建（free 会清零 d，导致 calc 失败）。
     * read_public 写入对端 Qp，与私有 d 独立，不影响后续 calc。 */
    if (!c->ecdh_inited) {
        if (mbedtls_ecdh_setup(&c->ecdh, grp) != 0) return -1;
        c->ecdh_inited = 1;
    }
    /* 对端 key_share 为 RFC 8446 编码（Mont=原始 N 字节 / ECC=压缩点），
     * mbedtls read_public 期望 ECPoint（1 长度字节 + 数据），先反归一化 */
    {
        uint8_t ecpt[72];
        size_t ecpt_len = 0;
        int c1 = denormalize_pub(grp, peer_pub, peer_len, ecpt, &ecpt_len);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ecdh_calc: denormalize rc=%d ecpt_len=%zu peer_len=%zu peer[0]=0x%02x\n",
                c1, ecpt_len, peer_len, peer_len ? peer_pub[0] : 0); fflush(stderr);
#endif
        if (c1 != 0) return -1;
        int c2 = mbedtls_ecdh_read_public(&c->ecdh, ecpt, ecpt_len);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ecdh_calc: read_public rc=%d\n", c2); fflush(stderr);
#endif
        if (c2 != 0) return -1;
        size_t olen = 0;
        int c3 = mbedtls_ecdh_calc_secret(&c->ecdh, &olen, c->shared, sizeof(c->shared),
                                          mbedtls_ctr_drbg_random, &g_drbg);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ecdh_calc: calc_secret rc=%d olen=%zu\n", c3, olen); fflush(stderr);
#endif
        if (c3 != 0) return -1;
        c->shared_len = olen;
    }
    /* 无条件 dump：ECDHE shared secret（X25519 原始 N 字节）供离线 oracle 独立复算 HS 密钥 */
    {
        FILE* fe = fopen("dump/srv_ecdh_shared.txt", "w");
        if (fe) {
            fprintf(fe, "grp=0x%04x\nshared_len=%zu\nshared=", c->sel_grp, c->shared_len);
            for (size_t i = 0; i < c->shared_len; i++) fprintf(fe, "%02x", c->shared[i]);
            fprintf(fe, "\n");
            fclose(fe);
        }
    }
    return 0;
}

/* ============================================================
 * 握手消息构造
 * ============================================================ */
/* ============================================================
 * QUIC 传输参数（RFC 9000 §18，TLS 扩展 0x0039）
 * 最小合法 TLV 集合：initial_max_data / initial_max_stream_data_bidi_local
 * / _bidi_remote / _uni / initial_max_streams_bidi /
 * initial_max_streams_uni / idle_timeout
 * 编码：{ type:varint, length:varint, value:opaque }
 * 字段编号：0x01 max_data, 0x04 bidi_local, 0x05 bidi_remote,
 *           0x06 uni, 0x07 streams_bidi, 0x08 streams_uni
 * ============================================================ */
/* RFC 9000 传输参数：每个 TLV = type(varint) + length(varint) + value。
 * value 数字型参数必须按 QUIC varint 编码（2^20=1048576 → 0x80 0x01 0x00 0x00，
 * 2^16=65536 → 0x80 0x04 0x00 0x00），而非大端固定宽度。
 *
 * 参数集合须含 aioquic 校验所需项：max_udp_payload_size（type 0x03，数值 1350）。
 * 字段编号：0x01 max_idle_timeout, 0x03 max_udp_payload_size,
 *           0x04 initial_max_data, 0x05 initial_max_stream_data_bidi_local,
 *           0x06 initial_max_stream_data_bidi_remote,
 *           0x07 initial_max_stream_data_uni, 0x08 initial_max_streams_bidi,
 *           0x09 initial_max_streams_uni */
static void tp_build_minimal(uint8_t* out, size_t cap, size_t* out_len) {
    (void)cap;
    size_t p = 0;
    /*
     * RFC 9000 传输参数（必须按 type 升序）：
     *  0x00 initial_source_connection_id：客户端 ODCID 原始字节（varint 长度前缀）
     *  0x01 max_idle_timeout
     *  0x04 initial_max_data
     *  0x05 initial_max_stream_data_bidi_local
     *  0x06 initial_max_stream_data_bidi_remote
     *  0x07 initial_max_stream_data_uni
     *  0x08 initial_max_streams_bidi
     *  0x09 initial_max_streams_uni
     *  0x2f max_udp_payload_size
     *
     * 0x00 的值是 CID 本身（不是 varint 数字），编码为：
     *   type(0x00) || u64_varint(len) || CID bytes
     * 其余数字型参数：type || u64_varint(len=varint 字节数) || varint bytes
     */
    /* 0x00 initial_source_connection_id（先写，因为 type 最小，且必须在最前） */
    /* 由调用方在 out 起始处直接填，这里跳过（tp_build_minimal 无 c 指针，
       改用 tp_ensure 时再填，见下方 tp_ensure 修改） */
    uint64_t vals[]  = { 1048576, 1350, 65536, 65536, 1048576, 256, 0, 65536 };
    uint8_t types[] = { 0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09 };
    for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
        uint8_t vp[8]; size_t vn = nl_quic_varint_write(vp, 8, vals[i]);
        if (vn == 0) continue;
        out[p++] = types[i];
        out[p++] = (uint8_t)vn;          /* length varint：vn∈{1,4}，直接 1 字节 */
        memcpy(out + p, vp, vn); p += vn;
    }
    *out_len = p;
}

static void tp_ensure(nl_tls13_conn_t* c) {
    if (c->tp_len == 0) {
        /*
         * RFC 9000 传输参数编号：
         *  0x00 original_destination_connection_id = 客户端 ODCID
         *  0x01 max_idle_timeout
         *  0x03 max_udp_payload_size
         *  0x04 initial_max_data
         *  0x05 initial_max_stream_data_bidi_local
         *  0x06 initial_max_stream_data_bidi_remote
         *  0x07 initial_max_stream_data_uni
         *  0x08 initial_max_streams_bidi
         *  0x09 initial_max_streams_uni
         *  0x0F initial_source_connection_id = 服务端 SCID（当前 SCID=NULL，值=空）
         */
        uint8_t buf[NL_TLS13_TP_MAX];
        size_t off = 0;

        /* 0x00 original_destination_connection_id（客户端 ODCID） */
        if (c->odcid_len > 0) {
            buf[off++] = 0x00;
            uint8_t v[8]; size_t vn = nl_quic_varint_write(v, 8, c->odcid_len);
            memcpy(buf + off, v, vn); off += vn;
            memcpy(buf + off, c->odcid, c->odcid_len); off += c->odcid_len;
        }
        /* 其余数字型参数（0x01,0x04..0x09,0x2f） */
        size_t rest_len;
        tp_build_minimal(buf + off, NL_TLS13_TP_MAX - off, &rest_len);
        off += rest_len;
        /* 0x0F initial_source_connection_id（服务端 SCID，当前为空） */
        buf[off++] = 0x0F;
        buf[off++] = 0x00;  /* varint(0) = 空 CID */

        memcpy(c->tp, buf, off);
        c->tp_len = off;
    }
}

static int build_client_hello(nl_tls13_conn_t* c, uint8_t* out, size_t cap, size_t* out_len) {
    uint8_t body[300];
    wbuf_t w = { body, sizeof(body), 0, 0 };

    w_u16(&w, 0x0303);
    w_bytes(&w, c->random, 32);
    w_u8(&w, 0);                       /* legacy_session_id_len = 0 */
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

    /* supported_groups（RFC 8446 §4.2.4）：ext(0x000a) + ext_len(0) +
     * u16 list_len(2) + selected_group 共 4 字节；须与 key_share 的组一致 */
    w_u16(&w, 0x000a); w_u16(&w, 4);
    w_u16(&w, 2);
    w_u16(&w, c->sel_grp ? c->sel_grp : TLS13_GRP_X25519);

    /* key_share（RFC 8446 §4.2.4）ClientHello：
     * ext(0x0033) + ext_len + u16 list_len + { group(u16) + opaque key(u16 len) }
     * list_len = 2(grp)+2(klen)+key；ext_len = 2(list_len)+list_len = 6+key */
    uint16_t sel_grp = c->sel_grp ? c->sel_grp : TLS13_GRP_X25519;
    w_u16(&w, 0x0033);
    w_u16(&w, (uint16_t)(2 + 2 + 2 + c->raw_pub_len));   /* ext_len = 2+list_len */
    w_u16(&w, (uint16_t)(2 + 2 + c->raw_pub_len));       /* list_len = grp+klen+key */
    w_u16(&w, sel_grp);                                   /* selected_group */
    w_u16(&w, (uint16_t)c->raw_pub_len);                  /* opaque key len */
    w_bytes(&w, c->raw_pub, c->raw_pub_len);             /* key（归一化编码） */

    /* QUIC 传输参数（RFC 9000 §18，TLS 扩展 0x0039）：
     * RFC 8446 §4.2：扩展内容为 binary vector，直接是 TP blob（其自身已含
     * 外层长度 varint），*不* 需额外 u16 长度前缀。ext_len = tp_len。
     * 旧实现多写一个 w_u16(tp_len)，导致整个扩展错位、aioquic 拒收 SH。 */
    tp_ensure(c);
    w_u16(&w, 0x0039);
    w_u16(&w, (uint16_t)c->tp_len);        /* ext_len = tp blob 长度 */
    w_bytes(&w, c->tp, c->tp_len);        /* TP blob（RFC 9000 varint 编码） */

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
    w_u8(&w, 0);                       /* legacy_session_id_len = 0 */
    w_u16(&w, 0x1301);
    w_u8(&w, 0);

    size_t ext_len_pos = w.len; w_u16(&w, 0);
    size_t ext_start = w.len;

    w_u16(&w, 0x002b); w_u16(&w, 2); w_u16(&w, 0x0304);

    w_u16(&w, 0x0010); w_u16(&w, 3);                           /* ALPN：选中 "h3" */
    w_u8(&w, 2); w_bytes(&w, (const uint8_t*)"h3", 2);

    /* supported_groups（RFC 8446 §4.2.4）：ext(0x000a) + ext_len(0) +
     * u16 list_len(2) + selected_group 共 4 字节；须与 key_share 的组一致 */
    w_u16(&w, 0x000a); w_u16(&w, 4);
    w_u16(&w, 2);
    w_u16(&w, c->sel_grp ? c->sel_grp : TLS13_GRP_X25519);

    /* key_share（RFC 8446 §4.2.6）ServerHello =
     * ext(0x0033) + ext_len + { selected_group(u16) + opaque key <u16 len + bytes> }
     * key = raw_pub（归一化编码；X25519=32B 原始，ECC=X9.62 点 65B） */
    w_u16(&w, 0x0033);
    w_u16(&w, (uint16_t)(2 + 2 + c->raw_pub_len));          /* ext_len = grp(2)+klen(2)+key */
    w_u16(&w, c->sel_grp);                                    /* selected_group */
    w_u16(&w, (uint16_t)c->raw_pub_len);                       /* opaque key len */
    w_bytes(&w, c->raw_pub, c->raw_pub_len);                  /* key */

    /* QUIC 传输参数（RFC 9000 §18，TLS 扩展 0x0039）：
     * RFC 8446 §4.2：扩展内容为 binary vector，直接是 TP blob（其自身已含
     * 外层长度 varint），*不* 需额外 u16 长度前缀。ext_len = tp_len。
     * 旧实现多写一个 w_u16(tp_len)，导致整个扩展错位、aioquic 拒收 SH。 */
    tp_ensure(c);
    w_u16(&w, 0x0039);
    w_u16(&w, (uint16_t)c->tp_len);        /* ext_len = tp blob 长度 */
    w_bytes(&w, c->tp, c->tp_len);        /* TP blob（RFC 9000 varint 编码） */

    uint16_t ext_len = (uint16_t)(w.len - ext_start);
    body[ext_len_pos]     = (uint8_t)(ext_len >> 8);
    body[ext_len_pos + 1] = (uint8_t)ext_len;

    if (w.err) return -1;
    return nl_tls13_encode_handshake(NL_TLS13_HS_SERVER_HELLO, body, w.len, out, cap, out_len);
}

static int build_encrypted_extensions(nl_tls13_conn_t* c, uint8_t* out, size_t cap, size_t* out_len) {
    /* RFC 9001 §4.4：服务器 MUST 在 EncryptedExtensions 中携带自身 QUIC 传输
     * 参数（0x0039 扩展，ext_len = TP blob 长度，blob 自身已含 RFC 9000 varint
     * 外层长度）。客户端（aioquic 等）依赖此扩展学习服务端
     * initial_max_data / max_idle_timeout / max_udp_payload_size，缺失则
     * 直接 abort，导致本端 E2E 永远收不到客户端的 Finished。 */
    uint8_t body[512];
    wbuf_t w = { body, sizeof(body), 0, 0 };

    size_t ext_len_pos = w.len; w_u16(&w, 0);

    /* ALPN 扩展（0x0010）：服务端在 EncryptedExtensions 中通告选定的协议。
     * 格式：ext_type=0x0010, ext_len=3, 内容 = u8 len(2) + "h3"。
     * 与 build_server_hello 中 SH 的 ALPN 编码保持一致。
     * aioquic 的 _client_handle_encrypted_extensions 依赖 EE 中的 ALPN
     * 扩展设置 alpn_negotiated，缺失则 alpn_negotiated=None，
     * 后续 H3 send_headers 触发 ValueError。 */
    w_u16(&w, 0x0010); w_u16(&w, 3);
    w_u8(&w, 2); w_bytes(&w, (const uint8_t*)"h3", 2);

    tp_ensure(c);
    w_u16(&w, 0x0039);
    w_u16(&w, (uint16_t)c->tp_len);        /* ext_len = TP blob 长度 */
    w_bytes(&w, c->tp, c->tp_len);        /* TP blob（RFC 9000 varint 编码） */

    uint16_t ext_len = (uint16_t)(w.len - ext_len_pos - 2);
    body[ext_len_pos]     = (uint8_t)(ext_len >> 8);
    body[ext_len_pos + 1] = (uint8_t)ext_len;

    if (w.err) return -1;
    return nl_tls13_encode_handshake(NL_TLS13_HS_ENCRYPTED_EXTENSIONS, body, w.len, out, cap, out_len);
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

#ifdef NLH3_DBG
    /* self-verify: confirm the signature validates against c->cert.pub_key */
    {
        int vrc = mbedtls_pk_verify(&c->cert.pk, MBEDTLS_MD_SHA256, hash, 32, sig, sig_len);
        uint8_t th_hex[64];
        for (int i = 0; i < 32; i++) sprintf((char*)th_hex + i*2, "%02x", th[i]);
        fprintf(stderr, "[h3-tls] CV self-verify: th=%s vrc=%d sig_len=%zu\n",
                (char*)th_hex, vrc, sig_len);
        fflush(stderr);
    }
#endif

    uint8_t body[300];
    wbuf_t w = { body, sizeof(body), 0, 0 };
    w_u16(&w, QUIC_TLS13_SIG_ECDSA_P256_SHA256);
    w_u16(&w, (uint16_t)sig_len);
    w_bytes(&w, sig, sig_len);
    if (w.err) return -1;
    return nl_tls13_encode_handshake(NL_TLS13_HS_CERTIFICATE_VERIFY, body, w.len, out, cap, out_len);
}

/* group → mbedtls ECDH 引擎：支持 X25519/P-256/P-384/P-521（RFC 8446 §4.2.7）。
 * 任一组可协商即可互通；非可协商组返回 0 跳过。 */
static int group_to_ecdh(uint16_t grp, mbedtls_ecp_group_id* out, size_t* key_len) {
    /* RFC 8446 附录 A 准确组号：P-256=0x0017 P-384=0x0018 P-521=0x0019 X25519=0x001d X448=0x001e
     * key_len 为对端 key_share 的字节数：
     *  - ECC 用 X9.62 未压缩点（0x04‖X‖Y）：P-256=65 / P-384=97 / P-521=133
     *  - Mont 用原始 N 字节：X25519=32 / X448=56 */
    switch (grp) {
        case 0x0017: *out = MBEDTLS_ECP_DP_SECP256R1;  *key_len = 65; break;
        case 0x0018: *out = MBEDTLS_ECP_DP_SECP384R1;  *key_len = 97; break;
        case 0x0019: *out = MBEDTLS_ECP_DP_SECP521R1;  *key_len = 133; break;
        case 0x001d: *out = MBEDTLS_ECP_DP_CURVE25519; *key_len = 32; break;
        case 0x001e: *out = MBEDTLS_ECP_DP_CURVE448;   *key_len = 56; break;
        default: return 0;
    }
    return 1;
}
/* mbedtls_ecp_group_id → TLS 1.3 组号（RFC 8446 附录 A） */
static uint16_t ecdh_to_group(mbedtls_ecp_group_id grp) {
    switch (grp) {
        case MBEDTLS_ECP_DP_CURVE25519: return 0x001d;
        case MBEDTLS_ECP_DP_SECP256R1:  return 0x0017;
        case MBEDTLS_ECP_DP_SECP384R1:  return 0x0018;
        case MBEDTLS_ECP_DP_SECP521R1:  return 0x0019;
        case MBEDTLS_ECP_DP_CURVE448:   return 0x001e;
        default: return 0;
    }
}

/* 该组 key_share 是否为 MONTGOMERY（X25519/X448）。
 * RFC 8446 §4.2.1：Montgomery 曲线 key_share 为原始 N 字节（不带长度前缀）。
 * RFC 4492 §4.2.2：ECC 曲线为压缩点（1 长度字节 + 数据，mbedTLS make_public 即此格式）。 */
static int is_montgomery_grp(mbedtls_ecp_group_id g) {
    return g == MBEDTLS_ECP_DP_CURVE25519 || g == MBEDTLS_ECP_DP_CURVE448;
}

/* 把 mbedtls 产出的公钥（make_public 的 ECPoint 格式：1 长度字节 + 数据）
 * 归一化为 RFC 8446 约定的 key_share 编码，写入 out（容量 64）。
 *  - MONTGOMERY：剥掉长度字节，输出原始 N 字节；
 *  - ECC：保留压缩点（1 长度字节 + 数据），原样输出。 */
static int normalize_pub(mbedtls_ecp_group_id grp, const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t* out_len) {
    if (!in || !out || !out_len) return -1;
    if (is_montgomery_grp(grp)) {
        if (in_len < 2) return -1;
        size_t n = in_len - 1;                     /* 去掉 ECPoint 长度字节 */
        if (n > 72) return -1;
        memcpy(out, in + 1, n);
        *out_len = n;
    } else {
        if (in_len > 72) return -1;
        memcpy(out, in, in_len);                  /* 压缩点原样 */
        *out_len = in_len;
    }
    return 0;
}

/* 把对方 key_share 编码归一化为 mbedtls read_public 期望的 ECPoint 格式。
 *  - MONTGOMERY：补 1 个长度字节；
 *  - ECC：key_share 是 X9.62 点（未压缩 0x04‖X‖Y），read_public 期望
 *    "1 长度字节 + 数据"（data_len 即 X9.62 字节数），故也补 1 个长度字节。 */
static int denormalize_pub(mbedtls_ecp_group_id grp, const uint8_t* in, size_t in_len,
                           uint8_t* out, size_t* out_len) {
    if (!in || !out || !out_len) return -1;
    if (is_montgomery_grp(grp)) {
        if (in_len > 71) return -1;
        out[0] = (uint8_t)in_len;
        memcpy(out + 1, in, in_len);
        *out_len = in_len + 1;
    } else {
        /* ECC：补 1 个 ECPoint 长度字节，后跟 X9.62 点数据（未压缩 65/97/133B） */
        if (in_len > 71) return -1;
        out[0] = (uint8_t)in_len;
        memcpy(out + 1, in, in_len);
        *out_len = in_len + 1;
    }
    return 0;
}

static int parse_peer_key_share(const uint8_t* hs, size_t len,
                                uint8_t out_pub[72], size_t* out_pub_len,
                                uint16_t* out_grp) {
    uint8_t mt = 0;
    const uint8_t* b = NULL;
    size_t bl = 0, consumed = 0;
    {
        int prc = nl_tls13_parse_handshake(hs, len, &mt, &b, &bl, &consumed);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ppks: parse_hs rc=%d len=%zu mt=0x%02x bl=%zu\n",
                prc, len, (unsigned)mt, bl); fflush(stderr);
#endif
        if (prc != 0) return -1;
    }
    if (out_grp) *out_grp = 0;
    if (mt != NL_TLS13_HS_CLIENT_HELLO && mt != NL_TLS13_HS_SERVER_HELLO) {
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ppks: FAIL mt=0x%02x not CH/SH\n", (unsigned)mt); fflush(stderr);
#endif
        return -1;
    }
    if (bl < 2 + 32 + 1) {
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ppks: FAIL bl=%zu < 35\n", bl); fflush(stderr);
#endif
        return -1;
    }

    /* 跳过 legacy_version(2) + random(32) + session_id(1+len) */
    size_t p = 2 + 32;
    size_t sid = b[p]; p += 1 + sid;
    if (p > bl) {
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ppks: FAIL sid skip p=%zu > bl=%zu\n", p, bl); fflush(stderr);
#endif
        return -1;
    }

    if (mt == NL_TLS13_HS_CLIENT_HELLO) {
        /* ciphersuites(u16 count + count*2B) + compression(1B count + count*1B) */
        if (p + 2 > bl) return -1;
        size_t cs = ((size_t)b[p] << 8) | b[p + 1]; p += 2 + cs;
        if (p + 1 > bl) return -1;
        size_t cm = b[p]; p += 1;          /* skip count byte */
        if (p + cm > bl) return -1;
        p += cm;                            /* skip cm method bytes */
    } else {
        /* legacy_version 已跳，ciphersuite(u16) + compression(u8) */
        p += 2 + 1;
    }

    /* extensions(u16 total_len) + 若干 {type(u16) len(u16) data} */
    if (p + 2 > bl) return -1;
    size_t ext_total = ((size_t)b[p] << 8) | b[p + 1]; p += 2;
    size_t ext_end = p + ext_total;
    if (ext_end > bl) return -1;

#ifdef NLH3_DBG
    fprintf(stderr, "[h3-tls] ppks: mt=0x%02x bl=%zu p=%zu ext_total=%zu ext_end=%zu\n",
            (unsigned)mt, bl, p, ext_total, ext_end);
    /* dump ext_total 前后 8 字节 */
    fprintf(stderr, "[h3-tls] ppks: b[%zu..%zu]=", p - 2, p + 5);
    for (size_t i = p - 2; i < p + 6 && i < bl; i++) fprintf(stderr, " %02x", b[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
#endif

    while (p + 4 <= ext_end) {
        uint16_t et = (uint16_t)((b[p] << 8) | b[p + 1]);
        uint16_t el = (uint16_t)((b[p + 2] << 8) | b[p + 3]);
        p += 4;
        if (p + el > ext_end) return -1;

#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] ppks: ext=0x%04x el=%u p=%zu\n", et, (unsigned)el, p);
        fflush(stderr);
#endif

        if (et == 0x0033) {
            const uint8_t* kd = b + p;
            size_t kr = el;
            uint16_t grp = 0;
#ifdef NLH3_DBG
            fprintf(stderr, "[h3-tls] ext0x33 el=%zu raw=", el);
            for (size_t i = 0; i < el && i < 20; i++) fprintf(stderr, " %02x", kd[i]);
            fprintf(stderr, "\n"); fflush(stderr);
#endif

            if (mt == NL_TLS13_HS_CLIENT_HELLO) {
                /* RFC 8446 §4.2.4 ClientHello key_share 扩展：
                 * u16 list_len + { KeyShareEntry[] }
                 * KeyShareEntry = u16 group + opaque key <1..2^16-1>
                 * （opaque 带 u16 长度前缀；ECC 为 X9.62 点 65/97/133B，Mont 为 N 字节） */
                if (kr < 2) return -1;
                kr = ((size_t)kd[0] << 8) | kd[1];      /* list_len（entries 区总长） */
                kd += 2;
                size_t list_end = kr;

                /* 遍历 KeyShareEntry，取第一个可协商组 */
                while (list_end >= 2 + 2) {
                    uint16_t sg = (uint16_t)((kd[0] << 8) | kd[1]);
                    size_t klen = ((size_t)kd[2] << 8) | kd[3];  /* opaque 长度前缀 */
                    if (kr < 2 + 2 + klen) {
#ifdef NLH3_DBG
                        fprintf(stderr, "[h3-tls] ppks: FAIL: kr=%zu < 4+klen=%zu\n",
                                kr, 4 + klen);
                        fflush(stderr);
#endif
                        return -1;
                    }
                    const uint8_t* key = kd + 4;
#ifdef NLH3_DBG
                    fprintf(stderr, "[h3-tls] ppks: entry grp=0x%04x klen=%zu\n",
                            sg, klen);
                    fflush(stderr);
#endif

                    mbedtls_ecp_group_id eng = MBEDTLS_ECP_DP_NONE;
                    size_t key_len = 0;
                    if (group_to_ecdh(sg, &eng, &key_len) &&
                        (eng == MBEDTLS_ECP_DP_CURVE25519  ||
                         eng == MBEDTLS_ECP_DP_CURVE448    ||
                         eng == MBEDTLS_ECP_DP_SECP256R1   ||
                         eng == MBEDTLS_ECP_DP_SECP384R1   ||
                         eng == MBEDTLS_ECP_DP_SECP521R1)   &&
                        klen == key_len) {
#ifdef NLH3_DBG
                        fprintf(stderr, "[h3-tls] key_share: grp=0x%04x klen=%zu first4=", sg, klen);
                        for (size_t i = 0; i < klen && i < 4; i++) fprintf(stderr, " %02x", key[i]);
                        fprintf(stderr, "\n"); fflush(stderr);
#endif
                        memcpy(out_pub, key, klen);
                        *out_pub_len = klen;
                        grp = sg;
                        if (out_grp) *out_grp = sg;
                        return 0;
                    }
                    kr -= (2 + 2 + klen);
                    kd += (2 + 2 + klen);
                    list_end = kr;
                }
                return -1;
            } else {
                /* 服务端 ServerHello：key_share = u16 selected_group + opaque key(u16 len) */
                if (kr < 4) return -1;
                grp = (uint16_t)((kd[0] << 8) | kd[1]);
                size_t klen = ((size_t)kd[2] << 8) | kd[3];
                size_t key_len = 0;
                mbedtls_ecp_group_id eng = MBEDTLS_ECP_DP_NONE;
                if (!group_to_ecdh(grp, &eng, &key_len)) return -1;
                if (eng != MBEDTLS_ECP_DP_CURVE25519 &&
                    eng != MBEDTLS_ECP_DP_CURVE448    &&
                    eng != MBEDTLS_ECP_DP_SECP256R1  &&
                    eng != MBEDTLS_ECP_DP_SECP384R1  &&
                    eng != MBEDTLS_ECP_DP_SECP521R1)
                    return -1;
                if (kr < 4 + klen) return -1;
                memcpy(out_pub, kd + 4, klen);
                *out_pub_len = klen;
                if (out_grp) *out_grp = grp;
                return 0;
            }
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

void nl_tls13_conn_set_odcid(nl_tls13_conn_t* c, const uint8_t* odcid, size_t len) {
    if (!c || !odcid || len == 0 || len > 20) return;
    memcpy(c->odcid, odcid, len);
    c->odcid_len = len;
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
    if (tls13_ecdh_make(c, MBEDTLS_ECP_DP_CURVE25519) != 0) return -1;
    c->sel_grp = ecdh_to_group(MBEDTLS_ECP_DP_CURVE25519);
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
    if (!c->cert_inited || !c->key_inited) {
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: no cert/key cert=%d key=%d\n",
                c->cert_inited, c->key_inited); fflush(stderr);
#endif
        return -1;
    }

    mbedtls_sha256_starts(&c->th, 0);
    th_update(c, ch, ch_len);
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_transcript_CH.txt", "w");
        if (fd) {
            fprintf(fd, "len=%zu\n", ch_len);
            fprintf(fd, "hex=");
            for (size_t i = 0; i < ch_len; i++) fprintf(fd, "%02x", ch[i]);
            fprintf(fd, "\n");
            fclose(fd);
        }
    }

    uint8_t peer_pub[72];
    size_t peer_pub_len = 0;
    uint16_t peer_grp = 0;
    {
        int pprs = parse_peer_key_share(ch, ch_len, peer_pub, &peer_pub_len, &peer_grp);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: parse_peer_key_share rc=%d grp=0x%04x pub_len=%zu\n",
                pprs, peer_grp, peer_pub_len); fflush(stderr);
#endif
        if (pprs != 0) return -1;
    }
    {
        uint8_t th_ch[32];
        th_snapshot(c, th_ch);
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_th_AFTER_CH.txt", "w");
        if (fd) { for (int i = 0; i < 32; i++) fprintf(fd, "%02x", th_ch[i]); fclose(fd); }
    }
    /* 按对端选的组做 ECDH：支持 X25519/X448/P-256/P-384/P-521 */
    mbedtls_ecp_group_id ecdh_grp = MBEDTLS_ECP_DP_CURVE25519;
    {
        mbedtls_ecp_group_id tmp;
        size_t kl;
        if (group_to_ecdh(peer_grp, &tmp, &kl) &&
            (tmp == MBEDTLS_ECP_DP_CURVE25519 ||
             tmp == MBEDTLS_ECP_DP_CURVE448    ||
             tmp == MBEDTLS_ECP_DP_SECP256R1    ||
             tmp == MBEDTLS_ECP_DP_SECP384R1    ||
             tmp == MBEDTLS_ECP_DP_SECP521R1))
            ecdh_grp = tmp;
    }
    c->sel_grp = ecdh_to_group(ecdh_grp);                 /* SH 的 key_share selected_group */
    {
        int erm = tls13_ecdh_make(c, ecdh_grp);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: ecdh_make rc=%d grp=0x%04x\n", erm, ecdh_to_group(ecdh_grp)); fflush(stderr);
#endif
        if (erm != 0) return -1;
        int erc = tls13_ecdh_calc(c, peer_pub, peer_pub_len, ecdh_grp);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: ecdh_calc rc=%d shared_len=%zu\n", erc, c->shared_len); fflush(stderr);
#endif
        if (erc != 0) return -1;
    }
    random_fill(c->random);
    memcpy(c->alpn, "h3", 3);          /* H3：服务端协商 ALPN = "h3" */

    size_t off = 0, n = 0;
    uint8_t th[32];

    {
        int bsh = build_server_hello(c, out + off, cap - off, &n);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: build_server_hello rc=%d n=%zu\n", bsh, n); fflush(stderr);
#endif
        if (bsh != 0) return -1;
    }
    th_update(c, out + off, n); off += n;
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_transcript_SH.txt", "w");
        if (fd) {
            fprintf(fd, "len=%zu\n", n);
            fprintf(fd, "hex=");
            for (size_t i = 0; i < n; i++) fprintf(fd, "%02x", out[off - n + i]);
            fprintf(fd, "\n");
            fclose(fd);
        }
    }

    th_snapshot(c, th);                                     /* Hash(CH..SH) */
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_th_AFTER_SH.txt", "w");
        if (fd) { for (int i = 0; i < 32; i++) fprintf(fd, "%02x", th[i]); fclose(fd); }
    }
    {
        int hs = nl_tls13_handshake_secrets(c->shared, c->shared_len, th, 32,
                                            c->c_hs, c->s_hs, c->master);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: handshake_secrets rc=%d\n", hs); fflush(stderr);
#endif
        if (hs != 0) return -1;
    }
    {
        /* dump 32B c_hs / s_hs 本身（HS secret 层），供 Python 独立重算直接对比 */
        FILE* fsec = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_hs_secrets.txt", "w");
        if (fsec) {
            fprintf(fsec, "c_hs=");
            for (int i = 0; i < 32; i++) fprintf(fsec, "%02x", c->c_hs[i]);
            fprintf(fsec, "\ns_hs=");
            for (int i = 0; i < 32; i++) fprintf(fsec, "%02x", c->s_hs[i]);
            fprintf(fsec, "\nmaster=");
            for (int i = 0; i < 32; i++) fprintf(fsec, "%02x", c->master[i]);
            fprintf(fsec, "\nshared=");
            for (int i = 0; i < c->shared_len; i++) fprintf(fsec, "%02x", c->shared[i]);
            fprintf(fsec, "\nth=");
            for (int i = 0; i < 32; i++) fprintf(fsec, "%02x", th[i]);
            fprintf(fsec, "\n");
            fclose(fsec);
        }
    }

    {
        int be = build_encrypted_extensions(c, out + off, cap - off, &n);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: build_encrypted_extensions rc=%d n=%zu\n", be, n); fflush(stderr);
#endif
        if (be != 0) return -1;
    }
    th_update(c, out + off, n); off += n;
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_transcript_EE.txt", "w");
        if (fd) {
            fprintf(fd, "len=%zu\n", n);
            fprintf(fd, "hex=");
            for (size_t i = 0; i < n; i++) fprintf(fd, "%02x", out[off - n + i]);
            fprintf(fd, "\n");
            fclose(fd);
        }
    }

    {
        int bc = build_certificate(c, out + off, cap - off, &n);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: build_certificate rc=%d n=%zu\n", bc, n); fflush(stderr);
#endif
        if (bc != 0) return -1;
    }
    th_update(c, out + off, n); off += n;
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_transcript_Cert.txt", "w");
        if (fd) {
            fprintf(fd, "len=%zu\n", n);
            fprintf(fd, "hex=");
            for (size_t i = 0; i < n; i++) fprintf(fd, "%02x", out[off - n + i]);
            fprintf(fd, "\n");
            fclose(fd);
        }
    }

    th_snapshot(c, th);                                     /* Hash(CH..Cert) */
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_th_AFTER_Cert.txt", "w");
        if (fd) { for (int i = 0; i < 32; i++) fprintf(fd, "%02x", th[i]); fclose(fd); }
    }
    {
        int bcv = build_certificate_verify(c, th, out + off, cap - off, &n);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: build_certificate_verify rc=%d n=%zu\n", bcv, n); fflush(stderr);
#endif
        if (bcv != 0) return -1;
    }
    th_update(c, out + off, n); off += n;
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_transcript_CV.txt", "w");
        if (fd) {
            fprintf(fd, "len=%zu\n", n);
            fprintf(fd, "hex=");
            for (size_t i = 0; i < n; i++) fprintf(fd, "%02x", out[off - n + i]);
            fprintf(fd, "\n");
            fclose(fd);
        }
    }

    th_snapshot(c, th);                                     /* Hash(CH..CV) */
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_th_AFTER_CV.txt", "w");
        if (fd) { for (int i = 0; i < 32; i++) fprintf(fd, "%02x", th[i]); fclose(fd); }
    }
    {
        int bf = build_finished(NL_TLS13_HS_FINISHED, c->s_hs, th, out + off, cap - off, &n);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: build_finished rc=%d n=%zu\n", bf, n); fflush(stderr);
#endif
        if (bf != 0) return -1;
    }
    th_update(c, out + off, n); off += n;
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_transcript_SF.txt", "w");
        if (fd) {
            fprintf(fd, "len=%zu\n", n);
            fprintf(fd, "hex=");
            for (size_t i = 0; i < n; i++) fprintf(fd, "%02x", out[off - n + i]);
            fprintf(fd, "\n");
            fclose(fd);
        }
    }

    th_snapshot(c, th);                                     /* Hash(CH..serverFin) */
    memcpy(c->server_fin_th, th, 32);                       /* 保存不含 CFin 的 transcript 快照 */
    {
        FILE* fd = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_th_AFTER_SFin.txt", "w");
        if (fd) { for (int i = 0; i < 32; i++) fprintf(fd, "%02x", th[i]); fclose(fd); }
    }
    {
        int as = nl_tls13_application_secrets(c->master, 32, th, 32, c->c_ap0, c->s_ap0);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: application_secrets rc=%d\n", as); fflush(stderr);
#endif
        if (as != 0) return -1;
    }
    /* dump 1-RTT 密钥派生中间值（master/th/c_ap0/s_ap0），供 Python 侧对比 */
    {
        FILE* fdump = fopen("f:/C-C++/NetLeaf/build_win/dump/srv_1rtt_intermediate.txt", "w");
        if (fdump) {
            fprintf(fdump, "master="); for (int i = 0; i < 32; i++) fprintf(fdump, "%02x", c->master[i]);
            fprintf(fdump, "\nth="); for (int i = 0; i < 32; i++) fprintf(fdump, "%02x", th[i]);
            fprintf(fdump, "\nc_ap0="); for (int i = 0; i < 32; i++) fprintf(fdump, "%02x", c->c_ap0[i]);
            fprintf(fdump, "\ns_ap0="); for (int i = 0; i < 32; i++) fprintf(fdump, "%02x", c->s_ap0[i]);
            fprintf(fdump, "\n");
            fclose(fdump);
        }
    }

    {
        int qk1 = nl_tls13_secret_to_quic_keys(c->c_ap0, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &c->read1);
        int qk2 = nl_tls13_secret_to_quic_keys(c->s_ap0, 32, NL_QUIC_CIPHER_AES128_GCM_SHA256, &c->write1);
#ifdef NLH3_DBG
        fprintf(stderr, "[h3-tls] srv_hs: quic_keys rc1=%d rc2=%d off=%zu\n", qk1, qk2, off); fflush(stderr);
#endif
        if (qk1 != 0) return -1;
        if (qk2 != 0) return -1;
    }

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
    uint8_t peer_pub[72];
    size_t peer_pub_len = 0;
    uint16_t peer_grp = 0;
    if (parse_peer_key_share(sh, sh_len, peer_pub, &peer_pub_len, &peer_grp) != 0) return -12;
    /* 按服务端选的组做 ECDH：支持 X25519/X448/P-256/P-384/P-521 */
    mbedtls_ecp_group_id ecdh_grp = MBEDTLS_ECP_DP_CURVE25519;
    {
        mbedtls_ecp_group_id tmp;
        size_t kl;
        if (group_to_ecdh(peer_grp, &tmp, &kl) &&
            (tmp == MBEDTLS_ECP_DP_CURVE25519 ||
             tmp == MBEDTLS_ECP_DP_CURVE448    ||
             tmp == MBEDTLS_ECP_DP_SECP256R1    ||
             tmp == MBEDTLS_ECP_DP_SECP384R1    ||
             tmp == MBEDTLS_ECP_DP_SECP521R1))
            ecdh_grp = tmp;
    }
    if (tls13_ecdh_calc(c, peer_pub, peer_pub_len, ecdh_grp) != 0) return -13;
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

/* 1-RTT 密钥 peek 版本：不检查 established。
 * 服务端 nl_tls13_server_handshake 内部已派生 read1/write1（c->c_ap0/s->s_ap0），
 * 但 conn 级 established 仅在 nl_tls13_server_finish 才置位。
 * 在 2a 分支（首次处理 client Initial）时，client 的 1-RTT 长首包（H3 SETTINGS 流 2）
 * 先于 client Finished 到达，此时需要 rtt_read 解密，但 established 尚未置位。
 * 此函数绕过 established 检查，直接返回已派生的密钥。 */
int nl_tls13_conn_peek_1rtt_keys(const nl_tls13_conn_t* c,
                                  nl_quic_keys_t* read_keys, nl_quic_keys_t* write_keys) {
    if (!c) return -1;
    if (read_keys)  *read_keys  = c->read1;
    if (write_keys) *write_keys = c->write1;
    return 0;
}

/* 服务端 1-RTT CRYPTO（FINISHED）消息：verify_data = finished_verify_data(
 * finished_key(s_hs), Hash(transcript-so-far))，与 nl_tls13_server_handshake 已发
 * 出的 FINISHED 字节完全一致（同一 s_hs、同一 transcript 快照点）。
 * 供 quictls_quic.c 在 2b/fallback 处理 client Finished 后，用 CRYPTO 帧装进
 * 1-RTT 短首包发给客户端；aioquic 客户端据此进入 application-data，再发 H3 数据帧。 */
int nl_tls13_server_finished_msg(const nl_tls13_conn_t* c,
                                 uint8_t* out, size_t cap, size_t* out_len) {
    if (!c || !out || cap < 4 + 3 + 32) return -1;
    return build_finished(NL_TLS13_HS_FINISHED, c->s_hs, c->server_fin_th, out, cap, out_len);
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
        /* SAN：localhost + 127.0.0.1（aioquic verify_certificate=True 时匹配 server_name） */
        {
            static const char DNS_LO[] = "localhost";
            static const unsigned char IP_LOCAL[] = { 127, 0, 0, 1 };
            mbedtls_x509_san_list dns_san, ip_san;
            mbedtls_x509_san_list* san_head = &dns_san;

            memset(&dns_san, 0, sizeof(dns_san));
            memset(&ip_san, 0, sizeof(ip_san));
            dns_san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
            dns_san.node.san.unstructured_name.len = (unsigned int)sizeof(DNS_LO) - 1;
            dns_san.node.san.unstructured_name.p = (const unsigned char*)DNS_LO;
            dns_san.next = &ip_san;

            ip_san.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
            ip_san.node.san.unstructured_name.len = (unsigned int)sizeof(IP_LOCAL);
            ip_san.node.san.unstructured_name.p = IP_LOCAL;
            ip_san.next = NULL;

            if (mbedtls_x509write_crt_set_subject_alternative_name(&crt, san_head) != 0) { rc = -1; break; }
        }

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
