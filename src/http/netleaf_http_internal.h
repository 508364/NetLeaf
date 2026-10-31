/*
 * NetLeaf HTTP/2 内部公共头（平台无关部分）。
 *
 * 说明：
 *  - 原先 linux/macos/windows 三份 netleaf_http_*.c 重复了 ~57 个函数。
 *    其中 ~40 个是纯逻辑（parse/hpack/h2 帧/accessor），
 *    已抽到 src/http/netleaf_http_common.c，只保留一份。
 *  - 本头声明被 common.c 与各平台 .c 共享的常量、结构体、枚举、纯逻辑
 *    函数原型，以及一个 I/O 函数表（nl_http_io_t）。
 *  - 平台相关代码（socket/线程/锁/RNG/生命周期/HTTPS）留在各平台 .c，
 *    通过 nl_http_io_t 把"收发一个 h2 帧、互斥锁"注入公共逻辑，
 *    使公共层不直接依赖任一平台的 <sys/socket.h>/<pthread.h>/<winsock2.h>。
 *  - h2 帧头统一为"普通 uint32 length + 31-bit stream_id"（无位域），
 *    消除原先 windows 用 plain uint32、posix 用位域的不一致。
 *
 * 注意：本头直接定义连接/服务器结构体（nl_http2_connection /
 * nl_http2_server），其句柄/线程/锁字段随平台宏（NL_HTTPS_SOCK /
 * NL_HTTP_THREAD / NL_HTTP_MUTEX）变化；平台 .c 在 include 本头之前
 * 先 #define 这些宏。nl_http_server / nl_http_client（H1，依赖 TLS
 * 构建配置）仍由各平台 .c 定义。
 */
#ifndef NETLEAF_HTTP_INTERNAL_H
#define NETLEAF_HTTP_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

#include "netleaf_http.h"

/* 跨 DLL 导出控制：netleaf_https 扩展需复用 netleaf_core 编译的 H2/H1
 * 公共实现（netleaf_http_common.c）。
 *  - netleaf_https 扩展构建（NL_HTTPS_ENABLE）→ dllimport（符号由 core 提供）；
 *  - core 构建（NL_EXPORTS，含 core 与本头各使用方）→ dllexport；
 *  - 测试等直接包含 netleaf_http_common.c 的构建 → 无修饰（本地定义）。 */
#if defined(_WIN32)
    #if defined(NL_HTTPS_ENABLE)
        #define NL_HTTP_INT_API __declspec(dllimport)
    #elif defined(NL_EXPORTS)
        #define NL_HTTP_INT_API __declspec(dllexport)
    #else
        #define NL_HTTP_INT_API
    #endif
#else
    #define NL_HTTP_INT_API
#endif

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

/* 流状态机（RFC 7540 §5.1，服务端视角精简：IDLE → OPEN → HALF_CLOSED_* → CLOSED）。
 * 前缀用 NL_H2_ST_ 以区别公共头 netleaf_http.h 的 NL_H2_STREAM_CLOSED 错误码。 */
enum nl_h2_stream_state {
    NL_H2_ST_IDLE = 0,
    NL_H2_ST_OPEN = 1,
    NL_H2_ST_HALF_CLOSED_LOCAL = 2,   /* 已发 END_STREAM，未收完对端 */
    NL_H2_ST_HALF_CLOSED_REMOTE = 3,  /* 已收 END_STREAM，响应未发完 */
    NL_H2_ST_CLOSED = 4
};

struct nl_http2_stream {
    uint32_t id;
    int state;
    int window_size;
    nl_http_request_t request;
    nl_http_response_t response;
    struct nl_http2_stream* next;
};

/* QUIC / HTTP3 状态与参数已随 QUIC/HTTP3 代码一并迁出主库（见 src/quic 扩展）。 */

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
    int dynamic_table_size;        /* 当前占用（RFC 7541 §4.1 计费：32+name+value） */
    int max_dynamic_table_size;    /* 容量上限（字节），由 HEADER_TABLE_SIZE 约束 */
    int dynamic_table_count;       /* 当前条目数（[0] 为最近使用 → HPACK 索引 62） */
    int dynamic_table_cap;         /* 已分配条目容量（避免逐条 realloc） */
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
    struct hpack_context hpack;      /* 解码对端头块的解码表 */
    struct hpack_context hpack_enc;  /* 编码本端响应头块的编码表（方向独立，RFC 7541 §2.3.2） */
    struct nl_h2_settings local_settings;
    struct nl_h2_settings remote_settings;
    int connection_window_size;
    int preface_sent;
    int settings_ack_received;
    int draining;                    /* 收到 GOAWAY 后置 1（对端进入 draining） */
    uint32_t goaway_last_stream;     /* 对端 GOAWAY 声明的最高可处理流 ID */
    struct nl_http2_stream* streams;
    NL_HTTP_MUTEX mutex;
    /* CONTINUATION：跨帧累积未完成的 HEADERS 头块（END_HEADERS 未置位时续帧） */
    uint8_t* pending_headers_block;
    size_t pending_headers_len;
    uint32_t pending_headers_stream;
};

