/* ============================================================
 * NetLeaf Web 引擎压测工具 (P8) —— 跨平台（Linux / macOS / Windows）
 * ============================================================
 * 对比三种模式：
 *   1. legacy pool  —— 旧 worker 池模型（force_legacy_pool=1）
 *   2. engine 1     —— 引擎单实例（Linux/macOS 单 listen；Windows 单 IOCP 消费线程）
 *   3. engine N     —— 引擎多实例（SO_REUSEPORT N 线程；Windows N 个 IOCP 消费线程）
 *   4. auto         —— 实例数自动伸缩（Linux/macOS engine_threads=0；Windows 等价单线程）
 *
 * 用法：bench_web <mode> <scenario> [client_threads] [duration_sec] [engine_threads]
 *   mode      : legacy / engine1 / engineN / auto / all(all=legacy)
 *   scenario  : A 小响应 / B 1MB / C 10MB / D 反代 / E 慢客户端 / F slowloris
 *   client_threads : 压测客户端并发线程数（默认 8）
 *   duration_sec   : 压测时长秒数（默认 5）
 *   engine_threads : engineN 模式的引擎线程数（默认 4）
 *
 * 输出：QPS / P50 / P99 延迟 / 内存 RSS
 * ============================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

/* ---------- 平台兼容层 ---------- */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>
#include <pthread.h>          /* winpthreads（llvm-mingw / mingw-w64 自带） */
#include <unistd.h>           /* mingw 提供 sleep/usleep 声明 */

typedef SOCKET nl_fd_t;
#define NL_BAD_SOCK        INVALID_SOCKET
#define nl_sock_close      closesocket
#define nl_sleep_ms(ms)    Sleep((DWORD)(ms))
#define nl_sleep_sec(s)    Sleep((DWORD)(s) * 1000u)
#else
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/resource.h>

typedef int nl_fd_t;
#define NL_BAD_SOCK        (-1)
#define nl_sock_close      close
#define nl_sleep_ms(ms)    usleep((ms) * 1000)
#define nl_sleep_sec(s)    sleep(s)
#endif

#include "netleaf.h"

/* ---------- 配置 ---------- */
#define BENCH_PORT        18925
#define PROXY_PORT        18926
#define UPSTREAM_PORT     18927
#define WARMUP_SEC        1

#ifdef _WIN32
#define BENCH_TMP_DIR     "."       /* Windows 无 /tmp，测试文件放当前目录 */
#else
#define BENCH_TMP_DIR     "/tmp"
#endif
#define FILE_1MB          BENCH_TMP_DIR "/netleaf_bench_1mb.bin"
#define FILE_10MB         BENCH_TMP_DIR "/netleaf_bench_10mb.bin"

/* ---------- 全局 ---------- */
static volatile int g_stop = 0;
static volatile long g_total_requests = 0;
static volatile long g_total_errors = 0;
static volatile long g_latency_sum_us = 0;
static volatile long g_latency_max_us = 0;
static volatile long g_latency_samples = 0;
/* 第190轮：建连耗时统计（分离"连接建立"与"请求处理"成本） */
static volatile long g_connect_sum_us = 0;
static volatile long g_connect_n = 0;
/* 第191轮：connect 分段计时（socket创建 / 握手select / 收尾setup） */
static volatile long g_c_sock_us = 0;
static volatile long g_c_hs_us = 0;
static volatile long g_c_tail_us = 0;

static pthread_mutex_t g_stat_lock = PTHREAD_MUTEX_INITIALIZER;

/* 运行期参数（命令行可配，避免硬编码） */
static int g_client_threads = 8;
static int g_duration_sec = 5;

/* ---------- 延迟直方图（us） ---------- */
#define LAT_BUCKETS 64
static long g_lat_hist[LAT_BUCKETS];

static int lat_bucket(long us) {
    if (us <= 100) return 0;
    if (us <= 500) return 1;
    if (us <= 1000) return 2;
    if (us <= 2000) return 3;
    if (us <= 5000) return 4;
    if (us <= 10000) return 5;
    if (us <= 50000) return 6;
    if (us <= 100000) return 7;
    return 8;
}

/* ---------- 客户端线程 ---------- */
struct client_arg {
    int thread_id;
    const char* path;
    int slow_read_ms;   /* >0 = 慢客户端模式，每 ms 读一字节 */
};

