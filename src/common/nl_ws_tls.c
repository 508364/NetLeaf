/*
 * nl_ws_tls.c - 第205轮 WSS：WebSocket over TLS 运行时后端实现（平台无关）
 *
 * 核心库（netleaf_core）不直接链接 TLS 扩展库：netleaf_tls3 / netleaf_tls2
 * 均反向链接 netleaf_core，硬链接会形成循环依赖（Windows 下更会造成
 * "导入库依赖对方 DLL 先产出"的构建顺序死锁）。因此在运行时通过
 * dlopen / LoadLibrary 加载后端库并解析符号：tls3 优先，回退 tls2，
 * 与 netleaf_https 的 BUILD_TLS3 优先策略一致。
 *
 * 证书 / 私钥路径由调用方传入（PEM 文件路径），本文件不硬编码任何密钥。
 * 后端库文件名是构建产物名（libnetleaf_tls3.so / netleaf_tls3.dll 等），
 * 不属于敏感信息。
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* dladdr（glibc 需要）；先于任何头文件定义 */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* nl_util.h 先行：其 NL_API 宏与 netleaf_module.h / netleaf_tls*.h 展开
 * 一致（同为 NL_EXPORTS 条件），后续头的 #ifndef NL_API 会跳过重复定义。 */
#include "nl_util.h"
#include "netleaf_tls3.h"
#include "netleaf_tls2.h"

#include "common/nl_ws_tls.h"

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/socket.h>   /* 第205轮修复：明文 recv/send 声明（隐式声明告警） */
#include <sys/types.h>
#endif

/* ============================================================
 * 后端符号表（tls3 / tls2 函数签名一一同构，统一收敛）
 * ============================================================ */

typedef void* (*ws_tls_create_fn)(void);
typedef void  (*ws_tls_destroy_fn)(void*);
typedef int   (*ws_tls_configure_fn)(void*, const void*);
typedef int   (*ws_tls_handshake_fn)(void*, int, int);   /* handshake_ex(ctx, sock, timeout_ms) */
typedef int   (*ws_tls_xfer_fn)(void*, const void*, size_t); /* send */
typedef int   (*ws_tls_read_fn)(void*, void*, size_t);       /* recv */
typedef int   (*ws_tls_avail_fn)(void);                      /* is_available */

typedef struct ws_tls_backend {
    void* handle;               /* dlopen / LoadLibrary 句柄（进程内常驻，不卸载） */
    int is_tls3;                /* 1=tls3 后端，0=tls2 后端（决定 config 结构体） */
    ws_tls_create_fn    create;
    ws_tls_destroy_fn   destroy;
    ws_tls_configure_fn configure;
    ws_tls_handshake_fn handshake;
    ws_tls_xfer_fn      send;
    ws_tls_read_fn      recv;
    ws_tls_avail_fn     is_available;
} ws_tls_backend_t;

struct nl_ws_tls_conn {
    void* ctx;                  /* nl_tls3_ctx_t* / nl_tls2_ctx_t* */
};

static ws_tls_backend_t g_be;
static int g_be_ok;             /* 1 = 后端已加载且符号齐全 */

/* nl_once 槽位 8 个（见 nl_util.c NL_ONCE_SLOTS），7 号预留给 WS-TLS 后端 */
#define NL_WS_TLS_ONCE_IDX 7

/* ============================================================
 * 动态库 / 符号解析
 * ============================================================ */

/* 库名格式：候选 1（与核心库同目录）与候选 2（加载器默认搜索路径） */
#ifdef _WIN32
#define WS_LIB_IN_DIR "%s\\%s.dll"     /* 例：<dir>\netleaf_tls3.dll */
#define WS_LIB_FILE   "%s.dll"
#elif defined(__APPLE__)
#define WS_LIB_IN_DIR "%s/lib%s.dylib" /* 例：<dir>/libnetleaf_tls3.dylib */
#define WS_LIB_FILE   "lib%s.dylib"
#else
#define WS_LIB_IN_DIR "%s/lib%s.so"    /* 例：<dir>/libnetleaf_tls3.so */
#define WS_LIB_FILE   "lib%s.so"
#endif

