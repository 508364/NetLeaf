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
 *   j. WS 升级隧道反代：101 切换 + 双向裸字节回显 + 头透传
 *   k. PROXY 目录转发（第195轮）：nginx location 语义——透传 / 最长前缀 /
 *      精确优先 / proxy_pass URI 拼接 / query 保留 / 前缀边界
 *
 * 构建：随 test/CMakeLists.txt 注册（WebEngineTest）。
 * 跨平台：POSIX socket + Windows winsock（netleaf.h 内部已含 winsock2.h）。
 */

#ifndef _WIN32
#define _GNU_SOURCE   /* usleep 等 POSIX 扩展 */
#endif

#include "netleaf.h"
/* 第195轮：目录转发的公共层纯逻辑（前缀匹配 / 路径改写）直测 */
#include "../src/common/nl_proxy_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>   /* WS 反代用例的上游线程 */
    #include <direct.h>    /* 第203轮：_mkdir 建目录端点测试目录 */
    static void nl_sleep_ms(int ms) { Sleep((DWORD)ms); }
    /* nl_sock_t 由 netleaf.h 定义（_WIN32 下 = SOCKET） */
    #define NL_SOCK_INVALID INVALID_SOCKET
    #define nl_sock_close closesocket
    #define nl_sock_send send
    #define nl_sock_recv recv
    #define nl_test_mkdir(p) _mkdir(p)
    #define nl_test_rmdir(p) _rmdir(p)
