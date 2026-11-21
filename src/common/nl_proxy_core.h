/*
 * nl_proxy_core.h - 反向代理会话层（平台无关纯逻辑）
 *
 * 背景：反代引擎在 Windows(IOCP) / Linux(epoll) / macOS(kqueue) 三处各写了一份
 * 相同逻辑，同一个缺陷要修三次，且两端无法在本机编译验证。本模块把与 IO 模型
 * 无关的部分集中到此：
 *   1) 上游请求重组（首行改写 + 客户端头透传 + XFF + 空行终止符）
 *   2) 上游响应 framing 状态机（Content-Length / chunked / 无 body / 101 隧道）
 *   3) 路由路径匹配（剥离 query）
 *   4) https 扩展必需能力契约（反代 HTTPS 上游）
 *
 * 平台层只保留：socket、事件注册、收发、连接池、槽位生命周期。
 * 纯逻辑不持有 fd、不阻塞、不分配内存（行缓冲由调用方提供）。
 */
#ifndef NL_PROXY_CORE_H
#define NL_PROXY_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Windows 共享库导出（与 nl_uri.h 同机制）：核心库以 NL_EXPORTS 编译，
 * 直接消费本头的测试/扩展按 dllimport 引用。第195轮新增的公共 API
 * （目录转发）需要被 test_web_engine 直接链接，故统一补上导出标记。 */
#ifndef NL_API
    #ifdef _WIN32
        #ifdef NL_EXPORTS
            #define NL_API __declspec(dllexport)
        #else
            #define NL_API __declspec(dllimport)
        #endif
    #else
        #define NL_API
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 上游响应 framing 状态 ---------------- */
#define NL_PX_UP_HDR     0  /* 响应头未收全 */
#define NL_PX_UP_CL      1  /* 按 Content-Length 计 body */
#define NL_PX_UP_CHUNK   2  /* Transfer-Encoding: chunked */
#define NL_PX_UP_DONE    3  /* 完成，连接可复用 */
#define NL_PX_UP_EOF     4  /* 不可复用（body 以 EOF 结束 / Connection: close） */
#define NL_PX_UP_TUNNEL  5  /* 101 已升级为透明隧道（WS / Upgrade），不可复用 */

/* ---------------- chunked 子状态 ---------------- */
#define NL_PX_CHUNK_SIZE    0
#define NL_PX_CHUNK_DATA    1
#define NL_PX_CHUNK_CRLF    2
#define NL_PX_CHUNK_TRAILER 3

/*
 * 解析上下文：平台层可直接用数组映射（逐字段拷贝进出），
 * 也可整体持有。linebuf 由调用方提供（建议 >= 8192，至少 64）。
 */
typedef struct nl_px_ctx {
    uint8_t  up_parse;      /* NL_PX_UP_* */
    uint8_t  chunk_state;   /* NL_PX_CHUNK_* */
    uint8_t  req_is_head;   /* 对应请求是 HEAD（响应无 body） */
    int64_t  up_cl;         /* 剩余 Content-Length */
    int64_t  chunk_rem;     /* 当前 chunk 剩余字节 */
    int      up_hlen;       /* linebuf 已用长度 */
    char*    linebuf;       /* 响应头 / chunk 行缓冲（非拥有） */
    size_t   linecap;       /* linebuf 容量 */
} nl_px_ctx;

/* 初始化上下文（linebuf 由调用方持有，需活到上下文销毁） */
void nl_px_ctx_init(nl_px_ctx* c, char* linebuf, size_t linecap);
/* 复位为“等待响应头”，保留 linebuf */
void nl_px_ctx_reset(nl_px_ctx* c);

/*
 * 上游请求重组。
 *   upstream_host  路由里配置的上游主机（用作 Host 头，保证虚拟主机路由正确）
 *   raw_hdrs/len   客户端首行之后的原始头（含空行之前的各行，可含 body 起始）
 *   peer_ip        真实客户端 IP；NULL/空则不追加 X-Forwarded-For
 * 行为：改写请求行与 Host、追加 XFF、逐行透传客户端头（过滤 Host/XFF，
 *       Connection 原值透传，未自带时补 keep-alive），最后补 CRLF 空行。
 * 返回写入字节数；<0 表示失败（容量不足/参数非法）。
 */
