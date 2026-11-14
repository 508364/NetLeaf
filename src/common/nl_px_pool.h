/*
 * nl_px_pool.h - 上游 keep-alive 连接池（平台无关）
 *
 * 三平台此前各有一份逐行相同的分桶链表池实现，仅 fd 类型与
 * close/probe/now 三个小动作不同。本模块把数据结构与回收策略集中到公共层，
 * 平台层只提供三个操作函数（见 nl_px_pool_ops）。
 *
 * fd 以 intptr_t 承载：
 *   - POSIX：直接是 int，无可用连接返回 -1；
 *   - Windows：SOCKET 强转（INVALID_SOCKET 转回来就是 -1）。
 *
 * 单线程引擎独占池对象，内部不加锁（与既有一致）。
 */
#ifndef NL_PX_POOL_H
#define NL_PX_POOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "host:port|proto" 键长上限（含结尾 NUL） */
#define NL_PX_POOL_KEY_CAP 576

typedef struct nl_px_pool_ops {
    int64_t (*now_ms)(void);                 /* 单调毫秒时钟 */
    int     (*probe_alive)(intptr_t fd);     /* 1=健康可复用，0=丢弃 */
    void    (*close_fd)(intptr_t fd);        /* 关闭池中 fd */
} nl_px_pool_ops;

typedef struct nl_px_pool_entry {
    intptr_t fd;
    int64_t  last_used_ms;
    struct nl_px_pool_entry* next;
} nl_px_pool_entry;

typedef struct nl_px_pool_bucket {
    char  key[NL_PX_POOL_KEY_CAP];
    nl_px_pool_entry* head;
    int   count;
} nl_px_pool_bucket;

typedef struct nl_px_pool {
    nl_px_pool_bucket* buckets;
    int bucket_cap;
    int total_idle;
    int max_total;             /* 全局空闲上限，默认 256 */
    int max_per_key;           /* 每 key 空闲上限，默认 8 */
    int idle_timeout_ms;       /* 空闲超时，默认 30000 */
    nl_px_pool_ops ops;
} nl_px_pool;

/* 构造 "host:port|proto"（不做内存分配） */
void nl_px_pool_make_key(char* out, size_t cap, const char* host, int port, int proto);

/* 初始化池；上限 <=0 时采用默认值。ops 三项必须非空。 */
void nl_px_pool_init(nl_px_pool* p, const nl_px_pool_ops* ops,
                     int max_total, int max_per_key, int idle_timeout_ms);

/* 取一条可用连接（探测存活，死连接丢弃）；无可用返回 -1。 */
intptr_t nl_px_pool_get(nl_px_pool* p, const char* host, int port, int proto);

/* 按已构造好的 key 归还连接（超限或池未就绪则直接关闭）。 */
void nl_px_pool_put(nl_px_pool* p, const char* key, intptr_t fd);

/* 回收空闲超时的连接。 */
void nl_px_pool_reap(nl_px_pool* p, int64_t now_ms);

/* 销毁池（关闭全部空闲连接，释放分桶数组；对象本身由调用方释放）。 */
void nl_px_pool_destroy(nl_px_pool* p);

#ifdef __cplusplus
}
#endif

#endif /* NL_PX_POOL_H */
