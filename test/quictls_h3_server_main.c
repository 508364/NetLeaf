/**
 * @file quictls_h3_server_main.c
 * @brief 可运行的 H3 服务端 harness：暴露 UDP 端口并写入自签 CA，供外部 QUIC 客户端（如 aioquic）连接，
 *        用于排查“HTTP/3 真兼容性（aioquic client 握手不通）”的握手失败点。
 *
 * 用法：./h3_server_main [port] [ca_out_pem]
 *   - 缺省端口 44333；缺省把自签 CA 写到 ./h3_ca.pem
 *   - 以 NLH3_RUN_SECS 环境变量指定运行秒数（默认 60s），便于自动化测试
 */
/* POSIX 特性宏须先于任何系统头包含（严格 -std=c99 模式下 usleep/socket 原型可见性） */
#if !defined(_WIN32)
  #ifndef _POSIX_C_SOURCE
    #define _POSIX_C_SOURCE 200809L
  #endif
  #ifndef _DEFAULT_SOURCE
    #define _DEFAULT_SOURCE
  #endif
#endif

#include "netleaf_quic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #define NL_SLEEP_MS(ms) Sleep((DWORD)(ms))
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <unistd.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #define NL_SLEEP_MS(ms) usleep((ms) * 1000)
  /* 平台别名：与 Windows SOCKET API 对齐，消除 harness 中的平台分支 */
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
  #define closesocket(fd) close(fd)
#endif

static void nlh3_demo_handler(const char* method, const char* path, const char* authority,
                              void* userdata, char** resp_body, size_t* resp_len) {
    static const char body[] = "NetLeaf HTTP/3 OK";
    (void)method; (void)authority; (void)userdata;
    size_t n = sizeof(body) - 1;
    *resp_body = (char*)malloc(n + 1);
    if (*resp_body) {
        memcpy(*resp_body, body, n);
        (*resp_body)[n] = '\0';
        *resp_len = n;
    } else {
        *resp_len = 0;
    }
    printf("[h3] req %s %s\n", method, path);
    fflush(stdout);
}

int main(int argc, char** argv) {
    int port = 44333;
    const char* ca_out = "h3_ca.pem";
    char cert_pem[4096], key_pem[4096];

    if (argc > 1) port = atoi(argv[1]);
    if (argc > 2) ca_out = argv[2];

    /* 把 stderr 重定向到文件，保证调试输出可捕获（PowerShell 管道对原生 exe 的 stderr 不可靠）。
     * Windows 联调路径，仅在 _WIN32 下启用（POSIX 下 stderr 直接输出即可）。 */
#if defined(_WIN32)
    {
        const char* dbg = "f:/C-C++/NetLeaf/build_win/bin/srv_dbg.log";
        FILE* ff = freopen(dbg, "w", stderr);
        if (ff) { setvbuf(stderr, NULL, _IONBF, 0); }
    }
#endif

    fprintf(stderr, "[h3-srv] server_main starting, port=%d ca=%s\n", port, ca_out);

    if (nl_quictls_init() != 0) { fprintf(stderr, "init failed\n"); return 2; }
    if (nl_tls13_gen_self_signed(cert_pem, sizeof(cert_pem), key_pem, sizeof(key_pem)) != 0) {
        fprintf(stderr, "gen self-signed failed\n"); return 3;
    }

    FILE* f = fopen(ca_out, "wb");
    if (f) { fputs(cert_pem, f); fclose(f); }

    nlh3_server_t* s = nlh3_server_create(port, cert_pem, key_pem);
    if (!s) { fprintf(stderr, "server create failed\n"); return 4; }
    nlh3_server_set_handler(s, nlh3_demo_handler, NULL);
    if (nlh3_server_start(s) != 0) { nlh3_server_destroy(s); return 5; }

    fprintf(stderr, "[h3-srv] server listening on udp/%d, ca=%s\n", port, ca_out);

    /* 验证：创建一个独立的 UDP socket 确认端口可绑定（排除端口冲突/防火墙） */
    {
        SOCKET vfd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in va;
        memset(&va, 0, sizeof(va));
        va.sin_family = AF_INET;
        va.sin_port = htons((unsigned short)port);
        va.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int vrc = (vfd == INVALID_SOCKET) ? -1 : bind(vfd, (struct sockaddr*)&va, sizeof(va));
        fprintf(stderr, "[h3-srv] verify bind rc=%d (0=OK, else CONFLICT)\n", vrc);
        if (vfd != INVALID_SOCKET) closesocket(vfd);
    }

    const char* es = getenv("NLH3_RUN_SECS");
    int secs = es ? atoi(es) : 60;
    for (int i = 0; i < secs; i++) NL_SLEEP_MS(1000);

    nlh3_server_stop(s);
    nlh3_server_destroy(s);
    fprintf(stderr, "[h3-srv] done\n");
    return 0;
}
