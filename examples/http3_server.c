/**
 * NetLeaf HTTP/3 Server Example
 * 
 * 一个简单的 HTTP/3 服务器示例
 */

#include <stdio.h>
#include <stdlib.h>
#include "netleaf_http.h"

// HTTP 请求处理器
void request_handler(const nl_http_request_t* req, nl_http_response_t* resp, void* user_data) {
    // 获取请求信息
    nl_http_method_t method = nl_http_request_get_method(req);
    nl_http_version_t version = nl_http_request_get_version(req);
    const char* path = nl_http_request_get_path(req);
    
    printf("Received %s request for %s (HTTP/%d)\n", 
           method == NL_HTTP_GET ? "GET" : 
           method == NL_HTTP_POST ? "POST" : 
           method == NL_HTTP_PUT ? "PUT" : 
           method == NL_HTTP_DELETE ? "DELETE" : "UNKNOWN", 
           path, version);
    
    // 生成响应
    nl_http_response_set_status(resp, 200);
    nl_http_response_set_header(resp, "Content-Type", "text/html; charset=utf-8");
    
    char body[2048];
    snprintf(body, sizeof(body), 
             "<!DOCTYPE html>"
             "<html>"
             "<head>"
             "<title>NetLeaf HTTP/3 Server</title>"
             "<style>"
             "body { font-family: Arial, sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }"
             "h1 { color: #9b59b6; }"
             ".info { background-color: #f8f9fa; padding: 15px; border-radius: 5px; margin-top: 20px; }"
             ".badge { display: inline-block; padding: 4px 12px; background-color: #9b59b6; color: white; border-radius: 4px; margin-left: 10px; }"
             "</style>"
             "</head>"
             "<body>"
             "<h1>Hello from NetLeaf HTTP/3! <span class=\"badge\">HTTP/3</span></h1>"
             "<p>This is a native HTTP/3 (QUIC) server built with NetLeaf.</p>"
             "<div class=\"info\">"
             "<p><strong>Request path:</strong> %s</p>"
             "<p><strong>Protocol:</strong> HTTP/%d (QUIC)</p>"
             "</div>"
             "</body>"
             "</html>", path, version);
    
    nl_http_response_set_body(resp, body, strlen(body));
}

int main(int argc, char* argv[]) {
    int port = 8443;
    
    if (argc > 1) {
        port = atoi(argv[1]);
    }
    
    printf("===============================================\n");
    printf("  NetLeaf HTTP/3 Server Example\n");
    printf("===============================================\n");
    printf("Starting HTTP/3 server on UDP port %d...\n", port);
    printf("Press Ctrl+C to stop the server\n");
    printf("\n");
    
    // 创建 HTTP/3 服务器
    nl_http3_server_t* server = nl_http3_server_create(port);
    if (!server) {
        fprintf(stderr, "Failed to create HTTP/3 server\n");
        return 1;
    }
    
    // 设置请求处理器
    nl_http3_server_set_handler(server, request_handler, NULL);
    
    // 启动服务器
    if (nl_http3_server_start(server) != 0) {
        fprintf(stderr, "Failed to start HTTP/3 server\n");
        nl_http3_server_destroy(server);
        return 1;
    }
    
    printf("HTTP/3 Server is running on UDP port %d\n", port);
    printf("Note: HTTP/3 (QUIC) requires client support for QUIC\n");
    printf("\n");
    
    // 保持程序运行
    while (1) {
        #ifdef _WIN32
        Sleep(1000);
        #else
        sleep(1);
        #endif
    }
    
    // 停止服务器（不会执行到这里）
    nl_http3_server_stop(server);
    nl_http3_server_destroy(server);
    
    return 0;
}