int nl_px_build_request(char* out, size_t out_cap,
                        const char* method, const char* path, const char* proto,
                        const char* upstream_host,
                        const char* raw_hdrs, size_t raw_hdrs_len,
                        const char* peer_ip);

/* TCP 上游：原始字节整包透传。返回字节数，0 表示放不下（保持“零转发”）。 */
int nl_px_passthrough(char* out, size_t out_cap, const char* raw, size_t raw_len);

/*
 * 旁路消费被转发给客户端的上游字节，推进 framing 状态。
 * 绝不修改 data（转发必须字节级原样）。
 * 返回推进后的状态（NL_PX_UP_*）。
 */
int nl_px_feed_response(nl_px_ctx* c, const char* data, size_t len);

/* 路由路径匹配：双方都剥离 query string（'?' 之后）后整段比较。 */
int nl_px_path_match(const char* route_path, const char* req_path);

/*
 * 第195轮：目录（前缀）路由匹配——与 nl_px_path_match 同样的规范化流程，
 * 比较放宽为"规范化后的 req_path 以 route_prefix 为前缀"（nginx location
 * 前缀语义）。空前缀返回 0。与 nl_px_path_match 配合实现"精确 > 最长前缀"。
 */
NL_API int nl_px_path_match_prefix(const char* route_prefix, const char* req_path);

/*
 * 第195轮：目录转发的上游请求路径改写（nginx proxy_pass URI 拼接语义）。
 *   match_path / is_prefix  命中路由的 path 与是否目录（前缀）路由
 *   up_uri                  上游 URI（如 "/v1/"）；NULL/空 → 路径原样透传
 *   req_path                客户端原始请求路径（含 query，字节原样）
 * up_uri 为空：直接透传（nginx `proxy_pass http://backend;`）；否则剥掉命中
 * 前缀（精确路由剥整段）拼接 up_uri 并原样挂回 ?query；原始 path 不以
 * match_path 字面开头时安全回退原样透传。返回写入字节数（不含 NUL），
 * <0 表示容量不足/参数非法。out 建议 >= 4096。
 */
NL_API int nl_px_dir_rewrite(const char* match_path, int is_prefix,
                             const char* up_uri, const char* req_path,
                             char* out, size_t cap);

/* 忽略大小写前缀比较（prefix 必须是全小写字面量）。 */
int nl_px_ci_starts(const char* s, size_t n, const char* prefix);

/* ---------------- HTTP 请求行解析 ----------------
 * 输入首行（不含结尾 LF；多传的 '\r' 会被去掉）。
 * 三平台反代引擎与旧单线程处理路径此前各自手写同一段空格切分，统一在此。
 * 成功返回 0，三个输出均 NUL 结尾（path 保留 query，由路由匹配自行剥离）；
 * 字段为空 / 超长 / 缺少空格返回 -1。不分配内存、不修改入参。
 */
int nl_px_parse_request_line(const char* line, size_t len,
                             char* method, size_t mcap,
                             char* path, size_t pcap,
                             char* proto, size_t vcap);

/* ---------------- 每上游连接的 framing 状态 ----------------
 * 平台层旧实现用 6 个并行数组承载同一份状态；统一为一个结构体数组，
 * 既减少分配与缓存行占用，也避免三端字段映射逻辑漂移。
 */
typedef struct nl_px_upstate {
    uint8_t up_parse;      /* NL_PX_UP_* */
    uint8_t chunk_state;   /* NL_PX_CHUNK_* */
    uint8_t req_is_head;   /* 对应请求是 HEAD */
    int64_t up_cl;         /* 剩余 Content-Length */
    int64_t chunk_rem;     /* 当前 chunk 剩余字节 */
    int     up_hlen;       /* linebuf 已用长度 */
} nl_px_upstate;

