/*
 * NetLeaf QUIC 扩展 —— 原主库 H3/QUIC 纯逻辑（legacy 迁移件）。
 *
 * 来源：v3.0.0 之前随主库分发的 src/http/netleaf_http_common.c 中的
 * QUIC varint / HTTP/3 流与帧处理 / Initial 应答构造 / 可插拔 AEAD 段。
 * 随 P1 重构（QUIC/HTTP3 拆出主库、与 quictls 合并为独立扩展 src/quic）
 * 整体迁入本文件，供 quic 扩展后续开发继续使用。
 *
 * 说明：
 *  - 本文件为"零密钥演示态"的 HTTP/3 + QUIC 传输纯逻辑（RFC 9000 varint、
 *    H3 流/帧处理、Initial 应答构造、可插拔 AEAD 默认实现），不直接依赖
 *    任何平台 socket 头；UDP 发送经 nl_http_io 函数表注入。
 *  - 结构体 / 常量 / 枚举随代码一并迁入并保持自包含（原 netleaf_http_internal.h
 *    与 netleaf_http.h 中的 QUIC/H3 部分已从主库删除）。
 *  - hpack 静态表 / 解析方法 / hpack_init/free 为原主库 common.c 的本地
 *    static 副本（扩展独立编译，不复用主库内部符号）。
 *  - quic_generate_conn_id 原为平台钩子（POSIX getrandom / Win BCrypt），
 *    迁入后改用 mbedTLS CTR_DRBG（扩展已链接 mbedcrypto），保持平台无关。
 *
 * 状态：随 P1 暂封存、暂不再开发；后续与 quictls 自研 QUIC-TLS 融合时以此为基。
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "optimize/netleaf_http.h"  /* nl_http_handler / nlh_http_method_t / nl_http_version_t */

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

/* ============================================================
 * 常量（随 QUIC/H3 代码自 netleaf_http_internal.h 迁入）
 * ============================================================ */
#define MAX_HEADERS 64
#define MAX_HEADER_NAME 128
#define MAX_HEADER_VALUE 1024
#define NL_HTTP_MAX_PATH 4096

#define H3_DEFAULT_MAX_STREAMS 100
#define H3_BUFFER_SIZE 65536
#define QUIC_VERSION_1 0x00000001
#define QUIC_MAX_CONN_ID_LEN 20
#define QUIC_INITIAL_MAX_DATA 1048576
#define QUIC_INITIAL_MAX_STREAM_DATA 131072
#define QUIC_DEFAULT_MAX_STREAMS_BIDI 100
#define QUIC_DEFAULT_MAX_STREAMS_UNI 100

#define NL_QUIC_AEAD_TAG_LEN 16   /* Poly1305 / GCM 认证标签长度 */

/* ============================================================
 * 枚举 / 值类型（随 QUIC/H3 代码自 netleaf_http.h 迁入）
 * ============================================================ */

/* HTTP/3 帧类型（RFC 9114 §7.2） */
typedef enum {
    NL_H3_FRAME_DATA = 0x0,
    NL_H3_FRAME_HEADERS = 0x1,
    NL_H3_FRAME_CANCEL_PUSH = 0x3,
    NL_H3_FRAME_SETTINGS = 0x4,
    NL_H3_FRAME_PUSH_PROMISE = 0x5,
    NL_H3_FRAME_GOAWAY = 0x7,
    NL_H3_FRAME_MAX_PUSH_ID = 0xD
} nl_h3_frame_type_t;

/* QUIC 数据包类型（RFC 9000 §17） */
typedef enum {
    NL_QUIC_PACKET_INITIAL = 0x0,
    NL_QUIC_PACKET_0RTT = 0x1,
    NL_QUIC_PACKET_HANDSHAKE = 0x2,
    NL_QUIC_PACKET_RETRY = 0x3,
    NL_QUIC_PACKET_VERSION_NEGOTIATION = 0x4,
    NL_QUIC_PACKET_SHORT = 0x5
} nl_quic_packet_type_t;

/* QUIC 帧类型（RFC 9000 §19） */
typedef enum {
    NL_QUIC_FRAME_PADDING = 0x0,
    NL_QUIC_FRAME_PING = 0x1,
    NL_QUIC_FRAME_ACK = 0x2,
    NL_QUIC_FRAME_ACK_ECN = 0x3,
    NL_QUIC_FRAME_RESET_STREAM = 0x4,
    NL_QUIC_FRAME_STOP_SENDING = 0x5,
    NL_QUIC_FRAME_CRYPTO = 0x6,
    NL_QUIC_FRAME_NEW_TOKEN = 0x7,
    NL_QUIC_FRAME_STREAM = 0x8,
    NL_QUIC_FRAME_STREAM_FIN = 0x9,
    NL_QUIC_FRAME_MAX_DATA = 0x10,
    NL_QUIC_FRAME_MAX_STREAM_DATA = 0x11,
    NL_QUIC_FRAME_MAX_STREAMS_BIDI = 0x12,
    NL_QUIC_FRAME_MAX_STREAMS_UNI = 0x13,
    NL_QUIC_FRAME_DATA_BLOCKED = 0x14,
    NL_QUIC_FRAME_STREAM_DATA_BLOCKED = 0x15,
    NL_QUIC_FRAME_STREAMS_BLOCKED_BIDI = 0x16,
    NL_QUIC_FRAME_STREAMS_BLOCKED_UNI = 0x17,
    NL_QUIC_FRAME_NEW_CONNECTION_ID = 0x18,
    NL_QUIC_FRAME_RETIRE_CONNECTION_ID = 0x19,
    NL_QUIC_FRAME_PATH_CHALLENGE = 0x1A,
    NL_QUIC_FRAME_PATH_RESPONSE = 0x1B,
    NL_QUIC_FRAME_CONNECTION_CLOSE = 0x1C,
    NL_QUIC_FRAME_HANDSHAKE_DONE = 0x1E
} nl_quic_frame_type_t;

