#ifndef NETLEAF_QUICTLS_H
#define NETLEAF_QUICTLS_H 1

/**
 * @file netleaf_quictls.h
 * @brief 自研 QUIC-TLS（RFC 9001）扩展库公共接口
 * @version 0.1.0
 *
 * 参考标准：
 *  - RFC 8446 (TLS 1.3)、RFC 9001 (Using TLS to Secure QUIC)
 *  - RFC 9000 (QUIC)、RFC 9002 (Loss Detection)、RFC 9114 (HTTP/3)、RFC 9204 (QPACK)
 * 参考开源实现（设计范式）：
 *  - picotls / quicly (h2o)、ngtcp2 / nghttp3、lsquic / lsqpack、picoquic、quiche
 *
 * 说明：本扩展在 mbedTLS 原语（HKDF/HMAC/AES-GCM/AES-ECB）之上自研 QUIC-TLS
 * 数据包保护与密钥调度层；不依赖 mbedTLS 的 TLS 记录层。
 */

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

#ifdef _WIN32
  #ifdef NL_QUICTLS_EXPORTS
    #define NL_QUICTLS_API __declspec(dllexport)
  #else
    #define NL_QUICTLS_API __declspec(dllimport)
  #endif
#else
  #define NL_QUICTLS_API
#endif

#define NL_QUICTLS_VERSION "0.1.0"

/* QUIC 数据包保护密钥长度上限（AES-128-GCM key/iv/hp） */
#define NL_QUIC_MAX_KEY_LEN 32
#define NL_QUIC_IV_LEN      12
#define NL_QUIC_MAX_HP_LEN  32
#define NL_QUIC_TAG_LEN     16

/* 加密级别（RFC 9001 §4.1.1 密钥阶段） */
typedef enum {
    NL_QUIC_LEVEL_INITIAL  = 0,
    NL_QUIC_LEVEL_HANDSHAKE = 1,
    NL_QUIC_LEVEL_ZERO_RTT = 2,
    NL_QUIC_LEVEL_ONE_RTT  = 3
} nl_quic_level_t;

/* 一套 QUIC 数据包保护密钥（key / iv / hp） */
typedef struct {
    uint8_t key[NL_QUIC_MAX_KEY_LEN];
    size_t  key_len;
    uint8_t iv[NL_QUIC_IV_LEN];
    uint8_t hp[NL_QUIC_MAX_HP_LEN];
    size_t  hp_len;
    int     is_chacha;   /* 0=AES(ECB 首部保护)，1=ChaCha20 首部保护 */
} nl_quic_keys_t;

/**
 * @brief HKDF-Expand-Label（RFC 8446 §7.1）
 * @param secret       输入密钥材料（PRK）
 * @param label        标签（不含 "tls13 " 前缀，函数内部自动补）
 * @param context      上下文（可为 NULL / 0）
 * @return 0 成功，<0 失败
 */
NL_QUICTLS_API int nl_quic_hkdf_expand_label(const uint8_t* secret, size_t secret_len,
                                             const char* label,
                                             const uint8_t* context, size_t context_len,
                                             uint8_t* out, size_t out_len);

/**
 * @brief QUIC v1 Initial 密钥派生（RFC 9001 §5.2）
 * @param dcid 客户端 Destination Connection ID
 * @return 0 成功
 */
NL_QUICTLS_API int nl_quic_initial_keys(const uint8_t* dcid, size_t dcid_len,
                                        nl_quic_keys_t* client_keys,
                                        nl_quic_keys_t* server_keys);

/**
 * @brief 长首部包 AEAD 保护 + 首部保护（就地，RFC 9001 §5.3/§5.4）
 *
 * 输入 pkt = [首部 0..pn_offset) | PN[pn_len] | 明文 payload[payload_len]];
 * 输出 pkt = [首部 | PN | 密文 | 16 字节 tag]，随后首部前 5 字节被掩码保护。
 * pkt 缓冲容量须 >= pn_offset + pn_len + payload_len + NL_QUIC_TAG_LEN。
 */
NL_QUICTLS_API int nl_quic_protect_packet(const nl_quic_keys_t* keys, uint64_t pn,
                                          uint8_t* pkt, size_t pn_offset, size_t pn_len,
                                          size_t payload_len, size_t* out_len);

