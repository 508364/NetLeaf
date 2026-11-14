/*
 * nl_px_pool.c - 上游 keep-alive 连接池实现（平台无关）
 *
 * 结构语义与三平台旧实现完全一致：按 "host:port|proto" 分桶（FNV-1a +
 * 线性探测），取用时逐个探活，死连接立即关闭丢弃；归还时受全局/单 key
 * 上限约束；空闲超时由 nl_px_pool_reap 统一回收。
 */
#include "nl_px_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NL_PX_POOL_DEFAULT_BUCKETS 64

void nl_px_pool_make_key(char* out, size_t cap, const char* host, int port, int proto) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s:%d|%d", host ? host : "", port, proto);
}

/* FNV-1a 哈希 */
static unsigned px_pool_hash(const char* key) {
    unsigned h = 2166136261u;
    for (; *key; key++) { h ^= (unsigned char)*key; h *= 16777619u; }
    return h;
}

/* 定位桶（create=1 时创建新键桶；线性探测解决碰撞） */
static nl_px_pool_bucket* px_pool_bucket(nl_px_pool* p, const char* key, int create) {
    if (!p || !p->buckets || p->bucket_cap <= 0 || !key) return NULL;
    int start = (int)(px_pool_hash(key) % (unsigned)p->bucket_cap);
    for (int i = 0; i < p->bucket_cap; i++) {
        nl_px_pool_bucket* b = &p->buckets[(start + i) % p->bucket_cap];
        if (b->key[0] == '\0') {
            if (!create) return NULL;
            snprintf(b->key, sizeof(b->key), "%s", key);
            return b;
        }
        if (strcmp(b->key, key) == 0) return b;
    }
    return NULL;
}

static void px_pool_close(nl_px_pool* p, intptr_t fd) {
    if (p && p->ops.close_fd) p->ops.close_fd(fd);
}

void nl_px_pool_init(nl_px_pool* p, const nl_px_pool_ops* ops,
                     int max_total, int max_per_key, int idle_timeout_ms) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    if (ops) p->ops = *ops;
    p->bucket_cap = NL_PX_POOL_DEFAULT_BUCKETS;
    p->buckets = (nl_px_pool_bucket*)calloc((size_t)p->bucket_cap, sizeof(nl_px_pool_bucket));
    if (!p->buckets) { p->bucket_cap = 0; return; }
    p->max_total = max_total > 0 ? max_total : 256;
    p->max_per_key = max_per_key > 0 ? max_per_key : 8;
    p->idle_timeout_ms = idle_timeout_ms > 0 ? idle_timeout_ms : 30000;
}

intptr_t nl_px_pool_get(nl_px_pool* p, const char* host, int port, int proto) {
    char key[NL_PX_POOL_KEY_CAP];
    nl_px_pool_make_key(key, sizeof(key), host, port, proto);
    nl_px_pool_bucket* b = px_pool_bucket(p, key, 0);
    if (!b) return (intptr_t)-1;
    nl_px_pool_entry** pp = &b->head;
    while (*pp) {
        nl_px_pool_entry* e = *pp;
        if (p->ops.probe_alive && p->ops.probe_alive(e->fd)) {
            *pp = e->next;
            intptr_t fd = e->fd;
            free(e);
            b->count--; p->total_idle--;
            return fd;
        }
        px_pool_close(p, e->fd);        /* 死连接：丢弃 */
        *pp = e->next;
        free(e);
        b->count--; p->total_idle--;
    }
    return (intptr_t)-1;
}

void nl_px_pool_put(nl_px_pool* p, const char* key, intptr_t fd) {
    if (fd < 0) return;
    if (!p || !p->buckets || !key || key[0] == '\0' ||
        p->total_idle >= p->max_total) { px_pool_close(p, fd); return; }
    nl_px_pool_bucket* b = px_pool_bucket(p, key, 1);
    if (!b || b->count >= p->max_per_key) { px_pool_close(p, fd); return; }
    nl_px_pool_entry* e = (nl_px_pool_entry*)malloc(sizeof(*e));
    if (!e) { px_pool_close(p, fd); return; }
    e->fd = fd;
    e->last_used_ms = p->ops.now_ms ? p->ops.now_ms() : 0;
    e->next = b->head;
    b->head = e;
    b->count++; p->total_idle++;
}

void nl_px_pool_reap(nl_px_pool* p, int64_t now_ms) {
    if (!p || !p->buckets) return;
    for (int i = 0; i < p->bucket_cap; i++) {
        nl_px_pool_bucket* b = &p->buckets[i];
        nl_px_pool_entry** pp = &b->head;
        while (*pp) {
            nl_px_pool_entry* e = *pp;
            if (now_ms - e->last_used_ms >= p->idle_timeout_ms) {
                px_pool_close(p, e->fd);
                *pp = e->next;
                free(e);
                b->count--; p->total_idle--;
            } else {
                pp = &e->next;
            }
        }
    }
}

void nl_px_pool_destroy(nl_px_pool* p) {
    if (!p) return;
    if (p->buckets) {
        for (int i = 0; i < p->bucket_cap; i++) {
            nl_px_pool_entry* e = p->buckets[i].head;
            while (e) {
                nl_px_pool_entry* n = e->next;
                px_pool_close(p, e->fd);
                free(e);
                e = n;
            }
            p->buckets[i].head = NULL;
            p->buckets[i].key[0] = '\0';
            p->buckets[i].count = 0;
        }
        free(p->buckets);
        p->buckets = NULL;
    }
    p->bucket_cap = 0;
    p->total_idle = 0;
}
