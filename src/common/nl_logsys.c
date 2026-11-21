/*
 * nl_logsys.c - 统一日志系统实现（平台无关，一份实现）
 *
 * 收敛三平台（linux/windows/macos）此前各自重复的 ~120 行日志实现：
 * 级别过滤、格式化输出、回调分发、debug 模式状态全部下沉到本文件。
 *
 * 相比旧实现的增强（第193轮）：
 *   - 时间戳精确到毫秒（旧：秒级）
 *   - 输出携带线程 ID（多线程服务端可区分日志来源）
 *   - 级别字段定宽对齐（[INFO ]），列对齐便于肉眼扫读
 *   - 新增文件 sink：可选落盘 + 按大小轮转（nl_log_set_file）
 *   - 新增 nl_log_get_level 查询接口
 *   - 文件写入/轮转由互斥锁保护，多线程并发写不交错、不丢轮转
 *
 * 输出格式（第194轮定稿，级别在前）：
 *   [INFO ] [T12345] [2026-11-20 17:30:45.123] 消息
 *
 * ANSI 颜色（第194轮新增，仅 stderr）：
 *   按级别着色级别字段，便于快速分辨：DEBUG 青 / INFO 绿 / WARN 黄 / ERROR 红；
 *   仅当 stderr 是控制台时启用（Windows 需成功开启 VT 处理），重定向到
 *   文件/管道或设置 NO_COLOR 环境变量时自动禁用；文件 sink 永不着色。
 *
 * 语义保持不变：
 *   - 级别过滤：level < 当前级别则丢弃
 *   - 设置了 callback 时只走 callback（不写 stderr）；文件 sink 独立于
 *     callback，无论是否设置 callback，只要开了文件就落盘
 *   - nl_debug_enable(1) 将级别降到 DEBUG 并记录 debug_mode 标志
 */

#include "netleaf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#if defined(_WIN32)
    /* netleaf.h 已先包含 winsock2.h，此处补 windows.h 是安全顺序 */
    #include <windows.h>
#else
    #include <pthread.h>
    #include <sys/time.h>
    #include <unistd.h>
    #if defined(__APPLE__)
        #include <pthread.h>
    #else
        #include <sys/syscall.h>
    #endif
#endif

/* ============================================================
 * 全局状态
 * ============================================================ */
static nl_log_level_t g_level = NL_LOG_INFO;
static nl_log_callback g_callback = NULL;
static void* g_callback_ud = NULL;
static int g_debug_mode = 0;

/* 文件 sink 状态 */
static FILE*  g_file = NULL;
static char   g_path[512];
static size_t g_max_bytes = 0;   /* 0 = 不轮转 */
static int    g_keep = 0;        /* 保留的历史文件个数 path.1 .. path.N */
static size_t g_written = 0;     /* 当前文件已写字节数（含换行） */

/* 文件 sink 互斥锁（写入 + 轮转必须串行） */
#if defined(_WIN32)
static INIT_ONCE      g_lock_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_lock;
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

#if defined(_WIN32)
static BOOL CALLBACK nl__lock_init(PINIT_ONCE once, PVOID param, PVOID* ctx) {
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_lock);
    return TRUE;
}
static void nl__lock(void) {
    InitOnceExecuteOnce(&g_lock_once, nl__lock_init, NULL, NULL);
    EnterCriticalSection(&g_lock);
}
static void nl__unlock(void) { LeaveCriticalSection(&g_lock); }
#else
static void nl__lock(void)   { pthread_mutex_lock(&g_lock); }
static void nl__unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

/* ============================================================
 * 平台差异：线程 ID / 毫秒时间戳
 * ============================================================ */
static unsigned long nl__tid(void) {
#if defined(_WIN32)
    return (unsigned long)GetCurrentThreadId();
#elif defined(__APPLE__)
    unsigned long long tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (unsigned long)tid;
#else
    return (unsigned long)syscall(SYS_gettid);
#endif
}