/* QUIC 连接状态 */
typedef enum {
    NL_QUIC_STATE_INIT,
    NL_QUIC_STATE_HANDSHAKE,
    NL_QUIC_STATE_ESTABLISHED,
    NL_QUIC_STATE_CLOSING,
    NL_QUIC_STATE_DRAINING,
    NL_QUIC_STATE_CLOSED
} nl_quic_state_t;

/* QUIC 流状态 */
typedef enum {
    NL_QUIC_STREAM_STATE_IDLE,
    NL_QUIC_STREAM_STATE_OPEN,
    NL_QUIC_STREAM_STATE_HALF_CLOSED_LOCAL,
    NL_QUIC_STREAM_STATE_HALF_CLOSED_REMOTE,
    NL_QUIC_STREAM_STATE_CLOSED
} nl_quic_stream_state_t;

/* 连接 ID（RFC 9000 §5.1，最长 20 字节） */
typedef struct {
    uint8_t data[20];
    uint8_t len;
} nl_quic_conn_id_t;

/* 传输参数（RFC 9000 §18）——演示态仅取整数字段 */
typedef struct {
    uint64_t original_destination_connection_id;
    uint64_t max_idle_timeout;
    uint64_t stateless_reset_token;
    uint64_t max_udp_payload_size;
    uint64_t initial_max_data;
    uint64_t initial_max_stream_data_bidi_local;
    uint64_t initial_max_stream_data_bidi_remote;
    uint64_t initial_max_stream_data_uni;
    uint64_t initial_max_streams_bidi;
    uint64_t initial_max_streams_uni;
    uint64_t ack_delay_exponent;
    uint64_t max_ack_delay;
    uint64_t disable_active_migration;
    uint64_t active_connection_id_limit;
    uint64_t initial_source_connection_id;
    uint64_t retry_source_connection_id;
} nl_quic_transport_params_t;

/* ============================================================
 * 结构体（随 QUIC/H3 代码自 netleaf_http_internal.h 迁入）
 * ============================================================ */

struct nl_http_header {
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];
};

struct nl_http_request {
    nlh_http_method_t method;
    nl_http_version_t version;
    char path[NL_HTTP_MAX_PATH];
    struct nl_http_header headers[MAX_HEADERS];
    int header_count;
    char* body;
    size_t body_size;
};

struct nl_http_response {
    int status;
    struct nl_http_header headers[MAX_HEADERS];
    int header_count;
    char* body;
    size_t body_size;
};

struct hpack_dynamic_entry {
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];
};

struct hpack_context {
    struct hpack_dynamic_entry* dynamic_table;
    int dynamic_table_size;
    int max_dynamic_table_size;
};

/* QUIC 流（HTTP/3 请求流） */
struct nl_http3_stream {
    uint64_t id;
    nl_quic_stream_state_t state;
    uint64_t recv_offset;
    uint64_t send_offset;
    uint64_t recv_window;
    uint64_t send_window;
    nl_http_request_t request;
    nl_http_response_t response;
    uint8_t* recv_buffer;
    size_t recv_buffer_len;
    size_t recv_buffer_size;
    struct nl_http3_stream* next;
};

/*
 * 平台相关的"句柄"类型：POSIX 下为 int fd，Windows 下为 SOCKET。
 * 本文件自包含，默认 int（扩展侧无平台 socket 依赖，UDP 发送经
 * nl_http_io 函数表注入）。
 */
#ifndef NL_HTTPS_SOCK
#define NL_HTTPS_SOCK int
#endif
typedef NL_HTTPS_SOCK nl_http_io_fd;
typedef nl_http_io_fd nl_http_handle_t;

#ifndef NL_HTTP_THREAD
#define NL_HTTP_THREAD void*
#endif
#ifndef NL_HTTP_MUTEX
#define NL_HTTP_MUTEX void*
#endif

/* H3/QUIC 连接（fd 为平台 UDP socket，client_addr 为平台 sockaddr）。 */
struct nl_http3_connection {
    nl_http_handle_t fd;
    void* client_addr;
    size_t client_addr_len;
    nl_quic_conn_id_t dest_conn_id;
    nl_quic_conn_id_t src_conn_id;
    nl_quic_conn_id_t original_conn_id;
    uint64_t packet_number;
    nl_quic_state_t state;
    nl_quic_transport_params_t local_params;
    nl_quic_transport_params_t remote_params;
    uint64_t max_data;
    uint64_t max_streams_bidi;
    uint64_t max_streams_uni;
    uint64_t streams_bidi_count;
    uint64_t streams_uni_count;
    struct hpack_context hpack;
    struct nl_http3_stream* streams;
    uint8_t* send_buffer;
    size_t send_buffer_len;
    size_t send_buffer_size;
    NL_HTTP_MUTEX mutex;
    struct nl_http3_connection* next;
};

/* H3/QUIC 服务端：UDP 句柄 + 服务端连接 ID + 默认传输参数。 */
struct nl_http3_server {
    nl_http_handle_t fd;
    int port;
    int running;
    NL_HTTP_THREAD thread;
    nl_http_handler handler;
    void* user_data;
    struct nl_http3_connection* connections;
    nl_quic_conn_id_t server_conn_id;
    nl_quic_transport_params_t default_params;
};