/**
 * @brief 长首部包解保护（先还原首部保护，再 AEAD 解密；就地）
 * @param ct_len 密文长度（不含 16 字节 tag）
 */
NL_QUICTLS_API int nl_quic_unprotect_packet(const nl_quic_keys_t* keys, uint64_t pn,
                                            uint8_t* pkt, size_t pn_offset, size_t pn_len,
                                            size_t ct_len, size_t* plaintext_len);

/* ============================================================
 * C2：短首部 / ChaCha20 / 包号
 * ============================================================ */

/* 密码套件编号（TLS） */
#define NL_QUIC_CIPHER_AES128_GCM_SHA256          0x1301
#define NL_QUIC_CIPHER_AES256_GCM_SHA384          0x1302
#define NL_QUIC_CIPHER_CHACHA20_POLY1305_SHA256   0x1303

/**
 * @brief 通用长/短首部包保护（就地，long_header=1 长首部，0 短首部）
 */
NL_QUICTLS_API int nl_quic_protect_packet_ex(const nl_quic_keys_t* keys, uint64_t pn,
                                             int long_header,
                                             uint8_t* pkt, size_t pn_offset, size_t pn_len,
                                             size_t payload_len, size_t* out_len);

NL_QUICTLS_API int nl_quic_unprotect_packet_ex(const nl_quic_keys_t* keys, uint64_t pn,
                                               int long_header,
                                               uint8_t* pkt, size_t pn_offset, size_t pn_len,
                                               size_t ct_len, size_t* plaintext_len);

/**
 * @brief 包号编码（RFC 9000 §17.1）与解码（RFC 9000 §A.3）
 */
NL_QUICTLS_API int nl_quic_pn_encode(uint64_t full_pn, uint64_t largest_acked,
                                     uint8_t* out, size_t* out_len);
NL_QUICTLS_API uint64_t nl_quic_pn_decode(uint64_t largest_pn, uint64_t truncated, size_t pn_len);

/* 首部保护解除（自动读取 pn_len，去掉固定 pn_len=1 限制）与 AEAD 解密（分离式，供 QUIC 包解析） */
NL_QUICTLS_API int nl_quic_hp_unprotect(const nl_quic_keys_t* keys, uint8_t* pkt, size_t pn_offset,
                                        int long_header, size_t total_len, size_t* out_pn_len);
NL_QUICTLS_API int nl_quic_unprotect_payload(const nl_quic_keys_t* keys, uint64_t pn,
                                             uint8_t* pkt, size_t pn_offset, size_t pn_len,
                                             size_t ct_len, size_t* plaintext_len);

/* 1-RTT 密钥集（支持 quic ku 密钥更新） */
typedef struct { nl_quic_keys_t keys; uint8_t secret[32]; unsigned suite; } nl_quic_keyset_t;
NL_QUICTLS_API int nl_quic_keyset_init(nl_quic_keyset_t* ks, const uint8_t secret[32], unsigned suite);
NL_QUICTLS_API int nl_quic_keyset_key_update(nl_quic_keyset_t* ks);

/* QUIC 变长整数（RFC 9000 §16）；返回写入/读取的字节数，0 表示失败/不足 */
NL_QUICTLS_API size_t nl_quic_varint_write(uint8_t* p, size_t cap, uint64_t v);
NL_QUICTLS_API size_t nl_quic_varint_read(const uint8_t* p, size_t len, uint64_t* v);

/* ============================================================
 * C5+：ACK 帧 + 丢包检测/重传 + 拥塞控制（RFC 9002 简化）
 * ============================================================ */
#define NL_QUIC_MAX_ACK_RANGES 64

typedef struct { uint64_t smallest, largest; } nl_quic_ack_range_t;   /* 闭区间，降序 */

/* ACK 帧（RFC 9000 §19.3，type 0x02）编解码 */
NL_QUICTLS_API size_t nl_quic_ack_encode(uint8_t* out, size_t cap, uint64_t ack_delay,
                                         const nl_quic_ack_range_t* ranges, size_t n_ranges,
                                         size_t* out_len);
NL_QUICTLS_API int nl_quic_ack_decode(const uint8_t* d, size_t len, uint64_t* ack_delay,
                                      nl_quic_ack_range_t* ranges, size_t max_ranges,
                                      size_t* n_ranges, size_t* consumed);

