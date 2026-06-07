#include "netleaf.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    // 创建Web服务器
    nl_web_server_t* server = nl_web_create(8080);
    
    // 设置编码为UTF-8
    nl_web_set_encoding(server, "UTF-8");
    
    // 示例1: HTML页面带变量替换
    int user_id = 12345;
    const char* username = "张三";
    const char* vars1[] = {"user_id", "username"};
    const char* values1[] = {"12345", "张三"};
    
    nl_web_add_html_with_vars(server, "/profile",
        "<!DOCTYPE html>\n"
        "<html><head><title>用户资料</title></head>\n"
        "<body>\n"
        "<h1>欢迎, {{<var>username</var>}}</h1>\n"
        "<p>用户ID: {{<var>user_id</var>}}</p>\n"
        "</body></html>",
        vars1, values1, 2);
    
    // 示例2: Vue页面带变量替换
    int product_count = 100;
    const char* vars2[] = {"product_count", "company_name"};
    const char* values2[] = {"100", "NetLeaf科技"};
    
    nl_web_add_vue_with_vars(server, "/dashboard",
        "<h1>{{<var>company_name</var>}} - 数据面板</h1>\n"
        "<div class=\"stat\">\n"
        "  <div class=\"stat-value\">{{<var>product_count</var>}}</div>\n"
        "  <div class=\"stat-label\">产品数量</div>\n"
        "</div>",
        vars2, values2, 2);
    
    // 示例3: 动态变量演示
    char dynamic_value[256];
    snprintf(dynamic_value, sizeof(dynamic_value), "%d", rand() % 1000);
    const char* vars3[] = {"random_number", "current_time"};
    const char* values3[] = {dynamic_value, "2024-01-15"};
    
    nl_web_add_html_with_vars(server, "/dynamic",
        "<!DOCTYPE html>\n"
        "<html><head><title>动态内容</title></head>\n"
        "<body>\n"
        "<h1>动态页面</h1>\n"
        "<p>随机数: {{<var>random_number</var>}}</p>\n"
        "<p>日期: {{<var>current_time</var>}}</p>\n"
        "</body></html>",
        vars3, values3, 2);
    
    printf("服务器启动在 http://localhost:8080\n");
    printf("访问路径:\n");
    printf("  - /profile    : HTML页面变量替换示例\n");
    printf("  - /dashboard  : Vue页面变量替换示例\n");
    printf("  - /dynamic    : 动态变量示例\n");
    printf("\n注意: 变量在服务器端已硬编码，用户F12看不到原始变量名\n");
    
    // 启动服务器
    nl_web_start(server);
    
    // 保持运行
    while (1) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
    }
    
    nl_web_destroy(server);
    return 0;
}