/* ============================================================
 * I/O 函数表（平台实现，公共逻辑调用；仅保留 UDP 路径）
 * ============================================================ */
typedef struct nl_http_io nl_http_io_t;

struct nl_http_io {
    /* 发送一个 UDP（HTTP3/QUIC）报文给指定客户端地址。
     * 成功返回 0，失败返回 -1。client_addr/len 由平台填充。 */
    int (*udp_send)(struct nl_http_io* io, nl_http_handle_t handle,
                    const uint8_t* data, size_t len,
                    void* client_addr, size_t client_addr_len);
};

/* ============================================================
 * QUIC 可插拔加密（AEAD），随代码自 netleaf_http_internal.h 迁入
 * ============================================================ */

typedef struct {
    uint8_t key[32];
    uint8_t key_len;
    uint8_t nonce[12];
    uint8_t nonce_len;
    int real_key;   /* 1 = 真实密钥就绪（seal/open 走 AEAD），0 = 零密钥演示态 */
} nl_quic_aead_ctx_t;

typedef struct nl_quic_crypto nl_quic_crypto_t;

struct nl_quic_crypto {
    void* opaque;               /* mbedTLS 句柄 / 平台 opaque 指针 / NULL */
    nl_quic_aead_ctx_t aead;    /* 协商出的密钥/nonce 上下文 */
    int (*seal)(struct nl_quic_crypto* crypto, nl_quic_aead_ctx_t* ctx,
                const uint8_t* in, size_t in_len,
                uint64_t pkt_num,
                const uint8_t* aad, size_t aad_len,
                uint8_t* out);
    int (*open)(struct nl_quic_crypto* crypto, nl_quic_aead_ctx_t* ctx,
                const uint8_t* in, size_t in_len,
                uint64_t pkt_num,
                const uint8_t* aad, size_t aad_len,
                uint8_t* out);
    void (*free_ctx)(struct nl_quic_crypto* crypto);
};

/* 当前生效的 crypto（定义在文件末尾，默认指向零密钥演示态实例）。 */
extern struct nl_quic_crypto* nl_quic_crypto_active;

/* 注入自定义 crypto（如 tls3 扩展持有 mbedTLS 句柄的实例）。传 NULL 卸载。 */
void nl_http_quic_crypto_install(struct nl_quic_crypto* crypto);

/* 内置零密钥 crypto：未协商时自用的演示态实现（seal/open 直接拷贝）。 */
nl_quic_crypto_t* nl_http_quic_crypto_default(void);

/* ============================================================
 * 函数原型（本文件内部，与原主库签名保持一致）
 * ============================================================ */
size_t quic_read_varint(const uint8_t* data, size_t len, uint64_t* value);
size_t quic_write_varint(uint8_t* data, size_t len, uint64_t value);
struct nl_http3_stream* h3_find_or_create_stream(struct nl_http3_connection* conn, uint64_t stream_id);
void h3_stream_free(struct nl_http3_stream* stream);
int h3_process_stream_frame(struct nl_http_io* io, struct nl_http3_connection* conn, struct nl_http3_stream* stream,
                            const uint8_t* data, size_t len, nl_http_handler handler, void* user_data);
int h3_process_quic_packet(struct nl_http_io* io, struct nl_http3_server* server, struct nl_http3_connection* conn,
                           const uint8_t* data, size_t len, nl_http_handler handler, void* user_data);
void quic_generate_conn_id(nl_quic_conn_id_t* conn_id, uint8_t len);
void h3_connection_init(struct nl_http3_connection* conn, nl_http_handle_t fd,
                        void* client_addr, size_t client_addr_len,
                        struct nl_http3_server* server);
void h3_connection_free(struct nl_http3_connection* conn);
int quic_varint_roundtrip(const uint64_t* values, size_t count,
                          uint8_t* out, size_t out_cap, size_t* out_used);

/* ============================================================
 * HPACK 本地 static 副本（自原主库 netleaf_http_common.c 复制；
 * 扩展独立编译，不复用主库内部符号）
 * ============================================================ */

static const char* hpack_static_table[][2] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""}
};

static nlh_http_method_t nl_http_parse_method(const char* method) {
    if (strcmp(method, "GET") == 0) return NL_HTTP_GET;
    if (strcmp(method, "POST") == 0) return NL_HTTP_POST;
    if (strcmp(method, "PUT") == 0) return NL_HTTP_PUT;
    if (strcmp(method, "DELETE") == 0) return NL_HTTP_DELETE;
    if (strcmp(method, "HEAD") == 0) return NL_HTTP_HEAD;
    if (strcmp(method, "OPTIONS") == 0) return NL_HTTP_OPTIONS;
    if (strcmp(method, "PATCH") == 0) return NL_HTTP_PATCH;
    return NL_HTTP_UNKNOWN;
}

static void hpack_init(struct hpack_context* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->max_dynamic_table_size = 4096;
}

static void hpack_free(struct hpack_context* ctx) {
    if (!ctx) return;
    free(ctx->dynamic_table);
    ctx->dynamic_table = NULL;
    ctx->dynamic_table_size = 0;
}

/* ============================================================
 * 连接 ID 生成（原为平台钩子：POSIX getrandom / Win BCrypt；
 * 迁入扩展后改用 mbedTLS CTR_DRBG，保持平台无关且无硬编码熵源）
 * ============================================================ */
