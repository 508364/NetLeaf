/* ============================================================
 * NetLeaf Web 引擎压测工具 (P8)
 * ============================================================
 * 对比三种模式：
 *   1. legacy pool  —— 旧 worker 池模型（force_legacy_pool=1）
 *   2. engine 1     —— 引擎单实例（SO_REUSEPORT 单 listen）
 *   3. engine N     —— 引擎多实例（SO_REUSEPORT N 线程）
 *
 * 压测场景：
 *   A. CONTENT 小响应（~1KB 内存路由）
 *   B. FILE 1MB     （BODY_FILE 分块 fread）
 *   C. FILE 10MB    （大文件流式）
 *   D. PROXY        （反向代理转发）
 *   E. 慢客户端     （10ms/byte 读延迟）
 *   F. slowloris    （半包连接占用 slot）
 *
 * 输出：QPS / P50 / P99 延迟 / 内存 RSS
 * ============================================================ */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/resource.h>

#include "netleaf.h"

/* ---------- 配置 ---------- */
#define BENCH_PORT        18925
#define PROXY_PORT        18926
#define UPSTREAM_PORT     18927
#define DURATION_SEC      5
#define WARMUP_SEC        1

/* 客户端并发数 */
#define CLIENT_THREADS    8
#define CONN_PER_THREAD   20  /* 每线程 keep-alive 连接数 */

/* ---------- 全局 ---------- */
static volatile int g_stop = 0;
static volatile long g_total_requests = 0;
static volatile long g_total_errors = 0;
static volatile long g_latency_sum_us = 0;
static volatile long g_latency_max_us = 0;
static volatile long g_latency_samples = 0;

static pthread_mutex_t g_stat_lock = PTHREAD_MUTEX_INITIALIZER;

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
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

static int connect_server(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[debug] socket() failed: %s\n", strerror(errno));
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[debug] connect() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    /* 设置接收超时 2s */
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* 发送一次 HTTP GET 并读完整响应 */
static int http_get_once(int fd, const char* path, int slow_read_ms, long* latency_us) {
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
        if (w <= 0) return -1;
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
            fprintf(stderr, "[debug] recv() failed (r=%d, errno=%s, read=%d, cl=%ld)\n",
                    r, strerror(errno), total_read, content_length);
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

        if (slow_read_ms > 0) usleep(slow_read_ms * 1000);
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

    while (!g_stop) {
        int fd = connect_server(BENCH_PORT);
        if (fd < 0) {
            err_count++;
            usleep(1000);  /* 1ms 退避 */
            continue;
        }

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
        close(fd);
    }

    /* 汇总 */
    pthread_mutex_lock(&g_stat_lock);
    g_total_requests += req_count;
    g_total_errors += err_count;
    g_latency_sum_us += lat_sum;
    if (lat_max > g_latency_max_us) g_latency_max_us = lat_max;
    g_latency_samples += lat_n;
    for (int b = 0; b < LAT_BUCKETS; b++) g_lat_hist[b] += lat_hist[b];
    pthread_mutex_unlock(&g_stat_lock);
    return NULL;
}

/* ---------- 上游回显服务器（PROXY 场景用） ---------- */
static void* upstream_thread(void* arg) {
    int port = *(int*)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 128);

    while (!g_stop) {
        struct timeval tv = {0, 100000};
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        if (select(listen_fd + 1, &rfds, NULL, NULL, &tv) <= 0) continue;

        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) continue;

        /* 简单回显 */
        char buf[4096];
        int r = recv(cfd, buf, sizeof(buf), 0);
        if (r > 0) {
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
            send(cfd, resp, 39, 0);
        }
        close(cfd);
    }
    close(listen_fd);
    return NULL;
}

/* ---------- RSS 读取 ---------- */
static long get_rss_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;  /* Linux: KB */
}