/* 接收端包号追踪（用于生成 ACK） */
typedef struct nl_quic_acktrack nl_quic_acktrack_t;
NL_QUICTLS_API nl_quic_acktrack_t* nl_quic_acktrack_new(void);
NL_QUICTLS_API void nl_quic_acktrack_free(nl_quic_acktrack_t* a);
NL_QUICTLS_API int nl_quic_acktrack_add(nl_quic_acktrack_t* a, uint64_t pn);
NL_QUICTLS_API int nl_quic_acktrack_ranges(const nl_quic_acktrack_t* a,
                                           nl_quic_ack_range_t* out, size_t max, size_t* n);

/* 发送端恢复：发送记录 + packet-threshold 丢包检测 + 重传 + NewReno 式拥塞 */
typedef struct nl_quic_recovery nl_quic_recovery_t;
NL_QUICTLS_API nl_quic_recovery_t* nl_quic_recovery_new(uint64_t mtu, uint64_t init_cwnd);
NL_QUICTLS_API void nl_quic_recovery_free(nl_quic_recovery_t* r);
/* now_ms 为逻辑时钟（毫秒），由调用方驱动（便于仿真/测试） */
NL_QUICTLS_API int nl_quic_recovery_on_sent(nl_quic_recovery_t* r, uint64_t pn, size_t bytes,
                                            const uint8_t* frames, size_t frames_len, uint64_t now_ms);
NL_QUICTLS_API int nl_quic_recovery_on_ack(nl_quic_recovery_t* r,
                                           const nl_quic_ack_range_t* ranges, size_t n_ranges,
                                           uint64_t now_ms);
/* 基于时间阈值的丢包检测（RFC 9002 §6.1.2）：标记发送已久未确认的包为丢失 */
NL_QUICTLS_API int nl_quic_recovery_on_time(nl_quic_recovery_t* r, uint64_t now_ms,
                                            uint64_t* out_lost);
/* 取一个待重传包（返回其帧副本与包号）；无返回 0 */
NL_QUICTLS_API int nl_quic_recovery_next_retransmit(nl_quic_recovery_t* r,
                                                    uint8_t* out, size_t cap, size_t* out_len,
                                                    uint64_t* out_pn);
NL_QUICTLS_API uint64_t nl_quic_recovery_cwnd(const nl_quic_recovery_t* r);
NL_QUICTLS_API uint64_t nl_quic_recovery_bytes_in_flight(const nl_quic_recovery_t* r);
/* RTT 估计与 PTO（RFC 9002 §5 / §6.2） */
NL_QUICTLS_API uint64_t nl_quic_recovery_latest_rtt(const nl_quic_recovery_t* r);
NL_QUICTLS_API uint64_t nl_quic_recovery_srtt(const nl_quic_recovery_t* r);
NL_QUICTLS_API uint64_t nl_quic_recovery_rttvar(const nl_quic_recovery_t* r);
NL_QUICTLS_API uint64_t nl_quic_recovery_pto(const nl_quic_recovery_t* r);

/* ============================================================
 * C5++：流控（RFC 9000 §4 / §19.9-19.11）—— 连接级 + 流级
 * ============================================================ */
typedef struct nl_quic_fc nl_quic_fc_t;

NL_QUICTLS_API nl_quic_fc_t* nl_quic_fc_new(uint64_t conn_window, uint64_t stream_window);
NL_QUICTLS_API void nl_quic_fc_free(nl_quic_fc_t* fc);

/* 发送侧：对端窗口允许发送的字节数（<= want）；已发送；收到对端上限 */
NL_QUICTLS_API uint64_t nl_quic_fc_send_allow(nl_quic_fc_t* fc, uint64_t stream_id, uint64_t want);
NL_QUICTLS_API void nl_quic_fc_on_sent(nl_quic_fc_t* fc, uint64_t stream_id, uint64_t bytes);
NL_QUICTLS_API void nl_quic_fc_on_max_data(nl_quic_fc_t* fc, uint64_t max);
NL_QUICTLS_API void nl_quic_fc_on_max_stream_data(nl_quic_fc_t* fc, uint64_t stream_id, uint64_t max);
NL_QUICTLS_API int  nl_quic_fc_send_blocked(const nl_quic_fc_t* fc, uint64_t stream_id);