void quic_generate_conn_id(nl_quic_conn_id_t* conn_id, uint8_t len) {
    static mbedtls_entropy_context entropy;
    static mbedtls_ctr_drbg_context ctr_drbg;
    static int rng_ready = 0;
    static const unsigned char personalization[] = "netleaf-quic-connid";

    if (!conn_id) return;
    if (len == 0 || len > QUIC_MAX_CONN_ID_LEN) len = QUIC_MAX_CONN_ID_LEN;
    conn_id->len = len;

    if (!rng_ready) {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                  personalization, sizeof(personalization) - 1) != 0) {
            /* 种子失败：清零（演示态退化为全 0 连接 ID，不中断流程） */
            memset(conn_id->data, 0, len);
            return;
        }
        rng_ready = 1;
    }

    if (mbedtls_ctr_drbg_random(&ctr_drbg, conn_id->data, len) != 0) {
        memset(conn_id->data, 0, len);
    }
}

/* ============================================================
 * QUIC varint（RFC 9000 §16）+ HTTP/3 流处理
 * ============================================================ */

size_t quic_read_varint(const uint8_t* data, size_t len, uint64_t* value) {
    if (len < 1) return 0;

    uint8_t first_byte = data[0];
    uint8_t prefix = (first_byte >> 6) & 0x03;

    if (prefix == 0 && len >= 1) {
        *value = first_byte & 0x3F;
        return 1;
    } else if (prefix == 1 && len >= 2) {
        *value = ((uint64_t)(first_byte & 0x3F) << 8) | data[1];
        return 2;
    } else if (prefix == 2 && len >= 4) {
        *value = ((uint64_t)(first_byte & 0x3F) << 24) |
                 ((uint64_t)data[1] << 16) |
                 ((uint64_t)data[2] << 8) |
                 data[3];
        return 4;
    } else if (prefix == 3 && len >= 8) {
        *value = ((uint64_t)(first_byte & 0x3F) << 56) |
                 ((uint64_t)data[1] << 48) |
                 ((uint64_t)data[2] << 40) |
                 ((uint64_t)data[3] << 32) |
                 ((uint64_t)data[4] << 24) |
                 ((uint64_t)data[5] << 16) |
                 ((uint64_t)data[6] << 8) |
                 data[7];
        return 8;
    }

    return 0;
}

size_t quic_write_varint(uint8_t* data, size_t len, uint64_t value) {
    if (value < 64 && len >= 1) {
        data[0] = (uint8_t)value;
        return 1;
    } else if (value < 16384 && len >= 2) {
        data[0] = 0x40 | (uint8_t)(value >> 8);
        data[1] = (uint8_t)(value & 0xFF);
        return 2;
    } else if (value < 1073741824 && len >= 4) {
        data[0] = 0x80 | (uint8_t)(value >> 24);
        data[1] = (uint8_t)((value >> 16) & 0xFF);
        data[2] = (uint8_t)((value >> 8) & 0xFF);
        data[3] = (uint8_t)(value & 0xFF);
        return 4;
    } else if (value < (1ULL << 62) && len >= 8) {
        /* 8 字节档：QUIC varint 实际可表示 0..2^62-1（prefix=3 占用 2 位） */
        data[0] = 0xC0 | (uint8_t)(value >> 56);
        data[1] = (uint8_t)((value >> 48) & 0xFF);
        data[2] = (uint8_t)((value >> 40) & 0xFF);
        data[3] = (uint8_t)((value >> 32) & 0xFF);
        data[4] = (uint8_t)((value >> 24) & 0xFF);
        data[5] = (uint8_t)((value >> 16) & 0xFF);
        data[6] = (uint8_t)((value >> 8) & 0xFF);
        data[7] = (uint8_t)(value & 0xFF);
        return 8;
    }
    return 0;
}

struct nl_http3_stream* h3_find_or_create_stream(struct nl_http3_connection* conn, uint64_t stream_id) {
    struct nl_http3_stream* stream = conn->streams;
    while (stream) {
        if (stream->id == stream_id) return stream;
        stream = stream->next;
    }

    stream = calloc(1, sizeof(*stream));
    if (!stream) return NULL;
    stream->id = stream_id;
    stream->state = NL_QUIC_STREAM_STATE_IDLE;
    stream->recv_window = QUIC_INITIAL_MAX_STREAM_DATA;
    stream->send_window = QUIC_INITIAL_MAX_STREAM_DATA;
    stream->next = conn->streams;
    conn->streams = stream;

    return stream;
}

void h3_stream_free(struct nl_http3_stream* stream) {
    if (stream) {
        if (stream->recv_buffer) free(stream->recv_buffer);
        if (stream->request.body) free(stream->request.body);
        if (stream->response.body) free(stream->response.body);
        free(stream);
    }
}