static void* ws_open_lib(const char* path) {
#ifdef _WIN32
    return (void*)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void ws_close_lib(void* handle) {
    if (!handle) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

/* 取符号地址，统一转成 void*（经 union 规避 -Wpedantic 的指针转换告警） */
static void* ws_get_sym(void* handle, const char* name) {
#ifdef _WIN32
    union { void* o; FARPROC f; } u;
    u.f = GetProcAddress((HMODULE)handle, name);
    return u.o;
#else
    return dlsym(handle, name);
#endif
}

/* 取本模块（netleaf_core 自身）所在目录：
 * 构建树 build/lib 与安装布局 <prefix>/lib（Windows 为 bin）都把核心库与
 * TLS 扩展放在同一目录，"同目录候选"可不依赖 LD_LIBRARY_PATH / PATH。 */
static int ws_core_dir(char* out, size_t cap) {
    char path[1024];
    char* sep = NULL;
    char* q;

#ifdef _WIN32
    HMODULE mod = NULL;
    union { void* o; int (*f)(char*, size_t); } fa;
    fa.f = ws_core_dir;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)fa.o, &mod) || !mod)
        return 0;
    {
        DWORD n = GetModuleFileNameA(mod, path, (DWORD)sizeof(path));
        if (n == 0 || n >= sizeof(path)) return 0;
    }
#else
    Dl_info info;
    union { void* o; int (*f)(char*, size_t); } fa;
    fa.f = ws_core_dir;
    if (dladdr(fa.o, &info) == 0 || !info.dli_fname) return 0;
    if (strlen(info.dli_fname) >= sizeof(path)) return 0;
    strcpy(path, info.dli_fname);
#endif

    for (q = path; *q; q++) {
        if (*q == '/' || *q == '\\') sep = q;
    }
    if (!sep || sep == path) return 0;      /* 无目录分量（如纯文件名） */
    *sep = '\0';
    if (strlen(path) + 1 > cap) return 0;
    strcpy(out, path);
    return 1;
}

/* 解析一个后端的全部所需符号；成功置 g_be / g_be_ok 并返回 1 */
static int ws_try_backend(void* handle, int is_tls3) {
    const char* p = is_tls3 ? "nl_tls3_" : "nl_tls2_";
    ws_tls_backend_t be;
    char nbuf[48];

    memset(&be, 0, sizeof(be));
    be.handle = handle;
    be.is_tls3 = is_tls3;

#define WS_RESOLVE(field, fn_type, sym)                            \
    do {                                                           \
        union { void* o; void (*f)(void); } u;                      \
        snprintf(nbuf, sizeof(nbuf), "%s%s", p, (sym));             \
        u.o = ws_get_sym(handle, nbuf);                             \
        if (!u.o) return 0;                                        \
        be.field = (fn_type)u.f;                                    \
    } while (0)

    WS_RESOLVE(create,     ws_tls_create_fn,    "create");
    WS_RESOLVE(destroy,    ws_tls_destroy_fn,   "destroy");
    WS_RESOLVE(configure,  ws_tls_configure_fn, "configure");
    WS_RESOLVE(handshake,  ws_tls_handshake_fn, "handshake_ex");
    WS_RESOLVE(send,       ws_tls_xfer_fn,      "send");
    WS_RESOLVE(recv,       ws_tls_read_fn,      "recv");
    WS_RESOLVE(is_available, ws_tls_avail_fn,   "is_available");
#undef WS_RESOLVE

    if (!be.is_available()) return 0;   /* 库在但后端自报不可用 */

    g_be = be;
    g_be_ok = 1;
    return 1;
}

/* 进程内一次性加载体：tls3 优先，回退 tls2；每种后端先"同目录"再"默认搜索路径" */
static void ws_tls_do_load(void) {
    int round;

    for (round = 0; round < 2 && !g_be_ok; round++) {
        int is_tls3 = (round == 0);
        const char* base = is_tls3 ? "netleaf_tls3" : "netleaf_tls2";
        char path[2048];   /* dir(≤1024) + "/lib" + base + 后缀，防截断告警 */
        char dir[1024];
        void* h = NULL;

        /* 候选 1：与核心库同目录（构建树 / 安装布局均成立） */
        if (ws_core_dir(dir, sizeof(dir))) {
            snprintf(path, sizeof(path), WS_LIB_IN_DIR, dir, base);
            h = ws_open_lib(path);
        }
        /* 候选 2：动态加载器默认搜索路径
         * （Linux: LD_LIBRARY_PATH / RUNPATH / ldconfig；macOS: DYLD 路径；
         *  Windows: exe 目录 / PATH） */
        if (!h) {
            snprintf(path, sizeof(path), WS_LIB_FILE, base);
            h = ws_open_lib(path);
        }

        if (h) {
            if (ws_try_backend(h, is_tls3)) return;
            ws_close_lib(h);   /* 库在但符号不齐（版本过旧）：回退下一后端 */
        }
    }
}

