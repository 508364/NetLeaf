/*
 * NetLeaf v2.0.0 - TCP服务器示例
 *
 * 编译:
 *   Windows: cl example_tcp_server.c /I ../include /link ../build/lib/Release/netleaf.lib ws2_32.lib
 *   Linux:   gcc example_tcp_server.c -I ../include -L ../build/lib -lnetleaf -lpthread -o example_tcp_server
 */

#include <stdio.h>
#include <string.h>
#include "netleaf.h"

#ifdef _WIN32
#include <windows.h>
#define SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP(ms) usleep((ms) * 1000)
#endif

/* 连接数据接收回调 */
void on_data(nl_conn_t* conn, const char* data, size_t len) {
    char client_ip[64];
    nl_conn_get_remote_addr(conn, client_ip, sizeof(client_ip));
    printf("[%s] 收到数据: %.*s\n", client_ip, (int)len, data);
    
    /* 回显数据 */
    nl_conn_send(conn, data, len);
}

/* 新连接回调 */
void on_connect(nl_conn_t* conn, void* user_data) {
    char client_ip[64];
    nl_conn_get_remote_addr(conn, client_ip, sizeof(client_ip));
    printf("[连接] 新客户端: %s\n", client_ip);
    
    /* 发送欢迎消息 */
    const char* welcome = "欢迎使用NetLeaf TCP服务器！\n";
    nl_conn_send(conn, welcome, strlen(welcome));
}

/* 断开连接回调 */
void on_disconnect(nl_conn_t* conn, void* user_data) {
    char client_ip[64];
    nl_conn_get_remote_addr(conn, client_ip, sizeof(client_ip));
    printf("[断开] 客户端: %s\n", client_ip);
}

int main() {
    printf("========================================\n");
    printf("   NetLeaf v2.0.0 - TCP回显服务器\n");
    printf("========================================\n\n");
    printf("监听端口: 9000\n");
    printf("测试方法: telnet localhost 9000\n");
    printf("按 Ctrl+C 停止...\n\n");

    /* 创建TCP服务器 */
    nl_server_t* server = nl_server_create(NL_PROTO_TCP, 9000);
    if (!server) {
        printf("错误: 无法创建服务器\n");
        return -1;
    }

    /* 设置回调 */
    nl_server_set_handler(server, on_data, NULL);
    nl_server_set_connect_handler(server, on_connect, NULL);
    nl_server_set_disconnect_handler(server, on_disconnect, NULL);

    /* 启动服务器 */
    if (nl_server_start(server) != 0) {
        printf("错误: 无法启动服务器\n");
        nl_server_destroy(server);
        return -1;
    }

    printf("服务器运行中...\n");

    /* 保持运行 */
    while (1) {
        SLEEP(1000);
    }

    /* 清理 */
    nl_server_stop(server);
    nl_server_destroy(server);

    return 0;
}
