/*
 * nl_px_local.h - 进程内路由分发（平台无关，一份实现）
 *
 * 背景：HTTP/2（h2c）桥接此前把每条 stream 用 HTTP/1.1 回放到本机监听端口，
 * 再由平台 web 服务器执行路由，多了一次本机 socket 往返。本模块让公共层
 * 直接查路由并生成完整响应：
 *   - CONTENT / FILE / REDIRECT / HTTP 反代：在桥接工作线程内直接完成；
 *   - HTTPS / TCP 上游：返回 NL_PX_LOCAL_FALLBACK，由调用方回退本机回放
 *     （保留平台既有 TLS / 上游 h2 能力）。
 *
 * 路由查找与上游连接由平台层通过回调提供，公共层不持有平台路由结构。
 *
 * 另外提供 IPv6 地址文本工具（三平台共用）：
 *   nl_px_sockaddr_ip / nl_px_split_hostport / nl_px_format_hostport
 */
#ifndef NL_PX_LOCAL_H
#define NL_PX_LOCAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 前置声明：避免在函数原型里首次出现时被当作原型作用域的新类型 */
struct sockaddr;

/* 路由类型 */
#define NL_PX_LOCAL_CONTENT   0
#define NL_PX_LOCAL_FILE      1
#define NL_PX_LOCAL_REDIRECT  2
#define NL_PX_LOCAL_PROXY     3

/* 上游协议（仅 PROXY 有效） */
#define NL_PX_LOCAL_PROTO_HTTP   0
#define NL_PX_LOCAL_PROTO_HTTPS  1
#define NL_PX_LOCAL_PROTO_TCP    2

/* 本地可读文件上限（与平台既有实现一致） */
#define NL_PX_LOCAL_FILE_MAX (10u << 20)

/* dispatch 返回值 */
#define NL_PX_LOCAL_FALLBACK (-2)
#define NL_PX_LOCAL_ERROR    (-1)
#define NL_PX_LOCAL_OK        0

typedef struct nl_px_local_route {
    int type;                  /* NL_PX_LOCAL_* */
    int protocol;              /* NL_PX_LOCAL_PROTO_*（PROXY） */
    int redirect_code;         /* REDIRECT：301/302（<=0 时按 302） */
    const char* content;       /* CONTENT：内容（不要求 NUL 结尾，见 content_len） */
    size_t content_len;        /* CONTENT：0 表示用 strlen(content) */
    const char* content_type;  /* CONTENT/FILE：MIME，空则 application/octet-stream */
    const char* file_path;     /* FILE：磁盘路径 */
    const char* redirect_url;  /* REDIRECT：Location */
    const char* upstream;      /* PROXY：host:port（IPv6 可写 [::1]:8080） */
    /* 第195轮：目录转发（nginx proxy_pass URI 拼接）。dir_prefix 非空=前缀路由；
     * up_uri 非空时按 nl_px_dir_rewrite 改写转发路径，空则原样透传。 */
    const char* dir_prefix;    /* PROXY：目录路由前缀（精确路由为 NULL） */
    const char* up_uri;        /* PROXY：上游 URI 部分（如 "/v1/"，空=不改写） */
} nl_px_local_route;

typedef struct nl_px_local_ops {
    /* 查路由：1 = 命中（out 已填充），0 = 未命中 */
    int (*lookup)(void* ctx, const char* path, nl_px_local_route* out);
    /* 连接上游：成功返回已连接 fd，失败返回 < 0（仅 HTTP 反代使用） */
    intptr_t (*connect_upstream)(void* ctx, const nl_px_local_route* route);
} nl_px_local_ops;

/* ============================================================
 * HTTP/1.1 本地路由计划（统一三平台本地响应头/状态构造）
 *
 * 平台引擎仍各自保留最优发送方式（Windows TransmitFile / Linux sendfile
 * 状态机 / macOS sendfile），只把"查路由 + 状态 + 响应头"收敛到公共层。
 * ============================================================ */
