/*
 * nl_px_h2.h - HTTP/2 反代桥接（平台无关，一份实现）
 *
 * 两个方向：
 *   1) 下游 h2c（明文 H2）：客户端把整条连接交给公共层专属线程，公共层
 *      终止 H2、按 stream 并行回放到本机 web 服务器（HTTP/1.1 + Connection:
 *      close），再把响应封装成 HEADERS/DATA。这样静态/重定向/反代全部复用
 *      既有路由逻辑，平台层不需要重复实现任何路由或上游逻辑。
 *   2) 上游 H2（TLS ALPN 协商出 h2）：把平台已构造好的 HTTP/1.1 请求改写成
 *      H2 帧，读回响应后再还原成 HTTP/1.1 写回客户端。
 *
 * 所有权约定：bridge_run / upstream_run 成功接管 fd 后，返回前关闭 fd
 * （upstream_run 还负责 api->conn_free(tls_conn)）。平台层只负责移交与回收槽位。
 *
 * 线程模型：bridge_run 自身阻塞运行（由平台起线程）；每个 stream 再起一个
 * 工作线程并行回放，互不阻塞。热路径缓冲固定/懒分配，无逐请求全局锁。
 */
#ifndef NL_PX_H2_H
#define NL_PX_H2_H

#include <stddef.h>
#include <stdint.h>

#include "nl_proxy_core.h"
#include "nl_px_local.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NL_PX_H2_PREFACE_LEN 24

/*
 * 首包判定：是否为 H2 连接预流（"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"）。
 * 返回 2 = 24 字节完整匹配；1 = 不足以判定但仍是其前缀（调用方应继续收）；
 *      0 = 不是 H2（按普通 HTTP/1.1 处理）。
 */
int nl_px_h2_preface_check(const char* data, size_t len);

/*
 * 在 HTTP/1.1 请求头块（首行之后、空行之前）中查找 "Upgrade: h2c"。
 * 返回 1 = 是 h2c 升级请求，0 = 否。
 */
int nl_px_h2_upgrade_check(const char* headers, size_t len);

/*
 * 下游 h2c 会话主循环（阻塞，独占 client_fd）。
 *   local_port   本机 web 服务端口（进程内分发不可用时回放目标）
 *   initial      平台已读到的首包字节（非升级：含 preface；升级：完整请求头）
 *   is_upgrade   1 = 由 HTTP/1.1 Upgrade: h2c 升级而来（initial 为请求头）
 *   idle_ms      空闲超时（毫秒），<=0 用默认值
 *   local_ctx    平台上下文（传给 local_ops 回调）
 *   local_ops    进程内路由分发回调；NULL 时全部走本机回放
 * 每条 stream 先尝试 local_ops 进程内分发（CONTENT/FILE/REDIRECT/HTTP 反代），
 * 仅在返回 NL_PX_LOCAL_FALLBACK 时才回放到本机监听端口。
 * 返回 0 = 正常结束，-1 = 参数/内存错误。
 */
int nl_px_h2_bridge_run(intptr_t client_fd, int local_port,
                        const char* initial, size_t initial_len,
                        int is_upgrade, int idle_timeout_ms,
                        void* local_ctx, const nl_px_local_ops* local_ops);

/*
 * 上游 H2（ALPN=h2）交换：initial 为平台构造好的 HTTP/1.1 请求（头 + 可含
 * 首包 body），函数完成 H2 请求/响应交换后，把响应还原成 HTTP/1.1 写回
 * client_fd，最后关闭双方 fd 并 api->conn_free(tls_conn)。
 * 返回 0 = 正常，-1 = 失败（已尽力回 502）。
 */
int nl_px_h2_upstream_run(intptr_t client_fd, intptr_t upstream_fd,
                          void* tls_conn, const nl_px_tls_api* api,
                          const char* initial, int initial_len,
                          int idle_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NL_PX_H2_H */