static long now_us(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int inited = 0;
    LARGE_INTEGER counter;
    if (!inited) { QueryPerformanceFrequency(&freq); inited = 1; }
    QueryPerformanceCounter(&counter);
    return (long)(counter.QuadPart * 1000000 / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
#endif
}

/* socket 层错误字符串（Windows 上 errno 不反映 WSA 错误码） */
static const char* sock_errstr(void) {
#ifdef _WIN32
    static char buf[40];
    snprintf(buf, sizeof(buf), "WSA%d", WSAGetLastError());
    return buf;
#else
    return strerror(errno);
#endif
}

static nl_fd_t connect_server(int port) {
    long t0 = now_us();
    nl_fd_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == NL_BAD_SOCK) {
        fprintf(stderr, "[debug] socket() failed: %s\n", sock_errstr());
        return NL_BAD_SOCK;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /* 第189轮：connect 改非阻塞 + select 2s 超时。
     * 阻塞 connect 在端口耗尽/TIME_WAIT 堆积时会走内核 SYN 重试（Windows 默认
     * 数分钟），极限压测线程全体挂死且不计入 Errors。
     * 第191轮：Windows 同样改非阻塞，但等待用"忙轮询 select（0 超时）"替代
     * 阻塞 select —— 实测阻塞 select 线程唤醒延迟 ~430us（握手段 892us vs
     * 阻塞 connect 462us），忙轮询无唤醒延迟且仍保留 2s 总超时防挂死。 */
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(fd, FIONBIO, &nb);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif
    long t1 = now_us();
    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc != 0) {
#ifdef _WIN32
        int in_progress = (WSAGetLastError() == WSAEWOULDBLOCK);
        if (in_progress) {
            long tdeadline = t1 + 2000000;   /* 2s 总超时（防端口耗尽挂死） */
            rc = -1;
            for (;;) {
                fd_set wset;
                struct timeval tv;
                FD_ZERO(&wset);
                FD_SET(fd, &wset);
                tv.tv_sec = 0;
                tv.tv_usec = 0;
                int r = select((int)fd + 1, NULL, &wset, NULL, &tv);
                if (r > 0) {
                    int err = 0;
                    socklen_t elen = sizeof(err);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
                    rc = (err == 0) ? 0 : -1;
                    break;
                }
                if (r < 0) break;            /* 出错：rc 保持 -1 */
                if (now_us() > tdeadline) break;  /* 超时：rc 保持 -1 */
                Sleep(0);   /* 让出时间片：纯忙轮询在 t=256 抢核拖垮整体
                             * （实测 999 vs 1773 QPS），握手等待期让出更优 */
            }
        }
#else
        int in_progress = (errno == EINPROGRESS);
        if (in_progress) {
            fd_set wset;
            struct timeval tv;
            FD_ZERO(&wset);
            FD_SET(fd, &wset);
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            rc = select((int)fd + 1, NULL, &wset, NULL, &tv);
            if (rc > 0) {
                int err = 0;
                socklen_t elen = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&err, &elen);
                rc = (err == 0) ? 0 : -1;
            } else {
                rc = -1;   /* 超时/出错 */
            }
        } else {
            rc = -1;
        }
#endif
    }
    if (rc != 0) {
        fprintf(stderr, "[debug] connect() failed: %s\n", sock_errstr());
        nl_sock_close(fd);
        return NL_BAD_SOCK;
    }
    long t2 = now_us();
    /* 恢复阻塞 */
#ifdef _WIN32
    u_long bl = 0;
    ioctlsocket(fd, FIONBIO, &bl);
#else
    if (fl >= 0) fcntl(fd, F_SETFL, fl);
#endif
    /* 设置接收超时 2s */
#ifdef _WIN32
    DWORD tv = 2000;   /* Windows SO_RCVTIMEO 收 DWORD 毫秒 */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (struct timeval*)&tv, sizeof(tv));
#endif
    long t3 = now_us();
    __atomic_add_fetch(&g_c_sock_us, t1 - t0, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_c_hs_us,   t2 - t1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_c_tail_us, t3 - t2, __ATOMIC_RELAXED);
    return fd;
}

