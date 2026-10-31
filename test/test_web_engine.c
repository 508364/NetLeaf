/*
 * test_web_engine.c - Web 引擎集成测试（P12 测试体系）
 *
 * 覆盖点（每用例独立起服/停服，端口 18881 起递增）：
 *   a. CONTENT 路由：状态码 / Content-Type / body
 *   b. FILE 路由：1MB 文件 Content-Length 与实际 body 字节数一致（流式/零拷贝正确性）
 *   c. REDIRECT 路由：302 + Location 头
 *   d. PROXY 路由：经回环上游（第二个引擎实例）转发拿到上游 body
 *   e. HEAD 请求：有 Content-Length 头但无 body
 *   f. 404：未注册路径
 *   g. 连续独立请求：同一服务器连续 3 次请求全部成功
 *   h. slowloris：半行请求 20 连接 + idle_timeout=2000ms，2.6s 后正常请求仍成功
 *     （服务器未被拖死，空闲槽位已回收）
 *   i. 客户端提前断开：发完完整请求立刻 close，随后正常请求仍成功
 *
 * 构建：随 test/CMakeLists.txt 注册（WebEngineTest）。
 * 跨平台：POSIX socket + Windows winsock（netleaf.h 内部已含 winsock2.h）。
 */

#ifndef _WIN32
#define _GNU_SOURCE   /* usleep 等 POSIX 扩展 */
#endif

#include "netleaf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
    static void nl_sleep_ms(int ms) { Sleep((DWORD)ms); }
    /* nl_sock_t 由 netleaf.h 定义（_WIN32 下 = SOCKET） */
    #define NL_SOCK_INVALID INVALID_SOCKET
    #define nl_sock_close closesocket
    #define nl_sock_send send
    #define nl_sock_recv recv
#else
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <signal.h>
    #include <strings.h>
    static void nl_sleep_ms(unsigned int ms) { usleep(ms * 1000u); }
    /* nl_sock_t 由 netleaf.h 定义（POSIX 下 = int） */
    #define NL_SOCK_INVALID (-1)
    #define nl_sock_close close
    #define nl_sock_send send
    #define nl_sock_recv recv
#endif

#define BASE_PORT 18881
#define UPSTREAM_PORT 18999   /* PROXY 用回环上游（第二个引擎实例） */

static int g_total = 0;
static int g_fail  = 0;
static int g_port  = BASE_PORT;

static void expect(const char* what, int ok) {
    g_total++;
    if (ok) {
        printf("  [PASS] %s\n", what);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

/* ------------------------------------------------------------------ */
/* 极简阻塞 HTTP 客户端：connect -> send -> recv 到对端关闭或超时       */
/* 返回总收字节数，-1 表示连接失败。recv 设 5s 超时防挂死。            */
/* ------------------------------------------------------------------ */
static int http_exchange(int port, const char* req, char* out, int out_cap) {
    nl_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == NL_SOCK_INVALID) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(0x7F000001);   /* 127.0.0.1 */

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        nl_sock_close(fd);
        return -1;
    }
    /* 接收超时 5s：对端不关连接也不至于挂死测试 */
#ifdef _WIN32
    DWORD rto = 5000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rto, sizeof(rto));
#else
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    if (nl_sock_send(fd, req, (int)strlen(req), 0) <= 0) {
        nl_sock_close(fd);
        return -1;
    }

    int total = 0;
    for (;;) {
        int n = nl_sock_recv(fd, out + total, out_cap - 1 - total, 0);
        if (n <= 0) break;          /* 关闭 / 超时 / 错误：均结束 */
        total += n;
        if (total >= out_cap - 1) break;
    }
    out[total] = '\0';
    nl_sock_close(fd);
    return total;
}

/* 解析响应状态码（失败返回 -1）。 */
static int http_status(const char* resp) {
    int major = 0, minor = 0, code = 0;
    if (sscanf(resp, "HTTP/%d.%d %d", &major, &minor, &code) != 3) return -1;
    return code;
}

