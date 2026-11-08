#ifndef NETLEAF_LINKAGG_INTERNAL_H
#define NETLEAF_LINKAGG_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "netleaf_linkagg.h"
#include "netleaf_ipc.h"

// ------------------------------------------------------------------
// 平台线程/同步原语（三平台仅在少量宏上有差异，其余逻辑全部共享）
// ------------------------------------------------------------------
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    typedef CRITICAL_SECTION  nl_lagg_mutex_t;
    typedef CONDITION_VARIABLE nl_lagg_cond_t;
    typedef HANDLE             nl_lagg_thread_t;

    #define NL_LAGG_MUTEX_INIT(m)     InitializeCriticalSection(m)
    #define NL_LAGG_MUTEX_LOCK(m)     EnterCriticalSection(m)
    #define NL_LAGG_MUTEX_UNLOCK(m)   LeaveCriticalSection(m)
    #define NL_LAGG_MUTEX_DESTROY(m)  DeleteCriticalSection(m)

    #define NL_LAGG_COND_INIT(c)            InitializeConditionVariable(c)
    #define NL_LAGG_COND_SIGNAL(c)          WakeConditionVariable(c)
    #define NL_LAGG_COND_BROADCAST(c)       WakeAllConditionVariable(c)
    #define NL_LAGG_COND_DESTROY(c)         ((void)(c))
    #define NL_LAGG_THREAD_INVALID          ((HANDLE)NULL)
#else
    #include <pthread.h>
    typedef pthread_mutex_t nl_lagg_mutex_t;
    typedef pthread_cond_t  nl_lagg_cond_t;
    typedef pthread_t       nl_lagg_thread_t;

    #define NL_LAGG_MUTEX_INIT(m)     pthread_mutex_init(m, NULL)
    #define NL_LAGG_MUTEX_LOCK(m)     pthread_mutex_lock(m)
    #define NL_LAGG_MUTEX_UNLOCK(m)   pthread_mutex_unlock(m)
    #define NL_LAGG_MUTEX_DESTROY(m)  pthread_mutex_destroy(m)

    #define NL_LAGG_COND_INIT(c)            pthread_cond_init(c, NULL)
    #define NL_LAGG_COND_SIGNAL(c)          pthread_cond_signal(c)
    #define NL_LAGG_COND_BROADCAST(c)       pthread_cond_broadcast(c)
    #define NL_LAGG_COND_DESTROY(c)         pthread_cond_destroy(c)
    #define NL_LAGG_THREAD_INVALID          ((pthread_t)0)
#endif

// 跨进程数据池服务默认监听端点（由端口派生，不含任何密钥信息）
#define NL_LAGG_POOL_SERVICE_DEFAULT 1
#define NL_LAGG_IPC_LINE_MAX 512

// ------------------------------------------------------------------
// 后端节点
// ------------------------------------------------------------------
typedef struct nl_lagg_backend {
    int type;                      // NL_BACKEND_HTTP / NL_BACKEND_IPC(拉取型)
    char endpoint[512];            // HTTP: "host:port"；拉取型: 宿主端点点名
    char host[256];                // HTTP 后端主机
    int port;                      // HTTP 后端端口
    int weight;
    int current_connections;
    long current_weight;           // SWRR 累计权重
    int healthy;                   // 1=正常，0=异常（超时 / 连接失败）
    int consecutive_failures;
    uint64_t last_failure_ms;

    // 该端点的待处理请求池（FIFO）
    nl_lagg_request_t* pool_head;
    nl_lagg_request_t* pool_tail;
    int pool_count;

    struct nl_lagg_backend* next;
} nl_lagg_backend_t;

// ------------------------------------------------------------------
// 待处理请求（数据池条目）
// 引用计数：客户端 1 + 队列/宿主 1；归零后释放。
// ------------------------------------------------------------------
struct nl_lagg_request {
    uint64_t id;
    char port_id[256];             // 归属端口 ID（server->port_id）
    char endpoint[512];            // 归属后端端点
    char* data;                    // 原始请求字节
    size_t len;
    char* resp;                    // 宿主回送的响应
    size_t resp_len;
    int responded;
    int aborted;
    int refs;
    int queued;                    // 是否仍在后端池中（受 server->mutex 保护）
    nl_lagg_server_t* server;
    nl_lagg_backend_t* backend;
    struct nl_lagg_request* next;  // 数据池 FIFO 链
    nl_lagg_mutex_t mutex;
    nl_lagg_cond_t cond;
};

// 跨进程数据池连接（宿主进程）
typedef struct nl_lagg_pool_conn {
    void* conn;                    // nl_ipc 连接
    nl_lagg_thread_t thread;
    int owned_by_stop;             // stop 已接管回收，线程不再自行摘链/释放
    struct nl_lagg_pool_conn* next;
} nl_lagg_pool_conn_t;

// ------------------------------------------------------------------
// 聚合服务端
// ------------------------------------------------------------------
struct nl_lagg_server {
    int port;
    char port_id[256];
    int policy;
    nl_lagg_backend_t* backends;
    int backend_count;
    int current_index;
    volatile int running;
    nl_lagg_mutex_t mutex;         // 保护后端链表/数据池/健康状况
    nl_lagg_cond_t pool_cond;      // 数据入池时唤醒宿主拉取
    intptr_t listen_fd;            // accept 监听 fd
    nl_lagg_thread_t accept_thread;
    int accept_thread_valid;
    int active_conns;              // 正在处理的客户端连接数（stop 时等待归零）

    // 超时 / 重试 / 健康
    int pull_timeout_ms;           // 客户端等待宿主响应的时间
    int pull_retries;              // 超时后重试次数（默认 1）

    // 跨进程数据池服务
    int pool_service_enabled;
    nl_ipc_t* pool_ipc;
    nl_lagg_thread_t pool_thread;
    int pool_thread_valid;
    nl_lagg_mutex_t pool_mutex;    // 保护 pool_conns
    nl_lagg_pool_conn_t* pool_conns;
    char pool_endpoint[512];
    int pool_port;                 // macOS(IPC=TCP) 专用

    uint64_t next_request_id;
    uint64_t total_requests;
    uint64_t failed_requests;

    nl_lagg_on_connect_cb on_connect;
    nl_lagg_on_disconnect_cb on_disconnect;
    void* user_data;
};

// ------------------------------------------------------------------
// 全局 ID 注册表（进程内端口 ID 冲突检测）
// ------------------------------------------------------------------
typedef struct nl_lagg_id_entry {
    char id[256];
    struct nl_lagg_id_entry* next;
} nl_lagg_id_entry_t;

bool nl_lagg_id_exists(const char* id);
bool nl_lagg_id_register(const char* id);
void nl_lagg_id_unregister(const char* id);

void nl_lagg_init_global_mutex(void);
void nl_lagg_mutex_lock(nl_lagg_mutex_t* m);
void nl_lagg_mutex_unlock(nl_lagg_mutex_t* m);

// 共享核心（netleaf_linkagg_core.c）内部辅助
void nl_lagg_backend_mark_healthy(nl_lagg_server_t* server, nl_lagg_backend_t* backend);
void nl_lagg_backend_mark_failed(nl_lagg_server_t* server, nl_lagg_backend_t* backend);

#endif // NETLEAF_LINKAGG_INTERNAL_H