/* 接收侧：收到数据（返回是否应发送窗口更新）；当前应通告的上限 */
NL_QUICTLS_API int      nl_quic_fc_on_recv(nl_quic_fc_t* fc, uint64_t stream_id, uint64_t bytes);
NL_QUICTLS_API uint64_t nl_quic_fc_advertised_max_data(const nl_quic_fc_t* fc);
NL_QUICTLS_API uint64_t nl_quic_fc_advertised_max_stream_data(const nl_quic_fc_t* fc, uint64_t stream_id);

/* 流控帧编解码（MAX_DATA 0x10 / MAX_STREAM_DATA 0x11） */
NL_QUICTLS_API size_t nl_quic_max_data_encode(uint8_t* out, size_t cap, uint64_t max, size_t* out_len);
NL_QUICTLS_API int    nl_quic_max_data_decode(const uint8_t* d, size_t len, uint64_t* max, size_t* consumed);
NL_QUICTLS_API size_t nl_quic_max_stream_data_encode(uint8_t* out, size_t cap, uint64_t sid, uint64_t max, size_t* out_len);
NL_QUICTLS_API int    nl_quic_max_stream_data_decode(const uint8_t* d, size_t len, uint64_t* sid, uint64_t* max, size_t* consumed);

/* ============================================================
 * C6+：QPACK 字段行（RFC 9204，静态表；无动态表 / 无霍夫曼）
 * ============================================================ */
typedef struct { char name[64]; char value[256]; } nl_h3_qpack_field_t;

/* 编码单字段（优先静态表索引：Indexed / Literal with Name Ref / Literal），返回字节数或 -1 */
NL_QUICTLS_API int nl_h3_qpack_encode_field(uint8_t* out, size_t cap,
                                            const char* name, const char* value);
/* 解码字段块到字段数组 */
NL_QUICTLS_API int nl_h3_qpack_decode(const uint8_t* d, size_t len,
                                      nl_h3_qpack_field_t* out, int max, int* count);
/* 便捷查找（未找到返回 NULL） */
NL_QUICTLS_API const char* nl_h3_qpack_get(const nl_h3_qpack_field_t* fields, int count,
                                           const char* name);

/* --- QPACK 动态表（RFC 9204 §3）与指令（§4.3/§4.4） --- */
typedef struct nl_h3_qpack_dt nl_h3_qpack_dt_t;
NL_QUICTLS_API nl_h3_qpack_dt_t* nl_h3_qpack_dt_new(uint64_t capacity);
NL_QUICTLS_API void     nl_h3_qpack_dt_free(nl_h3_qpack_dt_t* dt);
NL_QUICTLS_API int      nl_h3_qpack_dt_set_capacity(nl_h3_qpack_dt_t* dt, uint64_t capacity);
NL_QUICTLS_API uint64_t nl_h3_qpack_dt_size(const nl_h3_qpack_dt_t* dt);
NL_QUICTLS_API uint64_t nl_h3_qpack_dt_insert_count(const nl_h3_qpack_dt_t* dt);
/* 插入一对 name/value（超出容量则逐出旧条目）；rel_index=0 为最新 */
NL_QUICTLS_API int      nl_h3_qpack_dt_insert(nl_h3_qpack_dt_t* dt, const char* name, const char* value);
NL_QUICTLS_API int      nl_h3_qpack_dt_get(const nl_h3_qpack_dt_t* dt, uint64_t rel_index,
                                           char* name, size_t ncap, char* value, size_t vcap);
/* 编码器指令 */
NL_QUICTLS_API int nl_h3_qpack_enc_set_capacity(uint8_t* out, size_t cap, uint64_t capacity);
NL_QUICTLS_API int nl_h3_qpack_enc_insert_nameref(uint8_t* out, size_t cap, int is_static,
                                                  uint64_t name_index, const char* value);
NL_QUICTLS_API int nl_h3_qpack_enc_insert_literal(uint8_t* out, size_t cap,
                                                  const char* name, const char* value);
/* 解码器指令 */
NL_QUICTLS_API int nl_h3_qpack_dec_section_ack(uint8_t* out, size_t cap, uint64_t stream_id);
NL_QUICTLS_API int nl_h3_qpack_dec_stream_cancel(uint8_t* out, size_t cap, uint64_t stream_id);
NL_QUICTLS_API int nl_h3_qpack_dec_insert_count_increment(uint8_t* out, size_t cap, uint64_t inc);

