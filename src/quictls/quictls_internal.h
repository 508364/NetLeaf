/*
 * NetLeaf QUIC-TLS 扩展内部共享头（仅扩展内部使用，不对外安装）。
 *
 * 提供跨源文件复用的低层原语：
 *   - RFC 8446 §7.1 的 HKDF-Extract / HKDF-Expand / HKDF-Expand-Label
 * 定义在 netleaf_quictls.c，供 quictls_packet.c / quictls_tls.c 复用。
 */
#ifndef NETLEAF_QUICTLS_INTERNAL_H
#define NETLEAF_QUICTLS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#define NLQ_HASH_LEN 32   /* SHA-256 输出长度 */

/* HKDF-Extract（HMAC-SHA256），out 固定 32 字节 */
int nl_quic__hkdf_extract(const uint8_t* salt, size_t salt_len,
                          const uint8_t* ikm, size_t ikm_len,
                          uint8_t out[NLQ_HASH_LEN]);

/* HKDF-Expand（HMAC-SHA256） */
int nl_quic__hkdf_expand(const uint8_t* prk, size_t prk_len,
                         const uint8_t* info, size_t info_len,
                         uint8_t* out, size_t out_len);

/* HKDF-Expand-Label（label 不含 "tls13 " 前缀，函数内部自动补） */
int nl_quic__expand_label(const uint8_t* secret, size_t secret_len,
                          const char* label,
                          const uint8_t* context, size_t context_len,
                          uint8_t* out, size_t out_len);

#endif /* NETLEAF_QUICTLS_INTERNAL_H */