static void nl__timestamp(char* buf, size_t cap) {
#if defined(_WIN32)
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             (int)st.wYear, (int)st.wMonth, (int)st.wDay,
             (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
             (int)st.wMilliseconds);
#else
    struct timeval tv;
    struct tm tm_buf;
    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_buf);
    snprintf(buf, cap, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             (int)(tv.tv_usec / 1000));
#endif
}

static const char* nl__level_name(nl_log_level_t level) {
    static const char* names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    if (level < NL_LOG_DEBUG || level > NL_LOG_ERROR) return "?????";
    return names[level];
}

/* ============================================================
 * ANSI 颜色（仅 stderr，第194轮）
 * ============================================================ */
/* 按级别返回颜色码：DEBUG 青 / INFO 绿 / WARN 黄 / ERROR 红 */
static const char* nl__level_color(nl_log_level_t level) {
    switch (level) {
        case NL_LOG_DEBUG: return "\033[36m";  /* 青色 */
        case NL_LOG_INFO:  return "\033[32m";  /* 绿色 */
        case NL_LOG_WARN:  return "\033[33m";  /* 黄色 */
        case NL_LOG_ERROR: return "\033[31m";  /* 红色 */
        default:           return "\033[0m";
    }
}

#define NL__COLOR_RESET "\033[0m"

/* stderr 是否可着色：需是控制台且未设 NO_COLOR（https://no-color.org）。
 * 结果缓存（首次调用决定；Windows 首次顺带开启 VT 转义处理）。 */
static int nl__color_enabled(void) {
    static int cached = -1;
    if (cached >= 0) return cached;

    int on = 0;
    if (!getenv("NO_COLOR")) {
#if defined(_WIN32)
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
            /* Win10+ 控制台默认已支持 VT，显式开启以防被关闭 */
            if (SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                on = 1;
            }
        }
#else
        on = isatty(STDERR_FILENO) ? 1 : 0;
#endif
    }
    cached = on;
    return on;
}

/* ============================================================
 * 文件轮转
 * ============================================================ */
/* 轮转链：path -> path.1 -> path.2 ... -> path.keep（最旧被删除）。
 * 调用前提：g_file 已关闭、g_lock 已持有。 */
static void nl__rotate(void) {
    char src[576], dst[576];

    if (g_keep <= 0) {
        /* 不保留历史：直接删除旧文件重新开始 */
        remove(g_path);
    } else {
        /* 最旧的超出 keep 范围，先删 */
        snprintf(dst, sizeof(dst), "%s.%d", g_path, g_keep);
        remove(dst);
        /* 整体后移一位（Windows rename 不允许覆盖，先 remove 目标） */
        for (int i = g_keep - 1; i >= 1; i--) {
            snprintf(src, sizeof(src), "%s.%d", g_path, i);
            snprintf(dst, sizeof(dst), "%s.%d", g_path, i + 1);
            remove(dst);
            rename(src, dst);
        }
        snprintf(dst, sizeof(dst), "%s.1", g_path);
        remove(dst);
        rename(g_path, dst);
    }

    g_file = fopen(g_path, "ab");
    g_written = 0;
}

/* 打开文件 sink。path==NULL 关闭。返回 NL_OK / NL_ERROR / NL_EINVAL。
 * max_bytes==0 表示不轮转；keep 为保留的历史文件数（<0 视为 0）。 */
NL_API int nl_log_set_file(const char* path, size_t max_bytes, int keep) {
    nl__lock();
    if (g_file) {
        fclose(g_file);
        g_file = NULL;
    }
    if (!path || !path[0]) {
        g_path[0] = '\0';
        g_max_bytes = 0;
        g_keep = 0;
        g_written = 0;
        nl__unlock();
        return NL_OK;
    }

    snprintf(g_path, sizeof(g_path), "%s", path);
    g_max_bytes = max_bytes;
    g_keep = keep < 0 ? 0 : keep;

    g_file = fopen(g_path, "ab");
    if (!g_file) {
        g_path[0] = '\0';
        g_max_bytes = 0;
        g_keep = 0;
        nl__unlock();
        return NL_ERROR;
    }
    /* 追加模式：以已有大小为起点，避免首轮轮转判断失真 */
    if (fseek(g_file, 0, SEEK_END) == 0) {
        long pos = ftell(g_file);
        g_written = pos > 0 ? (size_t)pos : 0;
    } else {
        g_written = 0;
    }
    nl__unlock();
    return NL_OK;
}