/* H2 服务端：平台句柄 + 线程/处理器；connections 为 H2 连接链表。 */
struct nl_http2_server {
    nl_http_handle_t fd;
    int port;
    int running;
    NL_HTTP_THREAD thread;
    nl_http_handler handler;
    void* user_data;
    struct nl_http2_connection* connections;
};

typedef struct nl_http2_connection nl_http2_connection_t;

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
    /* 实现私有数据（如 TLS 上下文）。公共逻辑不读写此字段，
     * 仅由 io 实现方在回调内取用（见 netleaf_http_tls_*.c 的 TLS 版 io）。 */
    void* user;
};

/* ============================================================
 * 公共静态数据 / 连接初始化（common.c 持有数据与默认初始化逻辑）
 * ============================================================ */

/* H2 连接预流（"PRI * HTTP/2.0..." + 空行 + "SM"，共 24 字节），供平台线程比较预流。 */
extern NL_HTTP_INT_API const char* h2_preface;
#define H2_PREFACE_LEN 24

/* 连接初始化（common 部分）：memset + 默认 settings + hpack_init。
 * 锁初始化由平台 .c 在调用后用 NL_HTTP_MUTEX_INIT(m) 钩子补齐
 * （common 保持平台无关）。 */
NL_HTTP_INT_API void h2_connection_init(struct nl_http2_connection* conn, nl_http_handle_t fd);

/* 连接清理（common 部分）：释放 hpack 双表 / CONTINUATION 累积块 / 流链表。
 * 平台 .c 在线程退出（锁销毁）前调用，避免动态表内存泄漏。 */
NL_HTTP_INT_API void h2_connection_cleanup(struct nl_http2_connection* conn);

/* ============================================================
 * 纯逻辑函数原型（实现在 netleaf_http_common.c，各平台共享）
 * ============================================================ */

/* HTTP/1 解析与生成 */
NL_HTTP_INT_API long nl_http_find_terminator(const char* buf, size_t total);
nlh_http_method_t nl_http_parse_method(const char* method);
NL_HTTP_INT_API int nl_http_parse_request(nl_http_request_t* req, const char* data, size_t len);
NL_HTTP_INT_API void nl_http_generate_response_http1(nl_http_response_t* resp, char** out, size_t* out_len);

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
NL_HTTP_INT_API void h2_frame_header_read(const uint8_t* data, struct nl_h2_frame_header* header);
int h2_send_settings_ack(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn);
NL_HTTP_INT_API int h2_send_settings(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn);
int h2_send_window_update(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, uint32_t stream_id, uint32_t increment);
int h2_send_rst_stream(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, uint32_t stream_id, uint32_t error_code);
int h2_send_goaway(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, uint32_t last_stream_id, uint32_t error_code);
int h2_send_headers(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, struct nl_http2_stream* stream, nl_http_response_t* resp);
struct nl_http2_stream* h2_find_or_create_stream(struct nl_http2_connection* conn, uint32_t stream_id);
int h2_apply_header_block(struct nl_http2_connection* conn, struct nl_http2_stream* stream, const uint8_t* block, size_t len);
NL_HTTP_INT_API int h2_process_frame(struct nl_http_io* io, nl_http_handle_t fd, struct nl_http2_connection* conn, const uint8_t* data, size_t len, nl_http_handler handler, void* user_data);

/* ============================================================
 * 帧编解码回环（端对端联调）
 *
 * 供单元测试 / 联调使用：把 H2 帧写进缓冲再读回，校验头字段
 * 一一对应。返回 1 = 回环通过，0 = 失败。
 * 不依赖平台 socket，纯内存操作。
 * ============================================================ */
int h2_frame_roundtrip(uint8_t* data, size_t data_cap,
                       const struct nl_h2_frame_header* header,
                       const uint8_t* payload, size_t payload_len);

/* 注意：请求/响应访问器（nl_http_request_get_method / get_version /
 * get_path / get_header / get_body / get_body_size、
 * nl_http_response_set_status / set_header / set_body）均已在公共头
 * netleaf_http.h 用 NL_API 声明（跨 DLL 可见），此处不得再重复声明：
 * test_http_frames 等直接包含 netleaf_http_common.c 的构建未定义
 * NL_EXPORTS，公共头会把它们标成 dllimport，此处普通重声明会被
 * MinGW 报 "redeclared without dllimport attribute" 告警。 */

#endif /* NETLEAF_HTTP_INTERNAL_H */