int h3_process_stream_frame(struct nl_http_io* io, struct nl_http3_connection* conn, struct nl_http3_stream* stream,
                            const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
    (void)user_data;
    if (len < 1) return -1;

    uint8_t frame_type = data[0];
    size_t offset = 1;

    uint64_t frame_length = 0;
    size_t varint_len = quic_read_varint(data + offset, len - offset, &frame_length);
    if (varint_len == 0) return -1;
    offset += varint_len;

    if (offset + frame_length > len) return -1;

    switch (frame_type) {
        case NL_H3_FRAME_DATA: {
            /* 数据帧：追加到 recv_buffer（动态扩容） */
            if (frame_length > 0) {
                if (!stream->recv_buffer) {
                    stream->recv_buffer_size = 8192;
                    stream->recv_buffer = calloc(1, stream->recv_buffer_size);
                }

                if (stream->recv_buffer_len + frame_length > stream->recv_buffer_size) {
                    size_t new_size = stream->recv_buffer_size ? stream->recv_buffer_size : 8192;
                    while (new_size < stream->recv_buffer_len + frame_length) new_size *= 2;
                    uint8_t* new_buffer = realloc(stream->recv_buffer, new_size);
                    if (new_buffer) {
                        stream->recv_buffer = new_buffer;
                        stream->recv_buffer_size = new_size;
                    }
                }

                if (stream->recv_buffer) {
                    memcpy(stream->recv_buffer + stream->recv_buffer_len, data + offset, (size_t)frame_length);
                    stream->recv_buffer_len += (size_t)frame_length;
                    stream->recv_offset += frame_length;
                }
            }
            break;
        }

        case NL_H3_FRAME_HEADERS: {
            /* 头部帧：简化 hpack 静态表解析 + 调用 handler + 回响应 */
            stream->request.version = NL_HTTP_VERSION_3;
            stream->request.method = NL_HTTP_GET; /* 默认方法 */

            size_t header_offset = 0;
            while (header_offset < frame_length) {
                uint64_t index = 0;
                size_t index_len = quic_read_varint(data + offset + header_offset, (size_t)(frame_length - header_offset), &index);
                if (index_len == 0) break;

                if (index < 61) {
                    const char* name = hpack_static_table[index][0];
                    const char* value = hpack_static_table[index][1];

                    if (strcmp(name, ":method") == 0 && strlen(value) > 0) {
                        stream->request.method = nl_http_parse_method(value);
                    } else if (strcmp(name, ":path") == 0 && strlen(value) > 0) {
                        snprintf(stream->request.path, NL_HTTP_MAX_PATH, "%s", value);
                    } else if (name[0] != ':' && stream->request.header_count < MAX_HEADERS) {
                        snprintf(stream->request.headers[stream->request.header_count].name, MAX_HEADER_NAME, "%s", name);
                        snprintf(stream->request.headers[stream->request.header_count].value, MAX_HEADER_VALUE, "%s", value);
                        stream->request.header_count++;
                    }
                }

                header_offset += index_len;
            }

            /* 默认路径 */
            if (stream->request.path[0] == '\0') {
                snprintf(stream->request.path, NL_HTTP_MAX_PATH, "/");
            }

            /* 调用 handler 并回包 */
            if (handler) {
                nl_http_response_t resp = {0};
                resp.status = 200;
                handler(&stream->request, &resp, user_data);

                /* 用连接的堆 send_buffer 组装回包（原先两次 64KB 栈缓冲
                 * 合并复用，削减每请求 128KB 栈占用）。 */
                if (conn->send_buffer) {
                    size_t cap = conn->send_buffer_size;

                    /* 头部帧 */
                    size_t resp_offset = 0;
                    conn->send_buffer[resp_offset++] = NL_H3_FRAME_HEADERS;
                    resp_offset += quic_write_varint(conn->send_buffer + resp_offset, cap - resp_offset, 10);
                    conn->send_buffer[resp_offset++] = 0x88; /* 静态表 :status=200 (idx 8, 1-based) */

                    for (int i = 0; i < resp.header_count && resp_offset + 32 < cap; i++) {
                        conn->send_buffer[resp_offset++] = 0x00;
                        resp_offset += quic_write_varint(conn->send_buffer + resp_offset, cap - resp_offset, 0);
                        size_t name_len = strlen(resp.headers[i].name);
                        resp_offset += quic_write_varint(conn->send_buffer + resp_offset, cap - resp_offset, name_len);
                        memcpy(conn->send_buffer + resp_offset, resp.headers[i].name, name_len);
                        resp_offset += name_len;
                        size_t value_len = strlen(resp.headers[i].value);
                        resp_offset += quic_write_varint(conn->send_buffer + resp_offset, cap - resp_offset, value_len);
                        memcpy(conn->send_buffer + resp_offset, resp.headers[i].value, value_len);
                        resp_offset += value_len;
                    }

                    io->udp_send(io, conn->fd, conn->send_buffer, resp_offset,
                                 &conn->client_addr, conn->client_addr_len);

                    /* 数据帧（若有 body） */
                    if (resp.body && resp.body_size > 0 &&
                        resp.body_size < cap) {
                        size_t data_offset = 0;
                        conn->send_buffer[data_offset++] = NL_H3_FRAME_DATA;
                        data_offset += quic_write_varint(conn->send_buffer + data_offset, cap - data_offset, resp.body_size);
                        memcpy(conn->send_buffer + data_offset, resp.body, resp.body_size);
                        data_offset += resp.body_size;

                        io->udp_send(io, conn->fd, conn->send_buffer, data_offset,
                                     &conn->client_addr, conn->client_addr_len);
                    }

                    if (resp.body) free(resp.body);
                }
            }
            break;
        }

        case NL_H3_FRAME_SETTINGS:
            /* 远端 H3 SETTINGS 透传（本演示骨架未逐字段解析） */
            break;

        case NL_H3_FRAME_GOAWAY:
            conn->state = NL_QUIC_STATE_CLOSING;
            break;
    }

    /* 以下为 QUIC 传输层帧（RFC 9000）：在 H3 控制/请求流上到达，
     * 与 H3 应用帧共用 STREAM 载荷，故在同一 switch 处按帧号处理。 */
    switch (frame_type) {
        case NL_QUIC_FRAME_MAX_DATA: {
            if (frame_length < 8) return 0;
            uint64_t max_data = 0;
            quic_read_varint(data + offset, (size_t)frame_length, &max_data);
            conn->max_data = max_data;
            break;
        }
        case NL_QUIC_FRAME_MAX_STREAM_DATA: {
            if (frame_length < 16 || !stream) return 0;
            uint64_t s_id = 0, ms = 0;
            size_t o = quic_read_varint(data + offset, (size_t)frame_length, &s_id);
            if (o == 0) return 0;
            quic_read_varint(data + offset + o, (size_t)(frame_length - o), &ms);
            if (stream->id == s_id) stream->recv_window = ms;
            break;
        }
        case NL_QUIC_FRAME_MAX_STREAMS_BIDI:
        case NL_QUIC_FRAME_MAX_STREAMS_UNI: {
            if (frame_length < 8) return 0;
            uint64_t max_streams = 0;
            quic_read_varint(data + offset, (size_t)frame_length, &max_streams);
            conn->max_streams_bidi = max_streams;
            break;
        }
        case NL_QUIC_FRAME_ACK: {
            /* ACK 帧：解析 largest_acknowledged + ACK range，仅记录，不影响逻辑 */
            uint64_t largest_ack = 0;
            size_t o = quic_read_varint(data + offset, (size_t)frame_length, &largest_ack);
            if (o == 0) return 0;
            uint64_t ack_delay = 0, count = 0;
            o = quic_read_varint(data + offset + o, (size_t)(frame_length - o), &ack_delay);
            if (o == 0) return 0;
            quic_read_varint(data + offset + o * 2, (size_t)(frame_length - o * 2), &count);
            (void)count;
            /* 演示骨架：无需回 ACK-ACK，仅保留包号同步 */
            if (largest_ack > conn->packet_number) conn->packet_number = largest_ack;
            break;
        }
        case NL_QUIC_FRAME_PING:
            /* PING 无载荷，无需应答（对端可自行判定存活） */
            break;
        case NL_QUIC_FRAME_CONNECTION_CLOSE: {
            uint64_t reason_code = 0;
            quic_read_varint(data + offset, (size_t)frame_length, &reason_code);
            (void)reason_code;
            conn->state = NL_QUIC_STATE_CLOSING;
            break;
        }
        case NL_QUIC_FRAME_HANDSHAKE_DONE:
            /* 1-RTT 可开始；保留 ESTABLISHED */
            if (conn->state == NL_QUIC_STATE_HANDSHAKE) conn->state = NL_QUIC_STATE_ESTABLISHED;
            break;
        default:
            break;
    }

    return 0;
}