/* 发送一次 HTTP GET 并读完整响应 */
static int http_get_once(nl_fd_t fd, const char* path, int slow_read_ms, long* latency_us) {
    char req[1024];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: keep-alive\r\n"
        "\r\n", path);

    long t0 = now_us();

    /* 发送请求 */
    int sent = 0;
    while (sent < req_len) {
        int w = send(fd, req + sent, req_len - sent, 0);
        if (w <= 0) {
            fprintf(stderr, "[debug] send() failed (sent=%d/%d): %s\n",
                    sent, req_len, sock_errstr());
            return -1;
        }
        sent += w;
    }

    /* 读响应头 */
    char buf[8192];
    int total_read = 0;
    int header_done = 0;
    long content_length = -1;

    while (!header_done || total_read < content_length) {
        int to_read = slow_read_ms > 0 ? 1 : (int)sizeof(buf);
        /* 慢读模式：累积读入 buf；正常模式：每次复用 buf 起始位置 */
        char* dst = slow_read_ms > 0 ? buf + total_read : buf;
        if (slow_read_ms > 0 && total_read + to_read > (int)sizeof(buf)) break;
        int r = recv(fd, dst, to_read, 0);
        if (r <= 0) {
            fprintf(stderr, "[debug] recv() failed (r=%d, err=%s, read=%d, cl=%ld)\n",
                    r, sock_errstr(), total_read, content_length);
            return -1;
        }
        total_read += r;

        if (!header_done) {
            /* 检查 \r\n\r\n */
            for (int i = 0; i < total_read - 3; i++) {
                if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
                    header_done = 1;
                    /* 解析 Content-Length */
                    char* cl = strstr(buf, "Content-Length:");
                    if (cl) content_length = atol(cl + 15);
                    break;
                }
            }
        }

        if (slow_read_ms > 0) nl_sleep_ms(slow_read_ms);
    }

    *latency_us = now_us() - t0;
    return 0;
}

static void* client_thread(void* arg) {
    struct client_arg* ca = (struct client_arg*)arg;

    /* 短连接模式：每次请求建立新连接（引擎使用 Connection: close） */
    long req_count = 0, err_count = 0;
    long lat_sum = 0, lat_max = 0, lat_n = 0;
    long lat_hist[LAT_BUCKETS] = {0};
    long conn_sum = 0, conn_n = 0;

    while (!g_stop) {
        long tc0 = now_us();
        nl_fd_t fd = connect_server(BENCH_PORT);
        if (fd == NL_BAD_SOCK) {
            err_count++;
            nl_sleep_ms(1);  /* 1ms 退避 */
            continue;
        }
        conn_sum += now_us() - tc0;
        conn_n++;

        long lat = 0;
        if (http_get_once(fd, ca->path, ca->slow_read_ms, &lat) == 0) {
            req_count++;
            lat_sum += lat;
            lat_n++;
            if (lat > lat_max) lat_max = lat;
            int b = lat_bucket(lat);
            lat_hist[b]++;
        } else {
            err_count++;
        }
        nl_sock_close(fd);
    }

    /* 汇总 */
    pthread_mutex_lock(&g_stat_lock);
    g_total_requests += req_count;
    g_total_errors += err_count;
    g_latency_sum_us += lat_sum;
    if (lat_max > g_latency_max_us) g_latency_max_us = lat_max;
    g_latency_samples += lat_n;
    g_connect_sum_us += conn_sum;
    g_connect_n += conn_n;
    for (int b = 0; b < LAT_BUCKETS; b++) g_lat_hist[b] += lat_hist[b];
    pthread_mutex_unlock(&g_stat_lock);
    return NULL;
}