/* 结合动态表的字段行（T=0 索引 / 名称引用）；dt 可为 NULL（等价静态表版本） */
NL_QUICTLS_API int nl_h3_qpack_encode_field_dt(uint8_t* out, size_t cap,
                                               const nl_h3_qpack_dt_t* dt,
                                               const char* name, const char* value);
NL_QUICTLS_API int nl_h3_qpack_decode_dt(const uint8_t* d, size_t len,
                                         const nl_h3_qpack_dt_t* dt,
                                         nl_h3_qpack_field_t* out, int max, int* count);

/* ============================================================
 * C3/C4：TLS 1.3 密钥调度（RFC 8446 §7.1 / RFC 9001 §5.1）
 * ============================================================ */
#define NL_TLS13_HASH_LEN   32   /* SHA-256 */
#define NL_TLS13_SECRET_LEN 32

/* HKDF-Extract（HMAC-SHA256） */
NL_QUICTLS_API int nl_tls13_hkdf_extract(const uint8_t* salt, size_t salt_len,
                                         const uint8_t* ikm, size_t ikm_len,
                                         uint8_t out[NL_TLS13_HASH_LEN]);

/* Derive-Secret(secret, label, transcript_hash)（RFC 8446 §7.1） */
NL_QUICTLS_API int nl_tls13_derive_secret(const uint8_t* secret, size_t secret_len,
                                          const char* label,
                                          const uint8_t* transcript_hash, size_t th_len,
                                          uint8_t* out, size_t out_len);

/* SHA-256 哈希（供 transcript 计算） */
NL_QUICTLS_API int nl_tls13_sha256(const uint8_t* data, size_t len,
                                   uint8_t out[NL_TLS13_HASH_LEN]);

/**
 * @brief 由 ECDHE 共享秘密 + CH..SH transcript 派生握手交通秘密与 master secret
 */
NL_QUICTLS_API int nl_tls13_handshake_secrets(const uint8_t* ecdhe, size_t ecdhe_len,
                                              const uint8_t* th_ch_sh, size_t th_len,
                                              uint8_t c_hs[NL_TLS13_SECRET_LEN],
                                              uint8_t s_hs[NL_TLS13_SECRET_LEN],
                                              uint8_t master[NL_TLS13_SECRET_LEN]);

/**
 * @brief 由 master secret + CH..server-Finished transcript 派生应用交通秘密
 */
NL_QUICTLS_API int nl_tls13_application_secrets(const uint8_t* master, size_t master_len,
                                                const uint8_t* th_ch_sfin, size_t th_len,
                                                uint8_t c_ap0[NL_TLS13_SECRET_LEN],
                                                uint8_t s_ap0[NL_TLS13_SECRET_LEN]);

/**
 * @brief 交通秘密 → QUIC 包保护密钥（"quic key"/"quic iv"/"quic hp"，随密码套件）
 */
NL_QUICTLS_API int nl_tls13_secret_to_quic_keys(const uint8_t* secret, size_t secret_len,
                                                unsigned cipher_suite, nl_quic_keys_t* out);

/* Finished（RFC 8446 §4.4.4） */
NL_QUICTLS_API int nl_tls13_finished_key(const uint8_t* base_secret, size_t len,
                                         uint8_t out[NL_TLS13_HASH_LEN]);
NL_QUICTLS_API int nl_tls13_finished_verify_data(const uint8_t* finished_key,
                                                 const uint8_t* transcript_hash, size_t th_len,
                                                 uint8_t out[NL_TLS13_HASH_LEN]);

/* KeyUpdate（RFC 8446 §7.2，"traffic upd"；TLS 层用） */
NL_QUICTLS_API int nl_tls13_update_traffic_secret(uint8_t secret[NL_TLS13_SECRET_LEN]);

/* QUIC 密钥更新（RFC 9001 §6，标签 "quic ku"；QUIC 层用） */
NL_QUICTLS_API int nl_quic_update_key_secret(uint8_t secret[32]);

/**
 * @brief 握手消息编解码（RFC 8446 §4：type(1) + length(3) + body）
 */
NL_QUICTLS_API int nl_tls13_encode_handshake(uint8_t msg_type,
                                             const uint8_t* body, size_t body_len,
                                             uint8_t* out, size_t cap, size_t* out_len);
