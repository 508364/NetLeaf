/**
 * netleaf_worker_pool_windows.h
 *
 * 旧版阻塞式 worker 线程池（legacy worker pool）的内部接口。
 *
 * 该模块仅在 CMake 选项 NETLEAF_LEGACY_WORKER_POOL=ON 时参与编译（默认 OFF）。
 * 默认构建走 IOCP 引擎（nl_web_server 的 pr_iocp_consume_loop），不再包含本模块。
 *
 * 设计：模块不依赖 nl_web_server 的完整结构体，只通过调用方填充的上下文
 * （socket / running 标志 / 线程句柄数组 / 处理回调）工作，避免头文件耦合。
 */
#ifndef NETLEAF_WORKER_POOL_WINDOWS_H
#define NETLEAF_WORKER_POOL_WINDOWS_H

#include <winsock2.h>
#include <windows.h>

typedef struct nl_worker_pool_ctx {
    SOCKET        sock;           /* 监听 socket（worker 各自 accept） */
    volatile long* running;       /* 服务器运行标志（0 退出循环） */
    HANDLE*       handles;        /* 外部提供的线程句柄数组，容量 capacity */
    int           capacity;       /* handles 数组容量 */
    int           count;          /* 输出：实际成功启动的 worker 数 */
    int           active;         /* 输出：池是否处于运行态 */
    void (*handle_client)(void* owner, SOCKET client); /* 单连接处理回调 */
    void*         owner;          /* 回调上下文（通常是 nl_web_server_t*） */
} nl_worker_pool_ctx_t;

/**
 * 启动 target 个 worker 线程（每个线程循环 accept 并同步处理连接）。
 * 返回实际成功启动的数量；0 表示一个也没起来（调用方应回退到单线程 accept）。
 */
int nl_worker_pool_start(nl_worker_pool_ctx_t* ctx, int target);

/**
 * 等待已启动的 worker 退出并回收句柄（服务器 stop 后调用）。
 */
void nl_worker_pool_join(nl_worker_pool_ctx_t* ctx, int started);

#endif /* NETLEAF_WORKER_POOL_WINDOWS_H */
