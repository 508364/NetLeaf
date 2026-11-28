/*
 * nl_ws_tls.h - 第205轮 WSS：WebSocket over TLS 的运行时后端（平台无关）
 *
 * 设计说明（为什么是"运行时加载"而不是链接）：
 *   netleaf_core 不直接链接 TLS 扩展库——netleaf_tls3 / netleaf_tls2 均反向
 *   链接 netleaf_core（见 src/tls3/CMakeLists.txt 的 target_link_libraries），
 *   若核心库再链接它们会形成循环依赖；Windows 下更会因"导入库依赖对方 DLL
 *   先产出"造成构建顺序死锁。故在运行时通过 dlopen / LoadLibrary 按需加载
 *   后端库并解析符号：tls3 优先，回退 tls2（与 netleaf_https 的
 *   BUILD_TLS3 优先策略一致）。
 *
 * 证书 / 私钥路径一律由调用方传入（PEM 文件路径），本模块不硬编码任何
 * 密钥材料。未启用 TLS（conn == NULL）时 IO 走明文 socket，行为与明文
 * 完全一致。
 */
#ifndef NL_WS_TLS_H
#define NL_WS_TLS_H

#include <stdint.h>

/* 每连接一个 TLS 上下文（不透明句柄） */
typedef struct nl_ws_tls_conn nl_ws_tls_conn_t;

/* TLS 服务端握手超时（毫秒），与 nlh_tls_server_* 的 10s 口径一致 */
#define NL_WS_TLS_HS_TIMEOUT_MS 10000

/*
 * 进程内一次性加载 TLS 后端（幂等，线程安全）。
 * 返回 0=可用，-1=不可用（后端库缺失 / 符号缺失）。
 * 需在 nl_ws_server_enable_tls() 内部调用，通常无需直接使用。
 */
int nl_ws_tls_load(void);

/*
 * 每连接一次：创建 TLS 服务端 ctx → configure(is_server=1, cert/key) →
 * 阻塞式 TLS 握手（与现有 ws 客户端线程模型一致）。
 * 成功返回连接句柄；失败返回 NULL（不关闭 sock，由调用方关闭）。
 * 前置条件：nl_ws_tls_load() 已返回 0。
 */
nl_ws_tls_conn_t* nl_ws_tls_accept(uintptr_t sock,
                                   const char* cert_file,
                                   const char* key_file);

/* 释放 TLS ctx 与句柄（不关闭 socket）。NULL 安全。 */
void nl_ws_tls_release(nl_ws_tls_conn_t* conn);

/*
 * 统一读 / 写：conn == NULL 时走明文 socket（与未 enable TLS 完全一致），
 * 非空时走 TLS。返回 >0 字节数；<=0 表示失败 / 对端关闭。
 * TLS 发送内部循环处理部分写（mbedTLS 单次写约 16KB 上限）。
 * sock 为平台 socket 句柄（POSIX 为 fd，Windows 为 SOCKET）。
 */
int nl_ws_io_recv(nl_ws_tls_conn_t* conn, uintptr_t sock, void* buf, int len);
int nl_ws_io_send(nl_ws_tls_conn_t* conn, uintptr_t sock, const void* buf, int len);

#endif /* NL_WS_TLS_H */
