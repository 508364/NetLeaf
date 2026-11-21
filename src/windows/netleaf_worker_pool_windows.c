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

    /* 第189轮 stop 死锁修复：同 Linux —— 阻塞 accept 不检查 running，
     * 调用方必须先 join 再 closesocket → 空闲时 stop 永远卡死。
     * 先 WSAPoll(200ms) 等可读再 accept，空闲时最多 200ms 唤醒一次。
     * listen 已被调用方设为非阻塞：竞争输家拿 WSAEWOULDBLOCK 回循环退避。 */
    while (*ctx->running) {
        WSAPOLLFD pfd;
        pfd.fd = ctx->sock;
        pfd.events = POLLRDNORM;
        pfd.revents = 0;
        int rc = WSAPoll(&pfd, 1, 200);
        if (rc <= 0) {
            if (!*ctx->running) break;
            Sleep(10);
            continue;
        }
        SOCKET client = accept(ctx->sock, NULL, NULL);
        if (client == INVALID_SOCKET) {
            // accept 失败（wouldblock/fd 耗尽/被中断）时退避，避免 tight loop 空转
            Sleep(10);
            continue;
        }
        /* Windows：accept 的新 socket 继承 listening socket 的阻塞模式，恢复阻塞 */
        u_long nb = 0;
        ioctlsocket(client, FIONBIO, &nb);
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