/* ---------- 上游回显服务器（PROXY 场景用） ---------- */
static void* upstream_thread(void* arg) {
    int port = *(int*)arg;
    nl_fd_t listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == NL_BAD_SOCK) return NULL;
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);

    while (!g_stop) {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        if (select((int)listen_fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        nl_fd_t cfd = accept(listen_fd, NULL, NULL);
        if (cfd == NL_BAD_SOCK) continue;

        /* 简单回显 */
        char buf[4096];
        int r = recv(cfd, buf, sizeof(buf), 0);
        if (r > 0) {
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
            send(cfd, resp, 39, 0);
        }
        nl_sock_close(cfd);
    }
    nl_sock_close(listen_fd);
    return NULL;
}

/* ---------- RSS 读取 ---------- */
static long get_rss_kb(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (long)(pmc.PeakWorkingSetSize / 1024);
    return 0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;  /* Linux: KB */
#endif
}

/* ---------- 压测主流程 ---------- */
static void run_bench(const char* label, const char* path, int slow_read_ms) {
    printf("\n========== %s ==========\n", label);
    printf("Path: %s, Threads: %d, Duration: %ds\n",
           path, g_client_threads, g_duration_sec);

    /* 重置统计 */
    g_stop = 0;
    g_total_requests = 0;
    g_total_errors = 0;
    g_latency_sum_us = 0;
    g_latency_max_us = 0;
    g_latency_samples = 0;
    g_connect_sum_us = 0;
    g_connect_n = 0;
    g_c_sock_us = 0;
    g_c_hs_us = 0;
    g_c_tail_us = 0;
    memset(g_lat_hist, 0, sizeof(g_lat_hist));

    long rss_before = get_rss_kb();

    /* 预热 */
    nl_sleep_sec(WARMUP_SEC);

    /* 启动客户端线程 */
    pthread_t* tids = (pthread_t*)malloc(sizeof(pthread_t) * (size_t)g_client_threads);
    struct client_arg* args = (struct client_arg*)malloc(
        sizeof(struct client_arg) * (size_t)g_client_threads);
    if (!tids || !args) {
        fprintf(stderr, "malloc failed for client threads\n");
        free(tids);
        free(args);
        return;
    }
    for (int i = 0; i < g_client_threads; i++) {
        args[i].thread_id = i;
        args[i].path = path;
        args[i].slow_read_ms = slow_read_ms;
        pthread_create(&tids[i], NULL, client_thread, &args[i]);
    }

    /* 跑 duration */
    nl_sleep_sec(g_duration_sec);
    g_stop = 1;

    for (int i = 0; i < g_client_threads; i++) pthread_join(tids[i], NULL);
    free(tids);
    free(args);

    long rss_after = get_rss_kb();

    /* 输出结果 */
    double qps = (double)g_total_requests / g_duration_sec;
    double avg_lat = g_latency_samples > 0 ?
        (double)g_latency_sum_us / g_latency_samples : 0;

    printf("QPS:        %.0f\n", qps);
    printf("Errors:     %ld (%.2f%%)\n", g_total_errors,
           g_total_requests + g_total_errors > 0 ?
           100.0 * g_total_errors / (g_total_requests + g_total_errors) : 0);
    printf("Avg Latency: %.0f us\n", avg_lat);
    printf("Max Latency: %ld us\n", g_latency_max_us);
    printf("RSS:        %ld KB (+%ld KB)\n", rss_after, rss_after - rss_before);

    /* P50/P99 */
    long p50_us = 0, p99_us = 0;
    long target50 = g_latency_samples / 2;
    long target99 = g_latency_samples * 99 / 100;
    long cum = 0;
    const long bucket_ranges_us[] = {100, 500, 1000, 2000, 5000, 10000, 50000, 100000, 999999999};
    for (int b = 0; b < LAT_BUCKETS; b++) {
        cum += g_lat_hist[b];
        if (p50_us == 0 && cum >= target50) p50_us = bucket_ranges_us[b];
        if (p99_us == 0 && cum >= target99) p99_us = bucket_ranges_us[b];
    }
    printf("P50:        %ld us\n", p50_us);
    printf("P99:        %ld us\n", p99_us);
    /* 建连成本：connect 成功耗时均值（socket 创建→握手完成） */
    printf("Avg Connect: %.0f us (n=%ld)\n",
           g_connect_n > 0 ? (double)g_connect_sum_us / g_connect_n : 0.0,
           g_connect_n);
    if (g_connect_n > 0) {
        printf("  connect 分段: socket=%.0f hs=%.0f tail=%.0f us\n",
               (double)g_c_sock_us / g_connect_n,
               (double)g_c_hs_us / g_connect_n,
               (double)g_c_tail_us / g_connect_n);
    }
}

/* ---------- 测试文件生成 ---------- */
static void gen_test_files(void) {
    /* 1MB */
    FILE* f = fopen(FILE_1MB, "wb");
    if (f) {
        char buf[4096];
        memset(buf, 'A', sizeof(buf));
        for (int i = 0; i < 256; i++) fwrite(buf, 1, sizeof(buf), f);
        fclose(f);
    }
    /* 10MB */
    f = fopen(FILE_10MB, "wb");
    if (f) {
        char buf[4096];
        memset(buf, 'B', sizeof(buf));
        for (int i = 0; i < 2560; i++) fwrite(buf, 1, sizeof(buf), f);
        fclose(f);
    }
}

/* ---------- 主 ---------- */
int main(int argc, char* argv[]) {
    const char* mode = argc > 1 ? argv[1] : "all";  /* legacy / engine1 / engineN / auto / all */
    const char* scenario = argc > 2 ? argv[2] : "A";  /* A / B / C / D / E / F */
    if (argc > 3) g_client_threads = atoi(argv[3]);
    if (argc > 4) g_duration_sec = atoi(argv[4]);
    int engine_threads = argc > 5 ? atoi(argv[5]) : 4;
    if (g_client_threads < 1) g_client_threads = 1;
    if (g_client_threads > 4096) g_client_threads = 4096;
    if (g_duration_sec < 1) g_duration_sec = 1;
    if (engine_threads < 1) engine_threads = 1;

    /* 使用无缓冲输出，避免管道/重定向时输出丢失 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            fprintf(stderr, "WSAStartup failed\n");
            return 1;
        }
    }
    signal(SIGINT, SIG_DFL);   /* Windows 无 SIGPIPE */
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    printf("NetLeaf Web Engine Benchmark (P8)\n");
    printf("Mode: %s, Scenario: %s\n", mode, scenario);
    printf("Duration: %ds, Threads: %d\n", g_duration_sec, g_client_threads);

    gen_test_files();

    /* 启动 upstream（PROXY 场景） */
    pthread_t upstream_tid;
    int upstream_port = UPSTREAM_PORT;
    if (scenario[0] == 'D') {
        pthread_create(&upstream_tid, NULL, upstream_thread, &upstream_port);
        nl_sleep_ms(100);
    }

    /* 启动 Web 服务器（按模式） */
    fprintf(stderr, "[debug] Creating server on port %d\n", BENCH_PORT);
    nl_web_server_t* server = nl_web_create(BENCH_PORT);
    if (!server) {
        fprintf(stderr, "Failed to create server on port %d\n", BENCH_PORT);
        return 1;
    }
    fprintf(stderr, "[debug] Server created\n");

    if (strcmp(mode, "legacy") == 0 || strcmp(mode, "all") == 0) {
        nl_web_force_legacy_pool(server, 1);
        fprintf(stderr, "[config] force_legacy_pool = 1\n");
    } else if (strcmp(mode, "auto") == 0) {
        nl_web_set_engine_threads(server, 0);   /* 0 = 自动伸缩 */
        fprintf(stderr, "[config] engine_threads = auto\n");
    } else if (strcmp(mode, "engine1") == 0) {
        nl_web_set_engine_threads(server, 1);
        fprintf(stderr, "[config] engine_threads = 1\n");
    } else {
        nl_web_set_engine_threads(server, engine_threads);
        fprintf(stderr, "[config] engine_threads = %d\n", engine_threads);
    }

    /* 添加测试路由 */
    nl_web_add_json(server, "/small",
        "{\"msg\":\"hello world\",\"id\":12345,\"status\":\"ok\"}");

    if (scenario[0] == 'B') {
        nl_web_add_html_file(server, "/file", FILE_1MB);
    } else if (scenario[0] == 'C') {
        nl_web_add_html_file(server, "/file", FILE_10MB);
    }

    if (scenario[0] == 'D') {
        char proxy_target[64];
        snprintf(proxy_target, sizeof(proxy_target), "http://127.0.0.1:%d", UPSTREAM_PORT);
        nl_web_add_proxy(server, "/proxy", proxy_target);
    }

    /* 启动服务器 */
    if (nl_web_start(server) != 0) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }
    printf("Server started on port %d\n", BENCH_PORT);
    nl_sleep_ms(100);

    /* 选择压测路径 */
    const char* path = "/small";
    int slow_ms = 0;
    if (scenario[0] == 'B' || scenario[0] == 'C') path = "/file";
    else if (scenario[0] == 'D') path = "/proxy";
    else if (scenario[0] == 'E') { path = "/small"; slow_ms = 10; }  /* 慢读 10ms/byte */
    /* F: slowloris 场景（简化：用 E 的慢读模拟） */

    char label[128];
    snprintf(label, sizeof(label), "[%s] Scenario %c", mode, scenario[0]);
    run_bench(label, path, slow_ms);

    /* 清理 */
    nl_web_stop(server);
    nl_web_destroy(server);

    if (scenario[0] == 'D') {
        g_stop = 1;
        pthread_join(upstream_tid, NULL);
    }

    printf("\nBenchmark complete.\n");
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