/* 定位响应头结束位置（\r\n\r\n 后首字节），失败返回 NULL。 */
static const char* http_body(const char* resp) {
    const char* p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* 大小写不敏感前缀比较（跨平台小助手）。 */
static int _strnicmp_compat(const char* a, const char* b, size_t n);

/* 查找响应头（大小写不敏感前缀匹配），返回值起始指针或 NULL。 */
static const char* http_header_val(const char* resp, const char* name) {
    const char* p = strstr(resp, "\r\n");
    if (!p) return NULL;
    p += 2;
    size_t nlen = strlen(name);
    while (p && *p) {
        if (_strnicmp_compat(p, name, nlen) == 0) return p + nlen;
        const char* eol = strstr(p, "\r\n");
        if (!eol) return NULL;
        if (eol == p) return NULL;   /* 空行=头结束 */
        p = eol + 2;
    }
    return NULL;
}

static int _strnicmp_compat(const char* a, const char* b, size_t n) {
#ifdef _WIN32
    return _strnicmp(a, b, n);
#else
    return strncasecmp(a, b, n);
#endif
}

/* ------------------------------------------------------------------ */
/* 用例辅助：起一个引擎服务器                                         */
/* ------------------------------------------------------------------ */
static nl_web_server_t* engine_start(int port, int engine_threads, int idle_ms) {
    nl_web_server_t* s = nl_web_create(port);
    if (!s) return NULL;
    if (engine_threads > 0) nl_web_set_engine_threads(s, engine_threads);
    if (idle_ms > 0) nl_web_set_idle_timeout(s, idle_ms);
    if (nl_web_start(s) != 0) {
        nl_web_destroy(s);
        return NULL;
    }
    nl_sleep_ms(150);   /* 等监听就绪 */
    return s;
}

static void engine_stop(nl_web_server_t* s) {
    nl_web_stop(s);
    nl_web_destroy(s);
}

/* ------------------------------------------------------------------ */
/* 用例 a：CONTENT 路由                                                */
/* ------------------------------------------------------------------ */
static void case_content(void) {
    printf("\n[用例] a. CONTENT 路由\n");
    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start server", 0); return; }
    nl_web_add_route(s, "/c", "hello-netleaf", "text/plain");
    nl_sleep_ms(50);

    char buf[8192];
    int n = http_exchange(port,
        "GET /c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("收到响应", n > 0);
    expect("状态码 200", n > 0 && http_status(buf) == 200);
    expect("Content-Type: text/plain",
           n > 0 && strstr(buf, "Content-Type: text/plain") != NULL);
    expect("body 匹配", n > 0 && strstr(buf, "hello-netleaf") != NULL);

    engine_stop(s);
}

/* ------------------------------------------------------------------ */
/* 用例 b：FILE 路由（1MB，Content-Length 与实际 body 一致）           */
/* Windows 基线遗留：nl_web_add_html_file 仅 Linux/macOS 实现，        */
/* netleaf_windows.c 从未提供（bench_web 因此亦标记 NOT WIN32），      */
/* 故 FILE/REDIRECT 用例在 Windows 上条件编译跳过。                    */
/* ------------------------------------------------------------------ */
#ifndef _WIN32
#define FILE_BODY_BYTES (1024 * 1024)

static void case_file(void) {
    printf("\n[用例] b. FILE 路由 1MB\n");
    const char* path = "nl_test_web_1mb.tmp";
    FILE* fp = fopen(path, "wb");
    if (!fp) { expect("创建临时文件", 0); return; }
    char block[4096];
    memset(block, 'A', sizeof(block));
    for (int i = 0; i < FILE_BODY_BYTES / (int)sizeof(block); i++)
        fwrite(block, 1, sizeof(block), fp);
    fclose(fp);

    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start server", 0); remove(path); return; }
    nl_web_add_html_file(s, "/f", path);
    nl_sleep_ms(50);

    char* buf = (char*)malloc(FILE_BODY_BYTES + 8192);
    if (!buf) { expect("malloc", 0); engine_stop(s); remove(path); return; }
    int n = http_exchange(port,
        "GET /f HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, FILE_BODY_BYTES + 8192);
    expect("收到响应", n > 0);
    expect("状态码 200", n > 0 && http_status(buf) == 200);

    const char* body = (n > 0) ? http_body(buf) : NULL;
    if (body) {
        long cl = -1;
        const char* clv = http_header_val(buf, "Content-Length:");
        if (clv) cl = strtol(clv, NULL, 10);
        expect("Content-Length == 1MB", cl == FILE_BODY_BYTES);
        expect("body 实际字节数 == 1MB",
               (long)(n - (body - buf)) == FILE_BODY_BYTES);
    } else {
        expect("解析响应头", 0);
    }
    free(buf);
    engine_stop(s);
    remove(path);
}

/* ------------------------------------------------------------------ */
/* 用例 c：REDIRECT 302                                                */
/* ------------------------------------------------------------------ */
static void case_redirect(void) {
    printf("\n[用例] c. REDIRECT 302\n");
    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start server", 0); return; }
    nl_web_add_redirect_302(s, "/r", "http://example.com/new");
    nl_sleep_ms(50);

    char buf[4096];
    int n = http_exchange(port,
        "GET /r HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("收到响应", n > 0);
    expect("状态码 302", n > 0 && http_status(buf) == 302);
    expect("Location 头",
           n > 0 && strstr(buf, "Location: http://example.com/new") != NULL);

    engine_stop(s);
}
#else /* _WIN32：API 未实现，提供空用例保持调用点一致 */
static void case_file(void)    { printf("\n[用例] b. FILE 路由 1MB [SKIP: Windows 未实现 nl_web_add_html_file]\n"); }
static void case_redirect(void){ printf("\n[用例] c. REDIRECT 302 [SKIP: Windows 未实现 nl_web_add_redirect_302]\n"); }
#endif

/* ------------------------------------------------------------------ */
/* 用例 d：PROXY（回环上游 = 第二个引擎实例的 CONTENT 路由）           */
/* ------------------------------------------------------------------ */
static void case_proxy(void) {
    printf("\n[用例] d. PROXY 经回环上游\n");
    /* 先起上游（简易 CONTENT 服务器）。引擎反代为原路径转发：
     * GET /p 被原样发给上游，故上游需注册同名路径。 */
    nl_web_server_t* up = engine_start(UPSTREAM_PORT, 0, 0);
    if (!up) { expect("start upstream", 0); return; }
    nl_web_add_route(up, "/p", "UPSTREAM-OK", "text/plain");
    nl_sleep_ms(50);

    int port = g_port++;
    char target[64];
    snprintf(target, sizeof(target), "http://127.0.0.1:%d", UPSTREAM_PORT);
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start proxy server", 0); engine_stop(up); return; }
    nl_web_add_proxy(s, "/p", target);
    nl_sleep_ms(50);

    char buf[8192];
    int n = http_exchange(port,
        "GET /p HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("收到响应", n > 0);
    expect("状态码 200", n > 0 && http_status(buf) == 200);
    expect("转发 body 来自上游", n > 0 && strstr(buf, "UPSTREAM-OK") != NULL);

    engine_stop(s);
    engine_stop(up);
}

/* ------------------------------------------------------------------ */
/* 用例 e：HEAD 无 body                                                */
/* ------------------------------------------------------------------ */
static void case_head(void) {
    printf("\n[用例] e. HEAD 无 body\n");
    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start server", 0); return; }
    nl_web_add_route(s, "/c", "hello-netleaf", "text/plain");
    nl_sleep_ms(50);

    char buf[4096];
    int n = http_exchange(port,
        "HEAD /c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("收到响应", n > 0);
    expect("状态码 200", n > 0 && http_status(buf) == 200);
    /* RFC 7231：HEAD 响应头应与 GET 相同，含 Content-Length，只是无 body */
    const char* clv = (n > 0) ? http_header_val(buf, "Content-Length:") : NULL;
    expect("带 Content-Length 头", clv && strtol(clv, NULL, 10) == 13);
    const char* body = (n > 0) ? http_body(buf) : NULL;
    expect("无响应 body", body && *body == '\0');

    engine_stop(s);
}

/* ------------------------------------------------------------------ */
/* 用例 f：404                                                          */
/* ------------------------------------------------------------------ */
static void case_404(void) {
    printf("\n[用例] f. 404 未注册路径\n");
    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start server", 0); return; }
    nl_web_add_route(s, "/c", "x", "text/plain");
    nl_sleep_ms(50);

    char buf[4096];
    int n = http_exchange(port,
        "GET /no-such-path HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("收到响应", n > 0);
    expect("状态码 404", n > 0 && http_status(buf) == 404);

    engine_stop(s);
}

/* ------------------------------------------------------------------ */
/* 用例 g：连续 3 次独立请求全部成功                                   */
/* ------------------------------------------------------------------ */
static void case_sequential(void) {
    printf("\n[用例] g. 连续独立请求\n");
    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 2, 0);   /* 多实例模式（P6，TSan 竞争观测点） */
    if (!s) { expect("start server", 0); return; }
    nl_web_add_route(s, "/c", "seq-ok", "text/plain");
    nl_sleep_ms(50);

    char buf[4096];
    int ok3 = 1;
    for (int i = 0; i < 3; i++) {
        int n = http_exchange(port,
            "GET /c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
            buf, sizeof(buf));
        if (n <= 0 || http_status(buf) != 200 || !strstr(buf, "seq-ok")) {
            ok3 = 0;
            printf("    第 %d 次请求失败\n", i + 1);
        }
    }
    expect("3 次请求全部 200", ok3);

    engine_stop(s);
}

/* ------------------------------------------------------------------ */
/* 用例 h：slowloris 半行请求 + idle_timeout=2000ms 回收              */
/* ------------------------------------------------------------------ */
static void case_slowloris(void) {
    printf("\n[用例] h. slowloris 空闲回收\n");
    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 2000);
    if (!s) { expect("start server", 0); return; }
    nl_web_add_route(s, "/c", "alive", "text/plain");
    nl_sleep_ms(50);

    /* 20 个连接各发半行请求头（无 \r\n\r\n），永远不发完 */
    nl_sock_t socks[20];
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(0x7F000001);
    int opened = 0;
    for (int i = 0; i < 20; i++) {
        socks[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (socks[i] == NL_SOCK_INVALID) continue;
        if (connect(socks[i], (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            nl_sock_close(socks[i]);
            continue;
        }
        const char* partial = "GET /c HTTP/1.1\r\nX-Slow: ";
        nl_sock_send(socks[i], partial, (int)strlen(partial), 0);
        opened++;
    }
    expect("20 个半开连接建立", opened == 20);

    /* 等 idle_timeout(2s) + 扫描节流(~0.5s) + 余量 */
    nl_sleep_ms(3100);

    /* 服务器应仍可正常服务（半开槽位已被回收，未拖死引擎） */
    char buf[4096];
    int n = http_exchange(port,
        "GET /c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("超时后正常请求仍成功", n > 0 && http_status(buf) == 200);

    for (int i = 0; i < 20; i++)
        if (socks[i] != NL_SOCK_INVALID) nl_sock_close(socks[i]);
    engine_stop(s);
}

/* ------------------------------------------------------------------ */
/* 用例 i：客户端提前断开                                             */
/* ------------------------------------------------------------------ */
static void case_abort(void) {
    printf("\n[用例] i. 客户端提前断开\n");
    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start server", 0); return; }
    nl_web_add_route(s, "/c", "still-alive", "text/plain");
    nl_sleep_ms(50);

    /* 发完整请求后立刻关闭（不读响应） */
    nl_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd != NL_SOCK_INVALID) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)port);
        addr.sin_addr.s_addr = htonl(0x7F000001);
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            const char* req = "GET /c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
            nl_sock_send(fd, req, (int)strlen(req), 0);
        }
        nl_sock_close(fd);
    }
    nl_sleep_ms(200);

    char buf[4096];
    int n = http_exchange(port,
        "GET /c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("断开后正常请求仍成功", n > 0 && http_status(buf) == 200
           && strstr(buf, "still-alive") != NULL);

    engine_stop(s);
}

int main(void) {
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    printf("NetLeaf Web Engine Integration Test (P12)\n");

    case_content();
    case_file();
    case_redirect();
    case_proxy();
    case_head();
    case_404();
    case_sequential();
    case_slowloris();
    case_abort();

    printf("\n==== 汇总: %d 项, 通过 %d, 失败 %d ====\n",
           g_total, g_total - g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
