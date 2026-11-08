/**
 * @file test_linkagg_pull.c
 * @brief 链路聚合反向数据池（宿主主动拉取）回归测试
 *
 * 覆盖：
 *  1. 接入请求进入指定端点的数据池，宿主 pull 后 respond，客户端收到响应；
 *  2. 宿主不拉取时，超时重试一次后返回 503，并把端点标记为异常；
 *  3. 宿主再次拉取并响应后端点恢复健康。
 */
#include "netleaf_linkagg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    typedef SOCKET test_sock_t;
    #define TEST_SOCK_INVALID INVALID_SOCKET
    #define test_close(s) closesocket(s)
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    typedef int test_sock_t;
    #define TEST_SOCK_INVALID (-1)
    #define test_close(s) close(s)
#endif

static int net_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

static test_sock_t connect_client(int port) {
    test_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == TEST_SOCK_INVALID) return TEST_SOCK_INVALID;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        test_close(s);
        return TEST_SOCK_INVALID;
    }
#ifdef _WIN32
    DWORD tv = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

static int contains_text(const void* data, size_t len, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) return 0;
    const char* p = (const char*)data;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(p + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static int send_request(test_sock_t s, const char* path) {
    char req[256];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: test.local\r\nConnection: close\r\n\r\n", path);
    if (n <= 0) return -1;
    size_t sent = 0;
    while (sent < (size_t)n) {
        int w = (int)send(s, req + sent, n - (int)sent, 0);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static int read_all(test_sock_t s, char* buf, int cap) {
    int total = 0;
    while (total < cap - 1) {
        int n = (int)recv(s, buf + total, cap - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    return total;
}

int main(void) {
    if (net_init() != 0) {
        printf("FAIL: network init\n");
        return 1;
    }

    int port = 18991;
    nl_lagg_server_t* server = NULL;
    for (int i = 0; i < 5 && !server; i++) {
        server = nl_lagg_create(port + i);
        if (server) {
            if (nl_lagg_start(server) == NL_LINKAGG_OK) {
                port = port + i;
                break;
            }
            nl_lagg_destroy(server);
            server = NULL;
        }
    }
    if (!server) {
        printf("FAIL: cannot start linkagg server\n");
        return 1;
    }

    if (nl_lagg_add_ipc_backend(server, "node.a", 1) != NL_LINKAGG_OK) {
        printf("FAIL: add backend\n");
        nl_lagg_destroy(server);
        return 1;
    }
    nl_lagg_set_pull_timeout(server, 300, 1); // 总等待 ≈ 600ms 后判定超时

    int failures = 0;

    // ---- 1. 正常拉取 / 响应 ----
    test_sock_t c1 = connect_client(port);
    if (c1 == TEST_SOCK_INVALID || send_request(c1, "/ok") != 0) {
        printf("FAIL: client connect/send\n");
        failures++;
    } else {
        nl_lagg_request_t* req = nl_lagg_pull(server, "node.a", 1500);
        if (!req) {
            printf("FAIL: pull returned NULL\n");
            failures++;
        } else {
            size_t len = 0;
            const void* data = nl_lagg_request_get_data(req, &len);
            if (!data || len == 0 || !contains_text(data, len, "GET /ok")) {
                printf("FAIL: request payload mismatch\n");
                failures++;
            }
            uint64_t id = 0;
            nl_lagg_request_get_id(req, &id);
            const char* resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
            if (nl_lagg_request_respond(req, resp, strlen(resp)) != NL_LINKAGG_OK) {
                printf("FAIL: respond\n");
                failures++;
            }
        }
        char buf[512];
        read_all(c1, buf, sizeof(buf));
        if (strstr(buf, "200 OK") == NULL || strstr(buf, "OK") == NULL) {
            printf("FAIL: client did not receive 200 response: %s\n", buf);
            failures++;
        }
        test_close(c1);
    }

    // ---- 2. 不拉取 → 超时重试后 503，端点标记异常 ----
    test_sock_t c2 = connect_client(port);
    if (c2 == TEST_SOCK_INVALID || send_request(c2, "/timeout") != 0) {
        printf("FAIL: client2 connect/send\n");
        failures++;
    } else {
        char buf[512];
        read_all(c2, buf, sizeof(buf));
        if (strstr(buf, "503") == NULL) {
            printf("FAIL: expected 503 on pull timeout, got: %s\n", buf);
            failures++;
        }
        test_close(c2);
    }
    if (nl_lagg_get_backend_health(server, "node.a") != 0) {
        printf("FAIL: backend should be marked unhealthy after timeout\n");
        failures++;
    }

    // ---- 3. 再次拉取响应后恢复健康 ----
    test_sock_t c3 = connect_client(port);
    if (c3 == TEST_SOCK_INVALID || send_request(c3, "/recover") != 0) {
        printf("FAIL: client3 connect/send\n");
        failures++;
    } else {
        nl_lagg_request_t* req = nl_lagg_pull(server, "node.a", 1500);
        if (!req) {
            printf("FAIL: recovery pull returned NULL\n");
            failures++;
        } else {
            const char* resp = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
            nl_lagg_request_respond(req, resp, strlen(resp));
        }
        char buf[512];
        read_all(c3, buf, sizeof(buf));
        if (strstr(buf, "204") == NULL) {
            printf("FAIL: expected 204 after recovery, got: %s\n", buf);
            failures++;
        }
        test_close(c3);
    }
    if (nl_lagg_get_backend_health(server, "node.a") != 1) {
        printf("FAIL: backend health not restored\n");
        failures++;
    }

    nl_lagg_destroy(server);

    if (failures == 0) {
        printf("PASS: linkagg pull pool tests\n");
        return 0;
    }
    printf("FAIL: %d check(s) failed\n", failures);
    return 1;
}