NL_QUICTLS_API int nl_tls13_parse_handshake(const uint8_t* data, size_t len,
                                            uint8_t* msg_type,
                                            const uint8_t** body, size_t* body_len,
                                            size_t* consumed);
/* 握手消息类型（RFC 8446 §4） */
#define NL_TLS13_HS_CLIENT_HELLO        1
#define NL_TLS13_HS_SERVER_HELLO        2
#define NL_TLS13_HS_ENCRYPTED_EXTENSIONS 8
#define NL_TLS13_HS_CERTIFICATE         11
#define NL_TLS13_HS_CERTIFICATE_VERIFY  15
#define NL_TLS13_HS_FINISHED            20

/* ============================================================
 * C3：TLS 1.3 握手状态机（无证书认证骨架：ECDHE(X25519) + Finished）
 * 说明：握手消息以明文 CRYPTO 帧载荷形式在两端间传递（QUIC 包保护在更底层，
 *       由本扩展的 nl_quic_*_packet 负责）；本层只处理握手本身。
 * ============================================================ */
typedef struct nl_tls13_conn nl_tls13_conn_t;

NL_QUICTLS_API nl_tls13_conn_t* nl_tls13_conn_new(int is_server);
NL_QUICTLS_API void nl_tls13_conn_free(nl_tls13_conn_t* c);

/* 客户端：产出 ClientHello；服务端：处理 CH 并产出 ServerHello+EE+serverFinished */
NL_QUICTLS_API int nl_tls13_client_hello(nl_tls13_conn_t* c,
                                         uint8_t* out, size_t cap, size_t* out_len);
NL_QUICTLS_API int nl_tls13_server_handshake(nl_tls13_conn_t* c,
                                             const uint8_t* ch, size_t ch_len,
                                             uint8_t* out, size_t cap, size_t* out_len);
/* 客户端：处理服务端 flight，产出 client Finished；服务端：处理 client Finished */
NL_QUICTLS_API int nl_tls13_client_handshake(nl_tls13_conn_t* c,
                                             const uint8_t* in, size_t in_len,
                                             uint8_t* out, size_t cap, size_t* out_len);
/* 分阶段（供 QUIC 按 Initial/Handshake 层处理）：
 * _1 处理 ServerHello 并导出握手密钥；_2 处理 EE/Cert/CV/Fin 并产出 client Finished */
NL_QUICTLS_API int nl_tls13_client_handshake_1(nl_tls13_conn_t* c, const uint8_t* sh, size_t sh_len);
NL_QUICTLS_API int nl_tls13_client_handshake_2(nl_tls13_conn_t* c,
                                               const uint8_t* in, size_t in_len,
                                               uint8_t* out, size_t cap, size_t* out_len);
NL_QUICTLS_API int nl_tls13_server_finish(nl_tls13_conn_t* c, const uint8_t* in, size_t in_len);

NL_QUICTLS_API int nl_tls13_conn_established(const nl_tls13_conn_t* c);
/* 协商出的 ALPN 协议（如 "h3"；未协商返回 ""） */
NL_QUICTLS_API const char* nl_tls13_conn_alpn(const nl_tls13_conn_t* c);
/* 取 1-RTT 密钥（按角色自动映射 read/write） */
NL_QUICTLS_API int nl_tls13_conn_get_1rtt_keys(const nl_tls13_conn_t* c,
                                               nl_quic_keys_t* read_keys,
                                               nl_quic_keys_t* write_keys);
/* 取握手级密钥（QUIC Handshake 包用） */
NL_QUICTLS_API int nl_tls13_conn_get_handshake_keys(const nl_tls13_conn_t* c,
                                                    nl_quic_keys_t* read_keys,
                                                    nl_quic_keys_t* write_keys);

/* 证书与认证（ECDSA P-256 / ecdsa_secp256r1_sha256 = 0x0403） */
NL_QUICTLS_API int nl_tls13_conn_set_cert(nl_tls13_conn_t* c,
                                          const char* cert_pem, const char* key_pem);