typedef struct nl_px_local_plan {
    int status;              /* 200 / 301 / 302 / 404 / 500 */
    int type;                /* NL_PX_LOCAL_CONTENT / FILE / REDIRECT */
    char head[1024];         /* 完整 HTTP/1.1 响应头（含 \r\n\r\n，Connection: close） */
    size_t head_len;
    const char* body;        /* CONTENT：路由内容（借用，不拥有） */
    size_t body_len;
    const char* file_path;   /* FILE：磁盘路径 */
    long long file_size;     /* FILE：字节数 */
} nl_px_local_plan;

/*
 * 构造本地路由响应计划（HTTP/1.1）。
 * 返回 1 = 已生成（out 有效）；0 = 未命中或 PROXY（平台走原 404 / 反代路径）；
 *      -1 = 参数错误。
 * 文件不存在回 404、超过 10MB 回 500，均以 head 表达（body 为空）。
 */
int nl_px_local_plan_build(void* ctx, const nl_px_local_ops* ops,
                           const char* path, int is_head, nl_px_local_plan* out);

/* ============================================================
 * 监听地址族选择（三平台共用）
 *   NL_PX_LISTEN_DUAL = 双栈（:: + V6ONLY=0，默认）
 *   NL_PX_LISTEN_V4   = 仅 IPv4
 *   NL_PX_LISTEN_V6   = 仅 IPv6（V6ONLY=1）
 * resolve(configured)：configured >= 0 时直接采用；否则读环境变量
 *   NETLEAF_LISTEN_MODE=v4|v6|dual（不区分大小写），
 *   兼容旧开关 NETLEAF_LISTEN_IPV6=0（等价 v4）。
 * ============================================================ */
#define NL_PX_LISTEN_DUAL 0
#define NL_PX_LISTEN_V4   1
#define NL_PX_LISTEN_V6   2
int nl_px_listen_mode_resolve(int configured);

/*
 * 进程内分发一次请求。
 *   method/path     : 请求方法与路径（path 保留 query）
 *   authority       : :authority（可为空）
 *   req_hdrs/len    : 客户端请求头原始行（"name: value\r\n"，不含请求行）
 *   req_body/len    : 首个 DATA 帧已到的 body（可为空）
 *   peer_ip         : 客户端 IP 文本（可空，用于 X-Forwarded-For）
 *   is_head         : HEAD 请求（只回头，不回 body）
 *   idle_ms         : 上游读超时（<=0 用默认）
 * 成功返回 NL_PX_LOCAL_OK，out_status/out_hdrs/out_body 由本函数 malloc，
 * 调用方负责 free（out_hdrs 可为 NULL、长度 0）。返回 NL_PX_LOCAL_FALLBACK
 * 表示调用方应回退本机回放；NL_PX_LOCAL_ERROR 为参数/内存错误。
 */
int nl_px_local_dispatch(void* ctx, const nl_px_local_ops* ops,
                         const char* method, const char* path, const char* authority,
                         const char* req_hdrs, size_t req_hdr_len,
                         const char* req_body, size_t req_body_len,
                         const char* peer_ip, int is_head, int idle_ms,
                         int* out_status, char** out_hdrs, size_t* out_hlen,
                         char** out_body, size_t* out_blen);

/* ============================================================
 * IPv6 / 地址文本工具（三平台共用）
 * ============================================================ */

/*
 * sockaddr（INET/INET6）转 IP 文本；IPv4 映射地址（::ffff:a.b.c.d）还原点分。
 * 成功返回 1，失败 0。out 建议 >= 64 字节。
 */
int nl_px_sockaddr_ip(const struct sockaddr* sa, char* out, size_t cap);

/*
 * "host:port" / "[::1]:8080" / "host" / "::1" 拆分。
 * 方括号内的 IPv6 不会按 ':' 截断；port<=0 时用 def_port。
 * 返回 1 成功，0 参数错误。
 */
int nl_px_split_hostport(const char* in, char* host, size_t host_cap,
                         int* port, int def_port);

/*
 * host + port 还原为 "host:port"；host 含 ':' 且未带方括号时自动加 []。
 * 返回写入长度（不含 NUL），<0 表示容量不足。
 */
int nl_px_format_hostport(const char* host, int port, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* NL_PX_LOCAL_H */