/* 复位为“等待响应头”（req_is_head 一并清零） */
void nl_px_upstate_reset(nl_px_upstate* st);

/*
 * 旁路消费被转发给客户端的上游字节，推进 framing 状态。
 * linebuf 由调用方按连接提供（建议 >= 8192）；绝不修改 data。
 * 返回推进后的状态（NL_PX_UP_*）；参数非法时原样返回当前状态。
 */
int nl_px_upstate_feed(nl_px_upstate* st, char* linebuf, size_t linecap,
                       const char* data, size_t len);

/* ---------------- 本地响应头构造（content/file/redirect） ----------------
 * 三平台此前各自内联同一组模板字符串；统一后只有一份字节序列。
 * 返回写入的头字节数；<0 表示容量不足或参数非法。
 */
int nl_px_build_redirect_head(char* out, size_t cap, int code, const char* location);
int nl_px_build_body_head(char* out, size_t cap, const char* content_type,
                          long long content_len, int chunked);

/* ==================================================================
 * https 扩展必需能力契约（方案 C）
 *
 * 反代到 HTTPS 上游要求 https 扩展提供“在既有 TCP 连接上建立 TLS 并持续
 * 收发”的能力。仅握手不够：握手后的所有读写都必须走 TLS 会话，因此契约
 * 以会话对象为准。
 *
 * 扩展必须导出（ext_id = "https"）：
 *   nlh_tls_conn_wrap(sock, host, &alpn) -> void*   必需
 *   nlh_tls_conn_read(conn, buf, len)   -> int      必需，>0 字节数，0 暂无数据，<0 错误/EOF
 *   nlh_tls_conn_write(conn, buf, len)  -> int      必需，>0 已写，0 暂不可写，<0 错误
 *   nlh_tls_conn_free(conn)             -> void     必需
 * 会话为非阻塞语义（wrap 成功即把底层 socket 置为非阻塞），由调用方事件循环驱动。
 * 旧版单向钩子（可选，仅握手、不可用于后续收发）：
 *   nl_tls_handshake(sock, host) -> int             可选
 *
 * 契约不满足时，反代必须让该连接失败（502），禁止明文回退。
 * ================================================================== */
#define NL_PX_TLS_CONTRACT_VERSION 1

typedef struct nl_px_tls_api {
    int version;                                                    /* >= 1 */
    void* (*conn_wrap)(int sock, const char* host, const char** out_alpn);
    int   (*conn_read)(void* conn, void* buf, int len);
    int   (*conn_write)(void* conn, const void* buf, int len);
    void  (*conn_free)(void* conn);
    int   (*handshake)(int sock, const char* host);                  /* 可选 */
    int   client_impl;              /* 1 = 平台已实现 TLS 流式收发 */
} nl_px_tls_api;

/*
 * 探测 https 扩展契约。返回 1 = 满足（api 已填充），0 = 不满足。
 * 不满足时 nl_px_tls_contract_missing() 给出缺失项说明（静态文本）。
 */
int nl_px_tls_contract_get(nl_px_tls_api* api);
const char* nl_px_tls_contract_missing(void);

/* ==================================================================
 * HTTPS 上游字节隧道（平台无关，一份实现）
 *
 * 在专属线程里把客户端明文与 TLS 上游会话双向对灌：
 *   1) 先通过 api->conn_write 发送 initial（已构造好的上游请求头 + 首包 body）
 *   2) select 双 fd；上游 conn_read 读净后阻塞写客户端；客户端数据走 conn_write
 *   3) 任一侧 EOF/错误/空闲超时结束
 *
 * 所有权：函数返回前关闭 client_fd / upstream_fd，并 api->conn_free(tls_conn)。
 * 不触碰引擎槽位（槽位回收由平台层负责）。返回 0 正常结束，-1 异常结束。
 * ================================================================== */
int nl_px_tls_tunnel_run(intptr_t client_fd, intptr_t upstream_fd,
                         void* tls_conn, const nl_px_tls_api* api,
                         const char* initial, int initial_len,
                         int idle_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NL_PROXY_CORE_H */
