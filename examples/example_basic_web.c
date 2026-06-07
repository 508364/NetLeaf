/*
 * NetLeaf v2.0.0 - 基础Web服务器示例
 *
 * 编译:
 *   Windows: cl example_basic_web.c /I ../include /link ../build/lib/Release/netleaf.lib ws2_32.lib
 *   Linux:   gcc example_basic_web.c -I ../include -L ../build/lib -lnetleaf -lpthread -o example_basic_web
 */

#include <stdio.h>
#include "netleaf.h"

#ifdef _WIN32
#include <windows.h>
#define SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP(ms) usleep((ms) * 1000)
#endif

int main() {
    printf("========================================\n");
    printf("   NetLeaf v2.0.0 - 基础Web服务器示例\n");
    printf("========================================\n\n");
    printf("访问地址:\n");
    printf("  主页:     http://localhost:8080/\n");
    printf("  计数器:   http://localhost:8080/counter\n");
    printf("  数据面板: http://localhost:8080/dashboard\n");
    printf("  表单:     http://localhost:8080/form\n\n");
    printf("按 Ctrl+C 停止...\n\n");

    /* 创建Web服务器 */
    nl_web_server_t* server = nl_web_create(8080);
    if (!server) {
        printf("错误: 无法创建服务器\n");
        return -1;
    }

    /* 添加主页 - 自定义HTML */
    nl_web_add_html(server, "/",
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "  <title>NetLeaf Demo</title>\n"
        "  <style>\n"
        "    body { font-family:system-ui;padding:40px;background:linear-gradient(135deg,#667eea,#764ba2);color:white; }\n"
        "    .container { max-width:600px;margin:0 auto;background:white;border-radius:16px;padding:40px;color:#333; }\n"
        "    h1 { color:#667eea; }\n"
        "    a { display:block;padding:16px;margin:8px 0;background:#f0f0f0;border-radius:8px;text-decoration:none;color:#333;font-weight:600; }\n"
        "    a:hover { background:#667eea;color:white; }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"container\">\n"
        "    <h1>🚀 NetLeaf Demo</h1>\n"
        "    <p style=\"margin:24px 0;\">欢迎使用NetLeaf！点击下方链接查看各种预设组件：</p>\n"
        "    <a href=\"/counter\">📊 计数器组件</a>\n"
        "    <a href=\"/dashboard\">📈 数据面板</a>\n"
        "    <a href=\"/form\">📝 表单示例</a>\n"
        "  </div>\n"
        "</body>\n"
        "</html>");

    /* 添加计数器组件 */
    nl_web_add_counter(server, "/counter", "Interactive Counter");

    /* 添加数据面板 */
    nl_web_add_dashboard(server, "/dashboard", "Analytics Dashboard");

    /* 添加表单 */
    const char* fields[] = {"name", "email", "message"};
    nl_web_add_form(server, "/form", "Contact Form", fields, 3);

    /* 启动服务器 */
    if (nl_web_start(server) != 0) {
        printf("错误: 无法启动服务器\n");
        nl_web_destroy(server);
        return -1;
    }

    printf("服务器运行中...\n");

    /* 保持运行 */
    while (1) {
        SLEEP(1000);
    }

    /* 清理 (不会执行到这里) */
    nl_web_stop(server);
    nl_web_destroy(server);

    return 0;
}
