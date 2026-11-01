/**
 * netleaf_worker_pool_windows.c
 *
 * 旧版阻塞式 worker 线程池实现。仅在 NETLEAF_LEGACY_WORKER_POOL=ON 时编译。
 * 默认构建不包含本文件，主路径为 IOCP 引擎。
 */
#include "netleaf_worker_pool_windows.h"

static DWORD WINAPI nl_worker_thread(LPVOID arg) {
    nl_worker_pool_ctx_t* ctx = (nl_worker_pool_ctx_t*)arg;
    if (!ctx) return 0;

    while (*ctx->running) {
        SOCKET client = accept(ctx->sock, NULL, NULL);
        if (client == INVALID_SOCKET) {
            // accept 失败（fd 耗尽/被中断）时退避，避免 tight loop 空转
            Sleep(10);
            continue;
        }
        ctx->handle_client(ctx->owner, client);
    }
    return 0;
}

int nl_worker_pool_start(nl_worker_pool_ctx_t* ctx, int target) {
    if (!ctx || !ctx->handles || ctx->capacity <= 0 || target <= 0) {
        if (ctx) { ctx->count = 0; ctx->active = 0; }
        return 0;
    }
    if (target > ctx->capacity) target = ctx->capacity;

    int started = 0;
    for (int i = 0; i < target; i++) {
        ctx->handles[i] = CreateThread(NULL, 0, nl_worker_thread, ctx, 0, NULL);
        if (!ctx->handles[i]) break;
        started++;
    }
    ctx->count  = started;
    ctx->active = 1;
    return started;
}

void nl_worker_pool_join(nl_worker_pool_ctx_t* ctx, int started) {
    if (!ctx) return;
    if (started > ctx->capacity) started = ctx->capacity;
    for (int i = 0; i < started; i++) {
        if (ctx->handles && ctx->handles[i]) {
            WaitForSingleObject(ctx->handles[i], INFINITE);
            CloseHandle(ctx->handles[i]);
            ctx->handles[i] = NULL;
        }
    }
    ctx->count  = 0;
    ctx->active = 0;
}
