/*
 * NetLeaf HTTP/2/3 内部公共头（平台无关部分）。
 *
 * 说明：
 *  - 原先 linux/macos/windows 三份 netleaf_http_*.c 重复了 ~57 个函数。
 *    其中 ~40 个是纯逻辑（parse/hpack/h2 帧/h3/quic-varint/accessor），
 *    已抽到 src/http/netleaf_http_common.c，只保留一份。
 *  - 本头声明被 common.c 与各平台 .c 共享的常量、结构体、枚举、纯逻辑
 *    函数原型，以及一个 I/O 函数表（nl_http_io_t）。
 *  - 平台相关代码（socket/线程/锁/RNG/生命周期/HTTPS）留在各平台 .c，
 *    通过 nl_http_io_t 把"收发一个 h2 帧、收发 UDP、互斥锁"注入公共逻辑，
 *    使公共层不直接依赖任一平台的 <sys/socket.h>/<pthread.h>/<winsock2.h>。
 *  - h2 帧头统一为"普通 uint32 length + 31-bit stream_id"（无位域），
 *    消除原先 windows 用 plain uint32、posix 用位域的不一致。
 *
 * 注意：本头直接定义连接/服务器结构体（nl_http2_connection /
 * nl_http3_connection / nl_http2_server / nl_http3_server），其句柄/线程/锁
 * 字段随平台宏（NL_HTTPS_SOCK / NL_HTTP_THREAD / NL_HTTP_MUTEX）变化；
 * 平台 .c 在 include 本头之前先 #define 这些宏。
 * nl_http_server / nl_http_client（H1，依赖 TLS 构建配置）仍由各平台 .c 定义。
 */
#ifndef NETLEAF_HTTP_INTERNAL_H
#define NETLEAF_HTTP_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#include "netleaf_http.h"

/* ============================================================
 * 常量
 * ============================================================ */
#define MAX_HEADERS 64
#define MAX_HEADER_NAME 128
#define MAX_HEADER_VALUE 1024
#define NL_HTTP_MAX_PATH 4096
#define BUFFER_SIZE 16384

#define H2_DEFAULT_WINDOW_SIZE 65535
#define H2_MAX_FRAME_SIZE 16384
#define H2_INITIAL_SETTINGS_COUNT 6

#define H3_DEFAULT_MAX_STREAMS 100
#define H3_BUFFER_SIZE 65536
#define QUIC_VERSION_1 0x00000001
#define QUIC_MAX_CONN_ID_LEN 20
#define QUIC_INITIAL_MAX_DATA 1048576
#define QUIC_INITIAL_MAX_STREAM_DATA 131072
#define QUIC_DEFAULT_MAX_STREAMS_BIDI 100
#define QUIC_DEFAULT_MAX_STREAMS_UNI 100
#define QUIC_DEFAULT_MAX_IDLE_TIMEOUT 60000

/* ============================================================
 * 公共结构体 / 枚举
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

/* h2 帧头：统一用普通 uint32 存 length/stream_id，无位域。 */
struct nl_h2_frame_header {
    uint32_t length;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
};

struct nl_h2_settings {
    uint32_t header_table_size;
    uint32_t enable_push;
    uint32_t max_concurrent_streams;
    uint32_t initial_window_size;
    uint32_t max_frame_size;
    uint32_t max_header_list_size;
};

struct nl_http2_stream {
    uint32_t id;
    int state;
    int window_size;
    nl_http_request_t request;
    nl_http_response_t response;
    struct nl_http2_stream* next;
};

/* QUIC / HTTP3 状态与参数 */
typedef enum {
    NL_QUIC_STATE_INIT,
    NL_QUIC_STATE_HANDSHAKE,
    NL_QUIC_STATE_ESTABLISHED,
    NL_QUIC_STATE_CLOSING,
    NL_QUIC_STATE_DRAINING,
    NL_QUIC_STATE_CLOSED
} nl_quic_state_t;

typedef enum {
    NL_QUIC_STREAM_STATE_IDLE,
    NL_QUIC_STREAM_STATE_OPEN,
    NL_QUIC_STREAM_STATE_HALF_CLOSED_LOCAL,
    NL_QUIC_STREAM_STATE_HALF_CLOSED_REMOTE,
    NL_QUIC_STREAM_STATE_CLOSED
} nl_quic_stream_state_t;

typedef struct {
    uint8_t data[20];
    uint8_t len;
} nl_quic_conn_id_t;

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

struct hpack_huffman_node {
    int symbol;
    int bits;
};

struct hpack_dynamic_entry {
    char name[MAX_HEADER_NAME];
    char value[MAX_HEADER_VALUE];
};