int nl_ws_tls_load(void) {
    /* 第205轮 WSS：进程内一次性加载（幂等；失败也不重复尝试） */
    nl_once_run(NL_WS_TLS_ONCE_IDX, ws_tls_do_load);
    return g_be_ok ? 0 : -1;
}

/* ============================================================
 * 连接级 TLS 上下文
 * ============================================================ */

nl_ws_tls_conn_t* nl_ws_tls_accept(uintptr_t sock,
                                   const char* cert_file,
                                   const char* key_file) {
    void* ctx;
    int rc;
    nl_ws_tls_conn_t* conn;

    if (!g_be_ok || !cert_file || !key_file) return NULL;

    ctx = g_be.create();
    if (!ctx) return NULL;

    /* 第205轮 WSS：服务端角色；cert/key 为调用方传入的 PEM 路径（不硬编码） */
    if (g_be.is_tls3) {
        nl_tls3_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.cert_file = cert_file;
        cfg.key_file  = key_file;
        cfg.is_server = 1;
        cfg.handshake_timeout_ms = NL_WS_TLS_HS_TIMEOUT_MS;
        rc = g_be.configure(ctx, &cfg);
    } else {
        nl_tls2_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.cert_file = cert_file;
        cfg.key_file  = key_file;
        cfg.is_server = 1;
        cfg.handshake_timeout_ms = NL_WS_TLS_HS_TIMEOUT_MS;
        rc = g_be.configure(ctx, &cfg);
    }
    if (rc != 0) {                      /* NL_TLS3_OK / NL_TLS2_OK 均为 0 */
        g_be.destroy(ctx);
        return NULL;
    }

    /* 阻塞式 TLS 握手：与现有 ws 客户端线程（accept 后单线程处理）一致 */
    if (g_be.handshake(ctx, (int)sock, NL_WS_TLS_HS_TIMEOUT_MS) != 0) {
        g_be.destroy(ctx);
        return NULL;
    }

    conn = (nl_ws_tls_conn_t*)malloc(sizeof(*conn));
    if (!conn) {
        g_be.destroy(ctx);
        return NULL;
    }
    conn->ctx = ctx;
    return conn;
}

void nl_ws_tls_release(nl_ws_tls_conn_t* conn) {
    if (!conn) return;
    if (conn->ctx && g_be_ok) g_be.destroy(conn->ctx);
    free(conn);
}

/* ============================================================
 * 统一 IO：conn == NULL 走明文（与未 enable TLS 完全一致）
 * ============================================================ */

int nl_ws_io_recv(nl_ws_tls_conn_t* conn, uintptr_t sock, void* buf, int len) {
    if (conn) return g_be.recv(conn->ctx, buf, (size_t)len);
#ifdef _WIN32
    return recv((SOCKET)sock, (char*)buf, len, 0);
#else
    return (int)recv((int)sock, buf, (size_t)len, 0);
#endif
}

int nl_ws_io_send(nl_ws_tls_conn_t* conn, uintptr_t sock, const void* buf, int len) {
    if (conn) {
        /* mbedTLS 单次写约 16KB 上限：必须循环发完，否则对端收到截断帧 */
        int off = 0;
        while (off < len) {
            int n = g_be.send(conn->ctx, (const char*)buf + off,
                              (size_t)(len - off));
            if (n <= 0) return n;       /* 失败 / 对端关闭：交上层按断开处理 */
            off += n;
        }
        return off;
    }
#ifdef _WIN32
    return send((SOCKET)sock, (const char*)buf, len, 0);
#else
    return (int)send((int)sock, buf, (size_t)len, 0);
#endif
}