/* ============================================================
 * QUIC 报文处理（包类型判定 + 长/短包头 + Initial 应答构造）
 * ============================================================ */

int h3_process_quic_packet(struct nl_http_io* io, struct nl_http3_server* server, struct nl_http3_connection* conn,
                           const uint8_t* data, size_t len, nl_http_handler handler, void* user_data) {
    if (len < 1) return -1;

    uint8_t first_byte = data[0];
    nl_quic_packet_type_t packet_type;

    if ((first_byte & 0x80) == 0) {
        packet_type = NL_QUIC_PACKET_SHORT;
    } else if ((first_byte & 0x40) == 0) {
        packet_type = NL_QUIC_PACKET_VERSION_NEGOTIATION;
    } else {
        uint8_t type_bits = (first_byte >> 4) & 0x03;
        switch (type_bits) {
            case 0: packet_type = NL_QUIC_PACKET_INITIAL; break;
            case 1: packet_type = NL_QUIC_PACKET_0RTT; break;
            case 2: packet_type = NL_QUIC_PACKET_HANDSHAKE; break;
            case 3: packet_type = NL_QUIC_PACKET_RETRY; break;
            default: packet_type = NL_QUIC_PACKET_INITIAL; break;
        }
    }

    size_t offset = 1;

    if (packet_type == NL_QUIC_PACKET_VERSION_NEGOTIATION) {
        return 0;
    }

    if (packet_type != NL_QUIC_PACKET_SHORT) {
        /* 长包头：version + DCID + SCID */
        if (len < offset + 4) return -1;
        uint32_t version = ((uint32_t)data[offset] << 24) |
                           ((uint32_t)data[offset + 1] << 16) |
                           ((uint32_t)data[offset + 2] << 8) |
                           data[offset + 3];
        (void)version;
        offset += 4;

        if (len < offset + 1) return -1;
        uint8_t dcid_len = data[offset];
        offset += 1;
        if (dcid_len > 0 && len >= offset + dcid_len) {
            memcpy(conn->dest_conn_id.data, data + offset, dcid_len);
            conn->dest_conn_id.len = dcid_len;
            offset += dcid_len;
        }

        if (len < offset + 1) return -1;
        uint8_t scid_len = data[offset];
        offset += 1;
        if (scid_len > 0 && len >= offset + scid_len) {
            memcpy(conn->src_conn_id.data, data + offset, scid_len);
            conn->src_conn_id.len = scid_len;
            offset += scid_len;
        }

        if (packet_type == NL_QUIC_PACKET_INITIAL) {
            conn->state = NL_QUIC_STATE_HANDSHAKE;

            /* 复用连接级堆 send_buffer（按需分配、随连接回收），
             * 消除原每报文 64KB(H3_BUFFER_SIZE) 栈占用。 */
            if (!conn->send_buffer && conn->send_buffer_size < H3_BUFFER_SIZE) {
                conn->send_buffer = (uint8_t*)malloc(H3_BUFFER_SIZE);
                conn->send_buffer_size = conn->send_buffer ? H3_BUFFER_SIZE : 0;
                if (!conn->send_buffer) return -1;
            }
            uint8_t* response = conn->send_buffer;
            size_t resp_offset = 0;

            response[resp_offset++] = 0xC0; /* Fixed bit, long header, initial type */
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x00;
            response[resp_offset++] = 0x01;

            response[resp_offset++] = conn->src_conn_id.len;
            if (conn->src_conn_id.len > 0)
                memcpy(response + resp_offset, conn->src_conn_id.data, conn->src_conn_id.len);
            resp_offset += conn->src_conn_id.len;

            response[resp_offset++] = server->server_conn_id.len;
            if (server->server_conn_id.len > 0)
                memcpy(response + resp_offset, server->server_conn_id.data, server->server_conn_id.len);
            resp_offset += server->server_conn_id.len;

            response[resp_offset++] = NL_QUIC_FRAME_CRYPTO;
            resp_offset += quic_write_varint(response + resp_offset, conn->send_buffer_size - resp_offset, 0); /* offset */
            resp_offset += quic_write_varint(response + resp_offset, conn->send_buffer_size - resp_offset, 2);  /* length */
            response[resp_offset++] = 0x01;
            response[resp_offset++] = 0x00;

            /* 经可插拔 crypto 路径做认证加密（默认零密钥演示态；
             * tls3 扩展注入真实 AEAD 后此处自动启用真实密钥 + 标签）。 */
            if (nl_quic_crypto_active && nl_quic_crypto_active->seal) {
                uint8_t aad = 0;
                uint8_t crypt[8 + NL_QUIC_AEAD_TAG_LEN];
                int sealed = nl_quic_crypto_active->seal(nl_quic_crypto_active,
                    &nl_quic_crypto_active->aead,
                    response + resp_offset - 2, 2, conn->packet_number,
                    &aad, 1, crypt);
                if (sealed > 0) {
                    memcpy(response + resp_offset - 2, crypt, sealed);
                    resp_offset = resp_offset - 2 + sealed;
                }
            }

            io->udp_send(io, conn->fd, response, resp_offset,
                         &conn->client_addr, conn->client_addr_len);

            conn->state = NL_QUIC_STATE_ESTABLISHED; /* 演示态 */
        }
    } else {
        /* 短包头：已建立连接上的帧。
         * 先解 QUIC STREAM 帧（stream_id + offset + fin + 载荷），
         * 载荷内可能是 QUIC 传输帧（控制流）或 H3 应用帧（请求流）。 */
        if (conn->state == NL_QUIC_STATE_ESTABLISHED) {
            if (len < offset + 1) return -1;

            /* 短包头：DCID 长度字节 + packet number（演示 2 字节） */
            uint8_t dcid_len = data[offset];
            offset += 1 + dcid_len + 2;

            if (offset >= len) return 0;

            /* 解析 QUIC STREAM 帧：frame_type + varint len + varint stream_id + varint offset + FIN + payload */
            uint8_t qframe = data[offset];
            size_t qf_offset = offset + 1;
            uint64_t qframe_len = 0;
            size_t ql = quic_read_varint(data + qf_offset, len - qf_offset, &qframe_len);
            if (ql == 0) return 0;
            qf_offset += ql;

            uint64_t q_stream_id = 0;
            ql = quic_read_varint(data + qf_offset, qframe_len ? (qf_offset + qframe_len <= len ? (size_t)qframe_len : 0) : 0, &q_stream_id);
            if (ql == 0) return 0;
            qf_offset += ql;

            /* 在 STREAM 帧内进一步分发：QUIC 传输帧 vs H3 应用帧 */
            if (qframe == NL_QUIC_FRAME_STREAM || qframe == NL_QUIC_FRAME_STREAM_FIN) {
                /* H3 请求/控制流：跳过 varint stream_id/offset/FIN 后为 H3 应用帧载荷 */
                uint64_t q_stream_offset = 0;
                ql = quic_read_varint(data + qf_offset, qf_offset + qframe_len <= len ? (size_t)qframe_len : 0, &q_stream_offset);
                if (ql == 0) return 0;
                qf_offset += ql;
                /* FIN 标志在 payload 之后，忽略即可 */

                uint64_t stream_id_for_lookup = q_stream_id;
                struct nl_http3_stream* stream = h3_find_or_create_stream(conn, stream_id_for_lookup);
                if (stream) {
                    stream->recv_offset += q_stream_offset;
                    h3_process_stream_frame(io, conn, stream, data + qf_offset,
                                            (qf_offset + qframe_len <= len ? (size_t)qframe_len : 0),
                                            handler, user_data);
                }
            } else {
                /* 非 STREAM：直接对传输帧处理（控制流的传输层帧） */
                struct nl_http3_stream* ctrl = h3_find_or_create_stream(conn, 0);
                h3_process_stream_frame(io, conn, ctrl, data + offset,
                                        (offset + qframe_len <= len ? (size_t)qframe_len : 0),
                                        handler, user_data);
            }
        }
    }

    return 0;
}