NL_QUICTLS_API int nl_tls13_conn_set_ca(nl_tls13_conn_t* c, const char* ca_pem);
NL_QUICTLS_API int nl_tls13_conn_peer_verified(const nl_tls13_conn_t* c);
/* 生成自签 P-256 证书 + 私钥（PEM，测试/示例用） */
NL_QUICTLS_API int nl_tls13_gen_self_signed(char* cert_pem, size_t cert_cap,
                                            char* key_pem, size_t key_cap);

/* 模块 / 扩展入口 */
NL_QUICTLS_API int nl_quictls_init(void);
NL_QUICTLS_API int nl_quictls_is_available(void);
NL_QUICTLS_API const char* nl_quictls_version(void);

/**
 * @brief RFC 9001 Appendix A 自测（Initial 密钥向量 + 保护往返 + C2 往返/包号）
 * @return 0 全部通过；>0 首个失败步骤号
 */
NL_QUICTLS_API int nl_quic_rfc9001_selftest(void);

/**
 * @brief TLS 1.3 密钥调度 / 握手编解码 自测（结构一致性）
 * @return 0 全部通过；>0 首个失败步骤号
 */
NL_QUICTLS_API int nl_quictls_tls13_selftest(void);

/**
 * @brief TLS 1.3 握手状态机自测（客户端↔服务端进程内端到端，含证书认证）
 * @return 0 全部通过；>0 首个失败步骤号
 */
NL_QUICTLS_API int nl_quictls_handshake_selftest(void);

/**
 * @brief QUIC 传输 + HTTP/3 端到端自测（Initial/Handshake/1-RTT + H3 GET 请求/响应）
 * @return 0 全部通过；>0 首个失败步骤号
 */
NL_QUICTLS_API int nl_quictls_quic_selftest(void);

/**
 * @brief QUIC 可靠性自测（ACK 帧往返 + 丢包检测/重传 + 拥塞窗口，RFC 9002 简化）
 * @return 0 全部通过；>0 首个失败步骤号
 */
NL_QUICTLS_API int nl_quictls_recovery_selftest(void);

/**
 * @brief QUIC 流控自测（连接级 + 流级窗口、窗口更新、阻塞判定）
 * @return 0 全部通过；>0 首个失败步骤号
 */
NL_QUICTLS_API int nl_quictls_flowcontrol_selftest(void);

/**
 * @brief QPACK 自测（静态表索引 / 名称引用 / 字面量往返）
 * @return 0 全部通过；>0 首个失败步骤号
 */
NL_QUICTLS_API int nl_quictls_qpack_selftest(void);

/**
 * @brief 运行全部自测（KAT / 密钥调度 / 握手+证书 / QUIC+H3 / 可靠性 / 流控 / QPACK）
 * @return 0 全部通过；否则为失败的自测个数
 */
NL_QUICTLS_API int nl_quictls_selftest_all(void);

/* ============================================================
 * C7-1：QPACK/HPACK 霍夫曼编码（RFC 7541 Appendix B）
 * ============================================================ */
NL_QUICTLS_API int nl_h3_huffman_encode(const uint8_t* in, size_t in_len,
                                        uint8_t* out, size_t cap, size_t* out_len);
NL_QUICTLS_API int nl_h3_huffman_decode(const uint8_t* in, size_t in_len,
                                        uint8_t* out, size_t cap, size_t* out_len);
NL_QUICTLS_API int nl_quictls_huffman_selftest(void);

/* ============================================================
 * C7-2：QPACK 编解码器流与 Base 索引（RFC 9204 §4.2-4.4）
 * ============================================================ */
typedef struct nl_h3_qpack_streams nl_h3_qpack_streams_t;
NL_QUICTLS_API nl_h3_qpack_streams_t* nl_h3_qpack_streams_new(nl_h3_qpack_dt_t* dt);
NL_QUICTLS_API void nl_h3_qpack_streams_free(nl_h3_qpack_streams_t* s);
/* 解码器侧：处理编码器流字节（Set Capacity / Insert with Name Ref / Insert with Literal Name / Duplicate） */
NL_QUICTLS_API int nl_h3_qpack_streams_on_encoder(nl_h3_qpack_streams_t* s, const uint8_t* d, size_t len);
/* 编码器侧：处理解码器流字节（Section Ack / Stream Cancel / Insert Count Increment） */
NL_QUICTLS_API int nl_h3_qpack_streams_on_decoder(nl_h3_qpack_streams_t* s, const uint8_t* d, size_t len);
/* 以 Base 解析字段块中的动态相对索引（T=0；post_base=1 表示 Post-Base 索引） */
NL_QUICTLS_API int nl_h3_qpack_resolve_dynamic(const nl_h3_qpack_dt_t* dt, uint64_t base,
                                               int post_base, uint64_t rel_index,
                                               char* name, size_t ncap, char* value, size_t vcap);