NL_API void nl_log_close_file(void) {
    nl_log_set_file(NULL, 0, 0);
}

/* ============================================================
 * 核心输出
 * ============================================================ */
/* 格式化一行（无换行）：[级别] [T线程ID] [时间戳] 消息
 * color 为 1 时级别字段包 ANSI 颜色码（仅用于 stderr）。 */
static int nl__format_line(char* buf, size_t cap, nl_log_level_t level,
                           const char* msg, int color) {
    char ts[40];
    nl__timestamp(ts, sizeof(ts));
    if (color) {
        return snprintf(buf, cap, "%s[%-5s]" NL__COLOR_RESET " [T%lu] [%s] %s",
                        nl__level_color(level), nl__level_name(level),
                        nl__tid(), ts, msg);
    }
    return snprintf(buf, cap, "[%-5s] [T%lu] [%s] %s",
                    nl__level_name(level), nl__tid(), ts, msg);
}

static void nl__emit(nl_log_level_t level, const char* fmt, va_list ap) {
    if (level < g_level) return;

    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);

    nl_log_callback cb = g_callback;
    if (cb) {
        /* callback 路径：与旧实现一致，只分发不写 stderr（在锁外调用，
         * 允许用户回调内再次记录日志而不死锁） */
        cb(level, msg, g_callback_ud);
    } else {
        char line[2300];
        int n = nl__format_line(line, sizeof(line), level, msg, nl__color_enabled());
        if (n > 0) {
            if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
            fwrite(line, 1, (size_t)n, stderr);
            fputc('\n', stderr);
        }
    }

    /* 文件 sink 独立于 callback：只要开着就落盘（永不着色） */
    if (g_file) {
        char line[2300];
        int n = nl__format_line(line, sizeof(line), level, msg, 0);
        if (n < 0) return;
        if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;

        nl__lock();
        if (g_file) {
            if (g_max_bytes > 0 &&
                g_written > 0 &&
                g_written + (size_t)n + 1 > g_max_bytes) {
                fclose(g_file);
                g_file = NULL;
                nl__rotate();
            }
            if (g_file) {
                if (fwrite(line, 1, (size_t)n, g_file) == (size_t)n &&
                    fputc('\n', g_file) != EOF) {
                    g_written += (size_t)n + 1;
                }
                fflush(g_file);   /* 日志量小，逐条刷盘保证崩溃不丢 */
            }
        }
        nl__unlock();
    }
}

/* ============================================================
 * 公共 API
 * ============================================================ */
NL_API void nl_log_set_level(nl_log_level_t level) {
    g_level = level;
}

NL_API nl_log_level_t nl_log_get_level(void) {
    return g_level;
}

NL_API void nl_log_set_callback(nl_log_callback callback, void* user_data) {
    g_callback = callback;
    g_callback_ud = user_data;
}

NL_API void nl_log(nl_log_level_t level, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    nl__emit(level, format, ap);
    va_end(ap);
}

NL_API void nl_log_debug(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    nl__emit(NL_LOG_DEBUG, format, ap);
    va_end(ap);
}

NL_API void nl_log_info(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    nl__emit(NL_LOG_INFO, format, ap);
    va_end(ap);
}

NL_API void nl_log_warn(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    nl__emit(NL_LOG_WARN, format, ap);
    va_end(ap);
}

NL_API void nl_log_error(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    nl__emit(NL_LOG_ERROR, format, ap);
    va_end(ap);
}

NL_API void nl_debug_enable(int enable) {
    g_debug_mode = enable;
    if (enable) {
        g_level = NL_LOG_DEBUG;
        nl_log_info("Debug mode enabled");
    } else {
        nl_log_info("Debug mode disabled");
    }
}

NL_API int nl_debug_is_enabled(void) {
    return g_debug_mode;
}