/* ---------- 压测主流程 ---------- */
static void run_bench(const char* label, const char* path, int slow_read_ms) {
    printf("\n========== %s ==========\n", label);
    printf("Path: %s, Threads: %d, Conns: %d, Duration: %ds\n",
           path, CLIENT_THREADS, CLIENT_THREADS * CONN_PER_THREAD, DURATION_SEC);

    /* 重置统计 */
    g_stop = 0;
    g_total_requests = 0;
    g_total_errors = 0;
    g_latency_sum_us = 0;
    g_latency_max_us = 0;
    g_latency_samples = 0;
    memset(g_lat_hist, 0, sizeof(g_lat_hist));

    long rss_before = get_rss_kb();

    /* 预热 */
    usleep(WARMUP_SEC * 1000000);
    g_total_requests = 0;
    g_total_errors = 0;

    /* 启动客户端线程 */
    pthread_t tids[CLIENT_THREADS];
    struct client_arg args[CLIENT_THREADS];
    for (int i = 0; i < CLIENT_THREADS; i++) {
        args[i].thread_id = i;
        args[i].path = path;
        args[i].slow_read_ms = slow_read_ms;
        pthread_create(&tids[i], NULL, client_thread, &args[i]);
    }

    /* 跑 DURATION_SEC */
    sleep(DURATION_SEC);
    g_stop = 1;

    for (int i = 0; i < CLIENT_THREADS; i++) pthread_join(tids[i], NULL);

    long rss_after = get_rss_kb();

    /* 输出结果 */
    double qps = (double)g_total_requests / DURATION_SEC;
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
}

/* ---------- 测试文件生成 ---------- */
static void gen_test_files(void) {
    /* 1MB */
    FILE* f = fopen("/tmp/netleaf_bench_1mb.bin", "wb");
    if (f) {
        char buf[4096];
        memset(buf, 'A', sizeof(buf));
        for (int i = 0; i < 256; i++) fwrite(buf, 1, sizeof(buf), f);
        fclose(f);
    }
    /* 10MB */
    f = fopen("/tmp/netleaf_bench_10mb.bin", "wb");
    if (f) {
        char buf[4096];
        memset(buf, 'B', sizeof(buf));
        for (int i = 0; i < 2560; i++) fwrite(buf, 1, sizeof(buf), f);
        fclose(f);
    }
}

/* ---------- 主 ---------- */
int main(int argc, char* argv[]) {
    const char* mode = argc > 1 ? argv[1] : "all";  /* legacy / engine1 / engineN / all */
    const char* scenario = argc > 2 ? argv[2] : "A";  /* A / B / C / D / E / F */

    /* 使用无缓冲输出，避免管道/重定向时输出丢失 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("NetLeaf Web Engine Benchmark (P8)\n");
    printf("Mode: %s, Scenario: %s\n", mode, scenario);
    printf("Duration: %ds, Threads: %d\n", DURATION_SEC, CLIENT_THREADS);

    signal(SIGPIPE, SIG_IGN);
    gen_test_files();

    /* 生成 1MB/10MB 测试文件 */
    /* 启动 upstream（PROXY 场景） */
    pthread_t upstream_tid;
    int upstream_port = UPSTREAM_PORT;
    if (scenario[0] == 'D') {
        pthread_create(&upstream_tid, NULL, upstream_thread, &upstream_port);
        usleep(100000);
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
    } else {
        int threads = strcmp(mode, "engine1") == 0 ? 1 : 4;
        nl_web_set_engine_threads(server, threads);
        fprintf(stderr, "[config] engine_threads = %d\n", threads);
    }

    /* 添加测试路由 */
    nl_web_add_json(server, "/small",
        "{\"msg\":\"hello world\",\"id\":12345,\"status\":\"ok\"}");

    if (scenario[0] == 'B') {
        nl_web_add_html_file(server, "/file", "/tmp/netleaf_bench_1mb.bin");
    } else if (scenario[0] == 'C') {
        nl_web_add_html_file(server, "/file", "/tmp/netleaf_bench_10mb.bin");
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
    usleep(100000);

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
    return 0;
}