struct hpack_context {
    /* 动态表按需分配（原先 value-embed 4096 条 ≈4.7MB 且从不读写，纯死重；
     * 现改为指针，hpack_init 不分配、hpack_free 释放，连接结构体瘦身）。 */
    struct hpack_dynamic_entry* dynamic_table;
    int dynamic_table_size;
    int max_dynamic_table_size;
};

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
 * 各平台 .c 在 include 本头之前先 #define NL_HTTPS_SOCK 为对应句柄类型，
 * 本头据此推导 nl_http_io_fd；未定义时默认 int。
 * 这样公共层可引用 nl_http_handle_t 而不依赖 <sys/socket.h>/<winsock2.h>。
 */
#ifndef NL_HTTPS_SOCK
#define NL_HTTPS_SOCK int
#endif
typedef NL_HTTPS_SOCK nl_http_io_fd;
typedef nl_http_io_fd nl_http_handle_t;

/*
 * 线程句柄 / 互斥锁：由各平台 .c 在 include 本头之前分别定义
 * NL_HTTP_THREAD / NL_HTTP_MUTEX。连接/服务器结构体放在本头，
 * 使公共逻辑可直接访问其字段，同时句柄/线程/锁类型随平台变化。
 */
#ifndef NL_HTTP_THREAD
#define NL_HTTP_THREAD void*
#endif
#ifndef NL_HTTP_MUTEX
#define NL_HTTP_MUTEX void*
#endif

/*
 * 锁操作钩子：公共逻辑（h2/h3_process_frame 等）可能并发访问连接字段，
 * 需要加锁。平台 .c 在 include 本头之前分别定义这三者为对应平台原语：
 *   - POSIX:   pthread_mutex_init/unlock... (pthread_mutex_t)
 *   - Windows: InitializeCriticalSection/... (CRITICAL_SECTION)
 * 默认空实现，保证无平台头时本文件仍可编译。
 */
#ifndef NL_HTTP_MUTEX_INIT
#define NL_HTTP_MUTEX_INIT(m) do {} while (0)
#endif
#ifndef NL_HTTP_MUTEX_DESTROY
#define NL_HTTP_MUTEX_DESTROY(m) do {} while (0)
#endif
#ifndef NL_HTTP_MUTEX_LOCK
#define NL_HTTP_MUTEX_LOCK(m) do {} while (0)
#endif
#ifndef NL_HTTP_MUTEX_UNLOCK
#define NL_HTTP_MUTEX_UNLOCK(m) do {} while (0)
#endif

/* H2 连接：平台 .c 持有；公共逻辑经 io 表写帧、经字段存流/窗口。 */
struct nl_http2_connection {
    nl_http_handle_t fd;
    struct hpack_context hpack;
    struct nl_h2_settings local_settings;
    struct nl_h2_settings remote_settings;
    int connection_window_size;
    int preface_sent;
    int settings_ack_received;
    struct nl_http2_stream* streams;
    NL_HTTP_MUTEX mutex;
    /* CONTINUATION：跨帧累积未完成的 HEADERS 头块（END_HEADERS 未置位时续帧） */
    uint8_t* pending_headers_block;
    size_t pending_headers_len;
    uint32_t pending_headers_stream;
};

/* H3/QUIC 连接：平台 .c 持有（fd 为平台 UDP socket，client_addr 为平台 sockaddr）。 */
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

