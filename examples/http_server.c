/**
 * NetLeaf Combined HTTP Server Example
 * 
 * 一个综合的 HTTP 服务器示例，支持 HTTP/1.1、HTTP/2 和 HTTP/3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netleaf_http.h"

// 全局服务器指针
static nl_http_server_t* g_http1_server = NULL;
static nl_http2_server_t* g_http2_server = NULL;
static nl_http3_server_t* g_http3_server = NULL;

// 共享的 HTTP 请求处理器
void request_handler(const nl_http_request_t* req, nl_http_response_t* resp, void* user_data) {
    nl_http_method_t method = nl_http_request_get_method(req);
    nl_http_version_t version = nl_http_request_get_version(req);
    const char* path = nl_http_request_get_path(req);
    
    const char* proto_name = "HTTP/1.1";
    const char* badge_color = "#3498db";
    if (version == 2) {
        proto_name = "HTTP/2";
        badge_color = "#27ae60";
    } else if (version == 3) {
        proto_name = "HTTP/3";
        badge_color = "#9b59b6";
    }
    
    printf("[%s] Received %s request for %s\n", proto_name, 
           method == NL_HTTP_GET ? "GET" : 
           method == NL_HTTP_POST ? "POST" : 
           method == NL_HTTP_PUT ? "PUT" : 
           method == NL_HTTP_DELETE ? "DELETE" : "UNKNOWN", 
           path);
    
    // 生成响应
    nl_http_response_set_status(resp, 200);
    nl_http_response_set_header(resp, "Content-Type", "text/html; charset=utf-8");
    
    char body[4096];
    snprintf(body, sizeof(body), 
             "<!DOCTYPE html>"
             "<html>"
             "<head>"
             "<title>NetLeaf HTTP Server</title>"
             "<style>"
             "body { font-family: Arial, sans-serif; max-width: 900px; margin: 0 auto; padding: 20px; }"
             "h1 { color: #2c3e50; }"
             ".info { background-color: #f8f9fa; padding: 15px; border-radius: 5px; margin-top: 20px; }"
             ".badge { display: inline-block; padding: 4px 12px; background-color: %s; color: white; border-radius: 4px; margin-left: 10px; }"
             ".features { margin-top: 30px; }"
             ".feature { background-color: #fff; border: 1px solid #e0e0e0; border-radius: 5px; padding: 15px; margin-bottom: 10px; }"
             ".feature h3 { margin-top: 0; }"
             "</style>"
             "</head>"
             "<body>"
             "<h1>Welcome to NetLeaf! <span class=\"badge\">%s</span></h1>"
             "<p>This is a combined HTTP server supporting HTTP/1.1, HTTP/2, and HTTP/3.</p>"
             "<div class=\"info\">"
             "<p><strong>Request path:</strong> %s</p>"
             "<p><strong>Protocol:</strong> %s</p>"
             "</div>"
             "<div class=\"features\">"
             "<h2>Features</h2>"
             "<div class=\"feature\">"
             "<h3>✅ HTTP/1.1</h3>"
             "<p>Full support for HTTP/1.1 with standard headers and methods.</p>"
             "</div>"
             "<div class=\"feature\">"
             "<h3>✅ HTTP/2</h3>"
             "<p>Native HTTP/2 support with HPACK compression and stream multiplexing.</p>"
             "</div>"
             "<div class=\"feature\">"
             "<h3>✅ HTTP/3</h3>"
             "<p>Native HTTP/3 support with QUIC transport layer (UDP).</p>"
             "</div>"
             "</div>"
             "</body>"
             "</html>", badge_color, proto_name, path, proto_name);
    
    nl_http_response_set_body(resp, body, strlen(body));
}

// 信号处理函数（简化版）
static void cleanup_servers(void) {
    printf("\nShutting down servers...\n");
    
    if (g_http3_server) {
        nl_http3_server_stop(g_http3_server);
        nl_http3_server_destroy(g_http3_server);
        g_http3_server = NULL;
    }
    
    if (g_http2_server) {
        nl_http2_server_stop(g_http2_server);
        nl_http2_server_destroy(g_http2_server);
        g_http2_server = NULL;
    }
    
    if (g_http1_server) {
        nl_http_server_stop(g_http1_server);
        nl_http_server_destroy(g_http1_server);
        g_http1_server = NULL;
    }
    
    printf("All servers stopped.\n");
}

int main(int argc, char* argv[]) {
    int http1_port = 8080;
    int http2_port = 8443;
    int http3_port = 8443;
    
    printf("===============================================\n");
    printf("  NetLeaf Combined HTTP Server Example\n");
    printf("===============================================\n");
    printf("\n");
    
    // 创建和启动 HTTP/1.1 服务器
    printf("Starting HTTP/1.1 server on port %d...\n", http1_port);
    g_http1_server = nl_http_server_create(http1_port);
    if (!g_http1_server) {
        fprintf(stderr, "Failed to create HTTP/1.1 server\n");
        return 1;
    }
    nl_http_server_set_handler(g_http1_server, request_handler, NULL);
    if (nl_http_server_start(g_http1_server) != 0) {
        fprintf(stderr, "Failed to start HTTP/1.1 server\n");
        cleanup_servers();
        return 1;
    }
    printf("✅ HTTP/1.1 server is running at http://localhost:%d\n", http1_port);
    
    // 创建和启动 HTTP/2 服务器
    printf("\nStarting HTTP/2 server on port %d...\n", http2_port);
    g_http2_server = nl_http2_server_create(http2_port);
    if (!g_http2_server) {
        fprintf(stderr, "Failed to create HTTP/2 server\n");
        cleanup_servers();
        return 1;
    }
    nl_http2_server_set_handler(g_http2_server, request_handler, NULL);
    if (nl_http2_server_start(g_http2_server) != 0) {
        fprintf(stderr, "Failed to start HTTP/2 server\n");
        cleanup_servers();
        return 1;
    }
    printf("✅ HTTP/2 server is running on port %d\n", http2_port);
    
    // 创建和启动 HTTP/3 服务器
    printf("\nStarting HTTP/3 server on UDP port %d...\n", http3_port);
    g_http3_server = nl_http3_server_create(http3_port);
    if (!g_http3_server) {
        fprintf(stderr, "Failed to create HTTP/3 server\n");
        cleanup_servers();
        return 1;
    }
    nl_http3_server_set_handler(g_http3_server, request_handler, NULL);
    if (nl_http3_server_start(g_http3_server) != 0) {
        fprintf(stderr, "Failed to start HTTP/3 server\n");
        cleanup_servers();
        return 1;
    }
    printf("✅ HTTP/3 server is running on UDP port %d\n", http3_port);
    
    printf("\n===============================================\n");
    printf("All servers are running!\n");
    printf("===============================================\n");
    printf("\n");
    printf("Press Ctrl+C to stop all servers\n");
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
    cleanup_servers();
    
    return 0;
}