/* 静态表查询（供编解码器流解析 T=1 名称引用） */
NL_QUICTLS_API int nl_h3_qpack_static_get(uint64_t index, char* name, size_t ncap, char* value, size_t vcap);
/* 查询状态 */
NL_QUICTLS_API uint64_t nl_h3_qpack_streams_known_received(const nl_h3_qpack_streams_t* s);
NL_QUICTLS_API int nl_quictls_qpack_stream_selftest(void);

/* ============================================================
 * C7-3：完整拥塞控制补充（RFC 9002 §7）
 * ============================================================ */
NL_QUICTLS_API uint64_t nl_quic_recovery_cwnd_min(const nl_quic_recovery_t* r);
NL_QUICTLS_API void     nl_quic_recovery_on_persistent_congestion(nl_quic_recovery_t* r);
NL_QUICTLS_API void     nl_quic_recovery_on_ecn_ce(nl_quic_recovery_t* r, uint64_t ce_total);
NL_QUICTLS_API uint64_t nl_quic_recovery_ecn_ce(const nl_quic_recovery_t* r);

/* ============================================================
 * C8-A：可运行 HTTP/3 服务端（UDP 回环；POSIX 实现，Windows 暂桩返回 NULL/-1）
 * ============================================================ */
typedef struct nlh3_server nlh3_server_t;
/* 处理函数：设置 *resp_body（malloc 分配，调用方释放）与 *resp_len */
typedef void (*nlh3_handler_t)(const char* method, const char* path, const char* authority,
                               void* userdata, char** resp_body, size_t* resp_len);
NL_QUICTLS_API nlh3_server_t* nlh3_server_create(int port, const char* cert_pem, const char* key_pem);
NL_QUICTLS_API int  nlh3_server_start(nlh3_server_t* s);
NL_QUICTLS_API void nlh3_server_stop(nlh3_server_t* s);
NL_QUICTLS_API void nlh3_server_destroy(nlh3_server_t* s);
NL_QUICTLS_API void nlh3_server_set_handler(nlh3_server_t* s, nlh3_handler_t h, void* ud);
/* 客户端：发起一次请求（阻塞），响应体写入 out_body */
NL_QUICTLS_API int  nlh3_client_request(const char* host, int port, const char* method,
                                        const char* path, char* out_body, size_t cap, size_t* out_len);
/* 客户端（带 CA 校验）：ca_pem 非空则校验服务端证书链，未通过返回失败 */
NL_QUICTLS_API int  nlh3_client_request_ex(const char* host, int port, const char* method,
                                           const char* path, const char* ca_pem,
                                           char* out_body, size_t cap, size_t* out_len);
NL_QUICTLS_API int  nl_quictls_h3server_selftest(void);

/* ============================================================
 * C8-B：QPACK 动态表↔编解码器流 字段块联动
 * ============================================================ */
/* 联动编码：dt 已有 (name,value) → 输出 T=0 索引字段行；否则插入 dt 并输出
 * 需发送给对端的编码器流指令（enc_out），再输出字段行（field_out）。 */
NL_QUICTLS_API int nl_h3_qpack_link_encode(nl_h3_qpack_dt_t* dt,
                                           const char* name, const char* value,
                                           uint8_t* field_out, size_t field_cap, size_t* field_len,
                                           uint8_t* enc_out, size_t enc_cap, size_t* enc_len);
/* 联动解码：先应用对端编码器流指令（enc）到 dt，再解码字段块（field）。 */
NL_QUICTLS_API int nl_h3_qpack_link_decode(nl_h3_qpack_streams_t* s, const nl_h3_qpack_dt_t* dt,
                                           const uint8_t* enc, size_t enc_len,
                                           const uint8_t* field, size_t field_len,
                                           nl_h3_qpack_field_t* out, int max, int* count);
NL_QUICTLS_API int nl_quictls_qpack_link_selftest(void);

#endif /* NETLEAF_QUICTLS_H */