/* ============================================================
 * 连接初始化 / 释放（默认值 + hpack + client_addr 拷贝）
 * ============================================================ */

void h3_connection_init(struct nl_http3_connection* conn, nl_http_handle_t fd,
                        void* client_addr, size_t client_addr_len,
                        struct nl_http3_server* server) {
    if (!conn) return;
    memset(conn, 0, sizeof(*conn));
    conn->fd = fd;

    /* 平台 sockaddr（如 sockaddr_in）按 client_addr_len 拷贝 */
    if (client_addr && client_addr_len > 0) {
        conn->client_addr = malloc(client_addr_len);
        if (conn->client_addr) {
            memcpy(conn->client_addr, client_addr, client_addr_len);
            conn->client_addr_len = client_addr_len;
        }
    }

    if (server) {
        conn->dest_conn_id = server->server_conn_id;
        quic_generate_conn_id(&conn->src_conn_id, QUIC_MAX_CONN_ID_LEN);
        conn->original_conn_id = conn->src_conn_id;
        conn->max_data = QUIC_INITIAL_MAX_DATA;
        conn->max_streams_bidi = QUIC_DEFAULT_MAX_STREAMS_BIDI;
        conn->max_streams_uni = QUIC_DEFAULT_MAX_STREAMS_UNI;
        conn->next = server->connections;
        server->connections = conn;
    } else {
        quic_generate_conn_id(&conn->src_conn_id, QUIC_MAX_CONN_ID_LEN);
        conn->original_conn_id = conn->src_conn_id;
    }

    conn->packet_number = 0;
    conn->state = NL_QUIC_STATE_INIT;
    conn->streams = NULL;

    hpack_init(&conn->hpack);
}