#else
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <signal.h>
    #include <strings.h>
    #include <pthread.h>   /* WS 反代用例的上游线程 */
    #include <sys/stat.h>  /* 第203轮：mkdir 建目录端点测试目录 */
    static void nl_sleep_ms(unsigned int ms) { usleep(ms * 1000u); }
    /* nl_sock_t 由 netleaf.h 定义（POSIX 下 = int） */
    #define NL_SOCK_INVALID (-1)
    #define nl_sock_close close
    #define nl_sock_send send
    #define nl_sock_recv recv
    #define nl_test_mkdir(p) mkdir(p, 0755)
    #define nl_test_rmdir(p) rmdir(p)
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
/* 用例 f2：RFC 3986 端点规范化 + 路径穿越拒绝（第192轮）              */
/* ------------------------------------------------------------------ */
static void case_uri(void) {
    printf("\n[用例] f2. RFC 3986 端点规范化 / 路径穿越\n");
    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start server", 0); return; }
    nl_web_add_route(s, "/c", "uri-ok", "text/plain");
    nl_sleep_ms(50);

    char buf[4096];

    /* 1) 带 query 的请求命中路由（规范化剥掉 ?query，旧行为 404） */
    int n = http_exchange(port,
        "GET /c?a=1&b=2 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("query 剥离后命中路由(200)", n > 0 && http_status(buf) == 200);
    expect("body 来自路由", n > 0 && strstr(buf, "uri-ok") != NULL);

    /* 2) percent-decode 后命中（%63 = 'c'） */
    n = http_exchange(port,
        "GET /%63 HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("percent-decode 命中路由(200)", n > 0 && http_status(buf) == 200);

    /* 3) dot-segment 归一：/./c 与 /x/../c 均归一到 /c */
    n = http_exchange(port,
        "GET /./c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("dot-segment /./c 归一(200)", n > 0 && http_status(buf) == 200);
    n = http_exchange(port,
        "GET /x/../c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("dot-segment /x/../c 归一(200)", n > 0 && http_status(buf) == 200);

    /* 4) 越根 ../ 一律拒绝（403/404，绝不 200） */
    n = http_exchange(port,
        "GET /../c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("越根 /../c 被拒", n > 0 && http_status(buf) != 200);

    /* 5) 反斜杠端点一律 400（Windows 路径分隔符逃逸面） */
    n = http_exchange(port,
        "GET /..\\c HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
        buf, sizeof(buf));
    expect("反斜杠端点被拒", n > 0 && http_status(buf) != 200);

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

/* ------------------------------------------------------------------ */
/* 用例 j：WS 升级隧道反代（101 → 双向裸字节）                          */
/* 上游为手写 TCP 服务：校验 Upgrade / Sec-WebSocket-Key 是否透传，     */
/* 回 101 后把收到的字节原样回显。同时验证：p2 头透传、p3 隧道状态机。  */
/* ------------------------------------------------------------------ */
#define WS_UPSTREAM_PORT 19002

typedef struct {
    int port;
    int ready;
    int saw_upgrade;
    int saw_key;
} ws_up_ctx_t;

#ifdef _WIN32
static unsigned __stdcall ws_upstream_proc(void* p) {
#else
static void* ws_upstream_proc(void* p) {
#endif
    ws_up_ctx_t* ctx = (ws_up_ctx_t*)p;
    nl_sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == NL_SOCK_INVALID) return 0;
    {
        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)ctx->port);
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 4) != 0) {
        nl_sock_close(ls);
        return 0;
    }
    ctx->ready = 1;
    nl_sock_t c = accept(ls, NULL, NULL);
    if (c != NL_SOCK_INVALID) {
        char req[4096];
        int n = nl_sock_recv(c, req, (int)sizeof(req) - 1, 0);
        if (n > 0) {
            req[n] = '\0';
            ctx->saw_upgrade = strstr(req, "Upgrade: websocket") != NULL;
            ctx->saw_key = strstr(req, "Sec-WebSocket-Key:") != NULL;
            const char* resp =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: test-accept\r\n\r\n";
            nl_sock_send(c, resp, (int)strlen(resp), 0);
            for (;;) {   /* 原样回显，验证隧道双向 */
                char b[512];
                int m = nl_sock_recv(c, b, (int)sizeof(b), 0);
                if (m <= 0) break;
                if (nl_sock_send(c, b, m, 0) <= 0) break;
            }
        }
        nl_sock_close(c);
    }
    nl_sock_close(ls);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void case_ws_proxy(void) {
    printf("\n[用例] j. WS 升级隧道反代\n");
    ws_up_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.port = WS_UPSTREAM_PORT;

#ifdef _WIN32
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, ws_upstream_proc, &ctx, 0, NULL);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, ws_upstream_proc, &ctx) != 0) {
        expect("启动 WS 上游线程", 0);
        return;
    }
#endif
    nl_sleep_ms(200);

    int port = g_port++;
    char target[64];
    snprintf(target, sizeof(target), "http://127.0.0.1:%d", WS_UPSTREAM_PORT);
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start proxy server", 0); return; }
    nl_web_add_proxy(s, "/ws", target);
    nl_sleep_ms(50);

    nl_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(0x7F000001);
    int conn_ok = (fd != NL_SOCK_INVALID &&
                   connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    expect("连接反代服务器", conn_ok);
    if (conn_ok) {
#ifdef _WIN32
        DWORD rto = 5000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rto, sizeof(rto));
#else
        struct timeval tv = {5, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        const char* upreq =
            "GET /ws HTTP/1.1\r\n"
            "Host: t\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        nl_sock_send(fd, upreq, (int)strlen(upreq), 0);

        char resp[4096];
        int total = 0;
        resp[0] = '\0';
        while (total < (int)sizeof(resp) - 1 && !strstr(resp, "\r\n\r\n")) {
            int n = nl_sock_recv(fd, resp + total, (int)sizeof(resp) - 1 - total, 0);
            if (n <= 0) break;
            total += n;
            resp[total] = '\0';
        }
        expect("下游收到 101 Switching Protocols",
               strstr(resp, "101 Switching Protocols") != NULL);
        expect("101 响应头透传 Upgrade",
               strstr(resp, "Upgrade: websocket") != NULL);

        /* 隧道建立后双向裸字节：发什么回什么 */
        const char* ping = "PING-PONG-123";
        nl_sock_send(fd, ping, (int)strlen(ping), 0);
        char echo[512];
        int et = 0;
        echo[0] = '\0';
        while (et < (int)sizeof(echo) - 1 && et < (int)strlen(ping)) {
            int n = nl_sock_recv(fd, echo + et, (int)sizeof(echo) - 1 - et, 0);
            if (n <= 0) break;
            et += n;
            echo[et] = '\0';
        }
        expect("隧道双向回显", strcmp(echo, ping) == 0);
        nl_sock_close(fd);
    }

#ifdef _WIN32
    if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
#else
    pthread_join(th, NULL);
#endif
    expect("上游收到 Upgrade 头（请求头透传）", ctx.saw_upgrade);
    expect("上游收到 Sec-WebSocket-Key", ctx.saw_key);
    engine_stop(s);
}

/* ------------------------------------------------------------------ */
/* 用例 k：PROXY 目录转发（第195轮，nginx location 语义）              */
/* 上游 = 回显请求行的极简 TCP 线程：body 即上游收到的请求行，          */
/* 由此直接断言透传 / 前缀剥离 / URI 拼接 / query 保留 / 优先级。        */
/* ------------------------------------------------------------------ */
#define DIR_UPSTREAM_PORT 19003

typedef struct {
    int port;
    volatile int stop;
    int ready;
} dir_up_ctx_t;

#ifdef _WIN32
static unsigned __stdcall dir_upstream_proc(void* p) {
#else
static void* dir_upstream_proc(void* p) {
#endif
    dir_up_ctx_t* ctx = (dir_up_ctx_t*)p;
    nl_sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == NL_SOCK_INVALID) return 0;
    {
        int yes = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)ctx->port);
    a.sin_addr.s_addr = htonl(0x7F000001);
    if (bind(ls, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(ls, 8) != 0) {
        nl_sock_close(ls);
        return 0;
    }
    ctx->ready = 1;
    while (!ctx->stop) {
        nl_sock_t c = accept(ls, NULL, NULL);
        if (c == NL_SOCK_INVALID) break;
        if (ctx->stop) { nl_sock_close(c); break; }
        char req[4096];
        int n = nl_sock_recv(c, req, (int)sizeof(req) - 1, 0);
        if (n > 0) {
            req[n] = '\0';
            char* eol = strstr(req, "\r\n");
            if (eol) *eol = '\0';            /* 只回显请求行（方法+路径+版本） */
            int bl = (int)strlen(req);
            char resp[4608];
            int hn = snprintf(resp, sizeof(resp),
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                "Content-Length: %d\r\nConnection: close\r\n\r\n", bl);
            if (hn > 0 && hn + bl < (int)sizeof(resp)) {
                memcpy(resp + hn, req, (size_t)bl);
                nl_sock_send(c, resp, hn + bl, 0);
            }
        }
        nl_sock_close(c);   /* Connection: close：每请求一条上游连接 */
    }
    nl_sock_close(ls);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* 发一个 GET 并断言上游回显的请求行（即上游实际收到的请求行）。 */
static void dir_req_expect(int port, const char* path, const char* want_line) {
    char req[512], buf[8192], name[160];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", path);
    int n = http_exchange(port, req, buf, sizeof(buf));
    const char* body = (n > 0) ? http_body(buf) : NULL;
    int ok = (body != NULL && strcmp(body, want_line) == 0);
    snprintf(name, sizeof(name), "上游收到请求行 [%s] == [%s]", path, want_line);
    if (!ok)
        printf("    实得: %s\n", body ? body : "(无响应体)");
    expect(name, ok);
}

static void case_proxy_dir(void) {
    printf("\n[用例] k. PROXY 目录转发（nginx location 语义）\n");

    /* ---- 纯逻辑快检（公共层，无需起服） ---- */
    char rb[256];
    expect("前缀匹配: /api/ 命中 /api/echo",
           nl_px_path_match_prefix("/api/", "/api/echo") == 1);
    expect("前缀匹配: /api/ 不命中 /apifoo",
           nl_px_path_match_prefix("/api/", "/apifoo") == 0);
    expect("前缀匹配: 双方剥 query",
           nl_px_path_match_prefix("/api/", "/api/echo?x=1") == 1);
    expect("改写: 剥前缀+拼URI+保query",
           nl_px_dir_rewrite("/api/", 1, "/v1/", "/api/foo?a=1", rb, sizeof(rb)) > 0 &&
           strcmp(rb, "/v1/foo?a=1") == 0);
    expect("改写: up_uri 空=原样透传",
           nl_px_dir_rewrite("/api/", 1, "", "/api/foo?a=1", rb, sizeof(rb)) > 0 &&
           strcmp(rb, "/api/foo?a=1") == 0);
    expect("改写: 字面前缀不匹配→安全回退",
           nl_px_dir_rewrite("/api/", 1, "/v1/", "/apifoo", rb, sizeof(rb)) > 0 &&
           strcmp(rb, "/apifoo") == 0);

    /* ---- 端到端：回显上游 ---- */
    dir_up_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.port = DIR_UPSTREAM_PORT;
#ifdef _WIN32
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, dir_upstream_proc, &ctx, 0, NULL);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, dir_upstream_proc, &ctx) != 0) {
        expect("启动目录回显上游线程", 0);
        return;
    }
#endif
    for (int i = 0; i < 100 && !ctx.ready; i++) nl_sleep_ms(10);
    if (!ctx.ready) {
        expect("回显上游就绪", 0);
        ctx.stop = 1;
#ifdef _WIN32
        if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
#else
        pthread_join(th, NULL);
#endif
        return;
    }

    char target[80], target_v2[96], target_ex[96];
    snprintf(target, sizeof(target), "http://127.0.0.1:%d", DIR_UPSTREAM_PORT);
    snprintf(target_v2, sizeof(target_v2), "%s/v2/", target);
    snprintf(target_ex, sizeof(target_ex), "%s/exact/", target);

    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) {
        expect("start proxy server", 0);
        ctx.stop = 1;
        nl_sock_t w = socket(AF_INET, SOCK_STREAM, 0);   /* 唤醒 accept 后 join */
        if (w != NL_SOCK_INVALID) nl_sock_close(w);
#ifdef _WIN32
        if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
#else
        pthread_join(th, NULL);
#endif
        return;
    }
    nl_web_add_proxy_dir(s, "/api/", target);            /* 不带 URI：原样透传 */
    nl_web_add_proxy_dir(s, "/api/v2/", target_v2);      /* 带 URI：剥前缀拼接 */
    nl_sleep_ms(50);

    /* 1) 透传：不带 URI 的目录路由原样转发完整路径（含 query） */
    dir_req_expect(port, "/api/echo", "GET /api/echo HTTP/1.1");

    /* 2) 最长前缀 + URI 拼接 + query 保留：/api/v2/x?a=1 → 上游 /v2/x?a=1 */
    dir_req_expect(port, "/api/v2/x?a=1", "GET /v2/x?a=1 HTTP/1.1");

    /* 3) 精确 > 最长前缀：精确路由剥整段拼 URI，/api/v2/x → 上游 /exact/ */
    nl_web_add_proxy(s, "/api/v2/x", target_ex);
    nl_sleep_ms(50);
    dir_req_expect(port, "/api/v2/x", "GET /exact/ HTTP/1.1");

    /* 4) 前缀边界：/api/ 不吞 /apifoo（nginx location 前缀字面语义） */
    {
        char buf[4096];
        int n = http_exchange(port,
            "GET /apifoo HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
            buf, sizeof(buf));
        expect("前缀边界: /api/ 不匹配 /apifoo（404）",
               n > 0 && http_status(buf) == 404);
    }

    engine_stop(s);

    /* 停上游：置位 + 本地 connect 唤醒阻塞中的 accept */
    ctx.stop = 1;
    {
        nl_sock_t w = socket(AF_INET, SOCK_STREAM, 0);
        if (w != NL_SOCK_INVALID) {
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port = htons((unsigned short)DIR_UPSTREAM_PORT);
            a.sin_addr.s_addr = htonl(0x7F000001);
            connect(w, (struct sockaddr*)&a, sizeof(a));
            nl_sock_close(w);
        }
    }