/* H2 服务端：平台句柄 + 线程/处理器；connections 为平台 H3/连接链表。 */
struct nl_http2_server {
    nl_http_handle_t fd;
    int port;
    int running;
    NL_HTTP_THREAD thread;
    nl_http_handler handler;
    void* user_data;
    struct nl_http2_connection* connections;
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

typedef struct nl_http2_connection nl_http2_connection_t;
typedef struct nl_http3_connection nl_http3_connection_t;

/* H1 服务端 / 客户端：字段与 TLS 构建相关，由各平台 .c 自行定义。 */

/* ============================================================
 * I/O 函数表（平台实现，公共逻辑调用）
 * ============================================================ */
typedef struct nl_http_io nl_http_io_t;

struct nl_http_io {
    /* 发送一个已按 9 字节帧头 + payload 组好的 h2 帧（TCP 连接）。
     * 成功返回写入字节数（即 data 的总长度），失败返回 -1。 */
    int (*write_h2_frame)(struct nl_http_io* io, nl_http_handle_t handle,
                          const uint8_t* data, size_t len);
    /* 发送一个 UDP（HTTP3/QUIC）报文给指定客户端地址。
     * 成功返回 0，失败返回 -1。client_addr/len 由平台填充。 */
    int (*udp_send)(struct nl_http_io* io, nl_http_handle_t handle,
                    const uint8_t* data, size_t len,
                    void* client_addr, size_t client_addr_len);
};

/* ============================================================
 * 公共静态数据 / 连接初始化（common.c 持有数据与默认初始化逻辑）
 * ============================================================ */

/* H2 连接预流（"PRI * HTTP/2.0..." + 空行 + "SM"，共 24 字节），供平台线程比较预流。 */
extern const char* h2_preface;
#define H2_PREFACE_LEN 24

/* 连接初始化（common 部分）：memset + 默认 settings + hpack_init
 * + client_addr 拷贝 + 连接链表挂载。锁初始化由平台 .c 在调用后用
 * NL_HTTP_MUTEX_INIT(m) 钩子补齐（common 保持平台无关）。 */
void h2_connection_init(struct nl_http2_connection* conn, nl_http_handle_t fd);
void h3_connection_init(struct nl_http3_connection* conn, nl_http_handle_t fd,
                        void* client_addr, size_t client_addr_len,
                        struct nl_http3_server* server);
void h3_connection_free(struct nl_http3_connection* conn);

/* ============================================================
 * 纯逻辑函数原型（实现在 netleaf_http_common.c，各平台共享）
 * ============================================================ */

/* HTTP/1 解析与生成 */
long nl_http_find_terminator(const char* buf, size_t total);
nlh_http_method_t nl_http_parse_method(const char* method);
int nl_http_parse_request(nl_http_request_t* req, const char* data, size_t len);
void nl_http_generate_response_http1(nl_http_response_t* resp, char** out, size_t* out_len);

/* HPACK */
int hpack_read_varint(const uint8_t* data, size_t len, uint8_t prefix_bits, uint64_t* value, size_t* consumed);
int hpack_write_varint(uint8_t* data, size_t len, uint8_t prefix_bits, uint64_t value, uint8_t prefix);
void hpack_init(struct hpack_context* ctx);
void hpack_free(struct hpack_context* ctx);
int hpack_find_static(const char* name, const char* value);
int hpack_decode_header(struct hpack_context* ctx, const uint8_t* data, size_t len, char* name, char* value, size_t* consumed);
int hpack_encode_header(struct hpack_context* ctx, uint8_t* data, size_t len, const char* name, const char* value);

/* HTTP/2 帧头与发送（通过 io 表写帧） */
void h2_frame_header_write(uint8_t* data, const struct nl_h2_frame_header* header);
void h2_frame_header_read(const uint8_t* data, struct nl_h2_frame_header* header);
int h2_send_settings_ack(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn);
int h2_send_settings(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn);
int h2_send_window_update(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, uint32_t stream_id, uint32_t increment);
int h2_send_headers(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, struct nl_http2_stream* stream, nl_http_response_t* resp);
struct nl_http2_stream* h2_find_or_create_stream(struct nl_http2_connection* conn, uint32_t stream_id);
int h2_apply_header_block(struct nl_http2_connection* conn, struct nl_http2_stream* stream, const uint8_t* block, size_t len);
int h2_process_frame(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, const uint8_t* data, size_t len, nl_http_handler handler, void* user_data);

/* QUIC varint + HTTP/3 流/帧/包处理（通过 io 表发 UDP） */
size_t quic_read_varint(const uint8_t* data, size_t len, uint64_t* value);
size_t quic_write_varint(uint8_t* data, size_t len, uint64_t value);
struct nl_http3_stream* h3_find_or_create_stream(struct nl_http3_connection* conn, uint64_t stream_id);
void h3_stream_free(struct nl_http3_stream* stream);
int h3_process_stream_frame(struct nl_http_io* io, struct nl_http3_connection* conn, struct nl_http3_stream* stream,
                           const uint8_t* data, size_t len, nl_http_handler handler, void* user_data);
int h3_process_quic_packet(struct nl_http_io* io, struct nl_http3_server* server, struct nl_http3_connection* conn,
                           const uint8_t* data, size_t len, nl_http_handler handler, void* user_data);

/* 平台钩子：RNG 生成连接 ID（各平台 RNG 不同，实现在平台 .c）。 */
void quic_generate_conn_id(nl_quic_conn_id_t* conn_id, uint8_t len);

/* ============================================================
 * QUIC 可插拔加密（AEAD）
 *
 * QUIC 真实加密（TLS 1.3 密钥派生 + AEAD）由 HTTPS/TLS 扩展（tls3）
 * 承载；纯 HTTP 核心库不链接 mbedTLS，故把"对初始/1-RTT 报文的
 * 认证加密"抽象为可插拔函数表：
 *   - nl_http_quic_crypto_install / nl_http_quic_crypto_default
 *     注入或回退到内置零密钥实现（Initial 演示态）。
 *   - 平台 .c 负责密钥协商（TLS 1.3 HKDF 或 mbedTLS AEAD），
 *     把密钥/nonce 派生结果填入 nl_quic_aead_ctx_t 后调用 install。
 *   - 报文收/发路径先调用 crypto->seal / crypto->open 做认证加密，
 *     密钥未就绪（crypto == NULL）时退化为零密钥演示态，保持核心库
 *     在无 TLS 环境下可编译、可运行（不做强假设）。
 * ============================================================ */
#define NL_QUIC_AEAD_TAG_LEN 16   /* Poly1305 / GCM 认证标签长度 */

typedef struct {
    /* 当前协商出的密钥（如 ECDSA 派生密钥），未协商时全 0。 */
    uint8_t key[32];
    uint8_t key_len;
    /* 每报文独立 nonce 前缀；实际 nonce = 前缀 || 包号。 */
    uint8_t nonce[12];
    uint8_t nonce_len;
    /* 1 = 真实密钥就绪（seal/open 走 AEAD），0 = 零密钥演示态。 */
    int real_key;
} nl_quic_aead_ctx_t;

typedef struct nl_quic_crypto nl_quic_crypto_t;

struct nl_quic_crypto {
    /* 密钥句柄（mbedTLS 句柄 / 平台 opaque 指针 / NULL）。 */
    void* opaque;
    /* 协商出的密钥/nonce 上下文；seal/open 据此判断是否走真实 AEAD。 */
    nl_quic_aead_ctx_t aead;
    /*
     * 认证加密（seal）：in[in_len) + 包号 pkt_num + 关联数据 aad[aad_len)
     * 写入 out（容量 >= in_len + NL_QUIC_AEAD_TAG_LEN）。
     * 成功返回写入字节数（含 16 字节认证标签），失败返回 -1。
     * 无密钥（aead.real_key==0）时允许原地拷贝（演示态）。
     */
    int (*seal)(struct nl_quic_crypto* crypto, nl_quic_aead_ctx_t* ctx,
                const uint8_t* in, size_t in_len,
                uint64_t pkt_num,
                const uint8_t* aad, size_t aad_len,
                uint8_t* out);
    /*
     * 认证解密（open）：ciphertext[in_len) 含尾部 16 字节认证标签，
     * 明文写入 out（容量 >= in_len - 16）。成功返回明文长度，
     * 失败（标签校验失败）返回 -1。
     */
    int (*open)(struct nl_quic_crypto* crypto, nl_quic_aead_ctx_t* ctx,
                const uint8_t* in, size_t in_len,
                uint64_t pkt_num,
                const uint8_t* aad, size_t aad_len,
                uint8_t* out);
    /* 释放 opaque 资源（平台 mbedTLS 句柄等）。 */
    void (*free_ctx)(struct nl_quic_crypto* crypto);
};

/* 当前生效的 crypto（NULL = 未注入，走零密钥演示态）。 */
extern struct nl_quic_crypto* nl_quic_crypto_active;

/* 注入自定义 crypto（如 tls3 扩展持有 mbedTLS 句柄的实例）。传 NULL 卸载。 */
void nl_http_quic_crypto_install(struct nl_quic_crypto* crypto);

/* 内置零密钥 crypto：未协商时核心库自用的演示态实现（seal/open 直接拷贝）。 */
nl_quic_crypto_t* nl_http_quic_crypto_default(void);

/* ============================================================
 * 帧编解码回环（端对端联调）
 *
 * 供单元测试 / 联调使用：把 H2 帧写进缓冲再读回，校验头字段
 * 一一对应；QUIC varint 写再读校验。返回 1 = 回环通过，0 = 失败。
 * 不依赖平台 socket，纯内存操作。
 * ============================================================ */
int h2_frame_roundtrip(uint8_t* data, size_t data_cap,
                       const struct nl_h2_frame_header* header,
                       const uint8_t* payload, size_t payload_len);
int quic_varint_roundtrip(const uint64_t* values, size_t count,
                          uint8_t* out, size_t out_cap, size_t* out_used);

/* 请求/响应 访问器 */
nlh_http_method_t nl_http_request_get_method(const nl_http_request_t* req);
nl_http_version_t nl_http_request_get_version(const nl_http_request_t* req);
const char* nl_http_request_get_path(const nl_http_request_t* req);
const char* nl_http_request_get_header(const nl_http_request_t* req, const char* name);
const char* nl_http_request_get_body(const nl_http_request_t* req);
size_t nl_http_request_get_body_size(const nl_http_request_t* req);
void nl_http_response_set_status(nl_http_response_t* resp, int status);
void nl_http_response_set_header(nl_http_response_t* resp, const char* name, const char* value);
void nl_http_response_set_body(nl_http_response_t* resp, const char* body, size_t len);

#endif /* NETLEAF_HTTP_INTERNAL_H */