void h3_connection_free(struct nl_http3_connection* conn) {
    if (!conn) return;
    struct nl_http3_stream* stream = conn->streams;
    while (stream) {
        struct nl_http3_stream* next = stream->next;
        h3_stream_free(stream);
        stream = next;
    }
    hpack_free(&conn->hpack);
    if (conn->send_buffer) free(conn->send_buffer);
    if (conn->client_addr) free(conn->client_addr);
    free(conn);
}

/* ============================================================
 * QUIC 可插拔加密（内置零密钥演示态）
 *
 * 默认 crypto 仅做"拷贝 + 追加 16 字节占位标签"（无真实 AEAD，
 * aead.real_key==0）。tls3/quictls 扩展在真实密钥协商完成后，
 * 会用 mbedTLS AEAD 实现（ChaCha20-Poly1305 / AES-GCM）替换 seal/open，
 * 并把 real_key 置 1 后注入。
 * ============================================================ */
static int nl_quic_default_seal(struct nl_quic_crypto* crypto, nl_quic_aead_ctx_t* ctx,
                                const uint8_t* in, size_t in_len,
                                uint64_t pkt_num,
                                const uint8_t* aad, size_t aad_len,
                                uint8_t* out) {
    (void)crypto; (void)aad; (void)aad_len; (void)pkt_num;
    if (!ctx || !ctx->real_key) {
        /* 演示态：原样拷贝，追加 16 字节零标签，返回总长 */
        if (out) {
            memcpy(out, in, in_len);
            memset(out + in_len, 0, NL_QUIC_AEAD_TAG_LEN);
        }
        return (int)(in_len + NL_QUIC_AEAD_TAG_LEN);
    }
    /* 真实 AEAD 由注入的 seal 覆盖，这里不应被调用 */
    return -1;
}

static int nl_quic_default_open(struct nl_quic_crypto* crypto, nl_quic_aead_ctx_t* ctx,
                                const uint8_t* in, size_t in_len,
                                uint64_t pkt_num,
                                const uint8_t* aad, size_t aad_len,
                                uint8_t* out) {
    (void)crypto; (void)aad; (void)aad_len; (void)pkt_num;
    if (!ctx || !ctx->real_key) {
        /* 演示态：剥掉尾部 16 字节标签，返回明文体 */
        if (in_len <= NL_QUIC_AEAD_TAG_LEN) return -1;
        size_t plain = in_len - NL_QUIC_AEAD_TAG_LEN;
        if (out) memcpy(out, in, plain);
        return (int)plain;
    }
    return -1;
}

static void nl_quic_default_free(struct nl_quic_crypto* crypto) {
    (void)crypto;
}

static struct nl_quic_crypto nl_quic_default_crypto = {
    .opaque   = NULL,
    .aead     = { .real_key = 0 },
    .seal     = nl_quic_default_seal,
    .open     = nl_quic_default_open,
    .free_ctx = nl_quic_default_free,
};

struct nl_quic_crypto* nl_quic_crypto_active = &nl_quic_default_crypto;

void nl_http_quic_crypto_install(struct nl_quic_crypto* crypto) {
    nl_quic_crypto_active = crypto ? crypto : &nl_quic_default_crypto;
}

nl_quic_crypto_t* nl_http_quic_crypto_default(void) {
    return &nl_quic_default_crypto;
}

/* ============================================================
 * QUIC varint 编解码回环（纯内存，供单元测试 / 联调）
 * ============================================================ */

int quic_varint_roundtrip(const uint64_t* values, size_t count,
                          uint8_t* out, size_t out_cap, size_t* out_used) {
    if (!values || !out || !out_used) return 0;
    size_t off = 0;
    for (size_t i = 0; i < count; i++) {
        size_t w = quic_write_varint(out + off, out_cap - off, values[i]);
        if (w == 0) return 0;
        off += w;
    }
    *out_used = off;

    size_t p = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t v = 0;
        size_t r = quic_read_varint(out + p, off - p, &v);
        if (r == 0 || v != values[i]) return 0;
        p += r;
    }
    return 1;
}