#ifdef _WIN32
    if (th) { WaitForSingleObject(th, 5000); CloseHandle(th); }
#else
    pthread_join(th, NULL);
#endif
}

/* ------------------------------------------------------------------ */
/* 第203轮用例：DIR 端点（端点即根，../ clamp 不逃逸）                 */
/* ------------------------------------------------------------------ */
static void dir_write_text(const char* path, const char* content) {
    FILE* fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
}

/* 发一个请求并返回状态码（连接失败 = -1）。 */
static int dir_get_status(int port, const char* path, char* buf, int buf_cap,
                          const char* method) {
    char req[512];
    snprintf(req, sizeof(req),
             "%s %s HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
             method, path);
    int n = http_exchange(port, req, buf, buf_cap);
    return (n > 0) ? http_status(buf) : -1;
}

static void case_dir_endpoint(void) {
    printf("\n[用例] l. DIR 端点（文件/目录即端点根，../ clamp）\n");

    /* 布置：目录端点根 + 其中文件/子目录 + 端点外同名诱饵文件 */
    nl_test_mkdir("nl_test_web_dir");
    nl_test_mkdir("nl_test_web_dir/sub");
    dir_write_text("nl_test_web_dir/index.html", "INDEX-OK");
    dir_write_text("nl_test_web_dir/page.html", "PAGE-OK");
    dir_write_text("nl_test_web_dir/sub/inner.txt", "INNER-OK");
    dir_write_text("nl_test_single.txt", "SINGLE-OK");
    dir_write_text("nl_test_outside.txt", "OUTSIDE-SECRET");   /* 端点外文件 */

    int port = g_port++;
    nl_web_server_t* s = engine_start(port, 0, 0);
    if (!s) { expect("start server", 0); return; }

    expect("注册目录端点 /d", nl_web_add_dir(s, "/d", "nl_test_web_dir") == 0);
    expect("注册文件端点 /single", nl_web_add_dir(s, "/single", "nl_test_single.txt") == 0);
    /* 对照路由：/dx 不得被 /d 吞掉（前缀边界） */
    expect("注册对照路由 /dx", nl_web_add_route(s, "/dx", "dx-content", "text/plain") == 0);
    /* 非法端点：根 / 与非绝对前缀必须被拒 */
    expect("拒绝根端点 /", nl_web_add_dir(s, "/", "nl_test_web_dir") != 0);
    expect("拒绝非绝对前缀", nl_web_add_dir(s, "bad", "nl_test_web_dir") != 0);
    nl_sleep_ms(50);

    char buf[8192];
    const char* body;

    /* 1) 目录根：/d/ 与 /d 均服务 index.html */
    int st = dir_get_status(port, "/d/", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /d/ → 200 index.html", st == 200 && body && strstr(body, "INDEX-OK"));
    st = dir_get_status(port, "/d", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /d → 200 index.html", st == 200 && body && strstr(body, "INDEX-OK"));
    expect("GET /d/ Content-Type: text/html",
           strstr(buf, "Content-Type: text/html") != NULL);

    /* 2) 端点内普通文件 + 子目录 */
    st = dir_get_status(port, "/d/page.html", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /d/page.html → 200", st == 200 && body && strstr(body, "PAGE-OK"));
    st = dir_get_status(port, "/d/sub/inner.txt", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /d/sub/inner.txt → 200", st == 200 && body && strstr(body, "INNER-OK"));

    /* 3) ../ clamp：端点内上跳等价于端点根解释，不逃逸 */
    st = dir_get_status(port, "/d/../page.html", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /d/../page.html → 端点内 200（clamp）",
           st == 200 && body && strstr(body, "PAGE-OK"));
    st = dir_get_status(port, "/d/../../page.html", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /d/../../page.html → 端点内 200（越根 .. 忽略）",
           st == 200 && body && strstr(body, "PAGE-OK"));

    /* 4) 越界不逃逸：../ 想拿端点外文件，clamp 后仍在端点内 → 404 且无泄露 */
    st = dir_get_status(port, "/d/../nl_test_outside.txt", buf, sizeof(buf), "GET");
    expect("GET /d/../nl_test_outside.txt → 404（不逃逸端点）", st == 404);
    expect("响应体不含端点外内容",
           strstr(buf, "OUTSIDE-SECRET") == NULL);

    /* 5) 文件端点：任意余段均服务该文件 */
    st = dir_get_status(port, "/single", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /single → 200 该文件", st == 200 && body && strstr(body, "SINGLE-OK"));
    st = dir_get_status(port, "/single/anything/deep?x=1", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /single/anything/deep?x=1 → 200 该文件",
           st == 200 && body && strstr(body, "SINGLE-OK"));

    /* 6) 前缀边界：/dx 不被 /d 吞 */
    st = dir_get_status(port, "/dx", buf, sizeof(buf), "GET");
    body = http_body(buf);
    expect("GET /dx → 200 对照路由（不被 /d 吞）",
           st == 200 && body && strstr(body, "dx-content"));

    /* 7) 端点内不存在 → 404 */
    st = dir_get_status(port, "/d/nonexist.txt", buf, sizeof(buf), "GET");
    expect("GET /d/nonexist.txt → 404", st == 404);

    /* 8) HEAD：200 且无响应体 */
    st = dir_get_status(port, "/d/page.html", buf, sizeof(buf), "HEAD");
    body = http_body(buf);
    expect("HEAD /d/page.html → 200 无 body",
           st == 200 && body && body[0] == '\0');

    engine_stop(s);

    /* 清理布置的临时文件/目录 */
    remove("nl_test_web_dir/index.html");
    remove("nl_test_web_dir/page.html");
    remove("nl_test_web_dir/sub/inner.txt");
    nl_test_rmdir("nl_test_web_dir/sub");
    nl_test_rmdir("nl_test_web_dir");
    remove("nl_test_single.txt");
    remove("nl_test_outside.txt");
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
    case_ws_proxy();
    case_proxy_dir();
    case_head();
    case_404();
    case_uri();
    case_sequential();
    case_slowloris();
    case_abort();
    case_dir_endpoint();

    printf("\n==== 汇总: %d 项, 通过 %d, 失败 %d ====\n",
           g_total, g_total - g_fail, g_fail);
    return g_fail == 0 ? 0 : 1;
}
