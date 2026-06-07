/*
 * NetLeaf v2.0.0 - 完整功能示例
 *
 * 编译:
 *   Windows: cl example_all_features.c /I ../include /link ../build/lib/Release/netleaf.lib ws2_32.lib
 *   Linux:   gcc example_all_features.c -I ../include -L ../build/lib -lnetleaf -lpthread -o example_all_features
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netleaf.h"

#ifdef _WIN32
#include <windows.h>
#define SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP(ms) usleep((ms) * 1000)
#endif

// 简单的HTML首页
const char* HOME_HTML =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <title>NetLeaf v2.0.0 - 完整功能演示</title>\n"
    "  <style>\n"
    "    * { margin:0; padding:0; box-sizing:border-box; }\n"
    "    body { font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif; background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); min-height:100vh; padding:40px; }\n"
    "    .container { max-width:800px; margin:0 auto; background:#fff; border-radius:16px; box-shadow:0 20px 60px rgba(0,0,0,0.3); padding:40px; animation:fadeIn 0.5s ease-out; }\n"
    "    @keyframes fadeIn { from{opacity:0; transform:translateY(-20px);} to{opacity:1; transform:translateY(0);} }\n"
    "    h1 { color:#2d3748; font-size:2.5rem; margin-bottom:24px; text-align:center; }\n"
    "    .subtitle { text-align:center; color:#718096; margin-bottom:32px; font-size:1.1rem; }\n"
    "    .links { display:grid; grid-template-columns:repeat(auto-fit, minmax(250px,1fr)); gap:16px; }\n"
    "    .link { display:flex; align-items:center; gap:12px; padding:20px; background:#f7fafc; border-radius:12px; text-decoration:none; color:#2d3748; font-weight:600; transition:all 0.2s; border-left:4px solid #667eea; }\n"
    "    .link:hover { background:#667eea; color:white; transform:translateX(4px); }\n"
    "    .emoji { font-size:2rem; }\n"
    "    .info { margin-top:32px; padding:20px; background:#f7fafc; border-radius:12px; text-align:center; color:#4a5568; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div class=\"container\">\n"
    "    <h1>🚀 NetLeaf v2.0.0</h1>\n"
    "    <p class=\"subtitle\">高性能网络库 - 完整功能演示</p>\n"
    "    \n"
    "    <div class=\"links\">\n"
    "      <a href=\"/counter\" class=\"link\">\n"
    "        <span class=\"emoji\">🔢</span>\n"
    "        <span>计数器示例</span>\n"
    "      </a>\n"
    "      <a href=\"/dashboard\" class=\"link\">\n"
    "        <span class=\"emoji\">📊</span>\n"
    "        <span>数据面板</span>\n"
    "      </a>\n"
    "      <a href=\"/form\" class=\"link\">\n"
    "        <span class=\"emoji\">📝</span>\n"
    "        <span>表单提交</span>\n"
    "      </a>\n"
    "      <a href=\"/todo\" class=\"link\">\n"
    "        <span class=\"emoji\">✅</span>\n"
    "        <span>待办事项</span>\n"
    "      </a>\n"
    "      <a href=\"/chat\" class=\"link\">\n"
    "        <span class=\"emoji\">💬</span>\n"
    "        <span>聊天助手</span>\n"
    "      </a>\n"
    "    </div>\n"
    "    \n"
    "    <div class=\"info\">\n"
    "      <p><strong>服务器运行中...</strong></p>\n"
    "      <p>按 Ctrl+C 停止</p>\n"
    "    </div>\n"
    "  </div>\n"
    "</body>\n"
    "</html>";

int main() {
    printf("========================================\n");
    printf("   NetLeaf v2.0.0 - 完整功能演示\n");
    printf("========================================\n\n");
    
    // 创建Web服务器
    nl_web_server_t* server = nl_web_create(8080);
    if (!server) {
        printf("错误: 无法创建服务器\n");
        return 1;
    }
    
    // 添加首页
    nl_web_add_html(server, "/", HOME_HTML);
    
    // 添加计数器
    nl_web_add_counter(server, "/counter", "交互式计数器");
    
    // 添加数据面板
    nl_web_add_dashboard(server, "/dashboard", "实时数据面板");
    
    // 添加表单
    const char* form_fields[] = {"姓名", "邮箱", "消息"};
    nl_web_add_form(server, "/form", "联系表单", form_fields, 3);
    
    // 添加待办事项
    nl_web_add_todo(server, "/todo", "任务清单");
    
    // 添加聊天
    nl_web_add_chat(server, "/chat", "NetLeaf聊天助手");
    
    printf("服务器已启动！\n\n");
    printf("访问地址:\n");
    printf("  首页:      http://localhost:8080/\n");
    printf("  计数器:    http://localhost:8080/counter\n");
    printf("  数据面板:  http://localhost:8080/dashboard\n");
    printf("  表单:      http://localhost:8080/form\n");
    printf("  待办:      http://localhost:8080/todo\n");
    printf("  聊天:      http://localhost:8080/chat\n\n");
    printf("按 Ctrl+C 停止...\n\n");
    
    // 启动服务器
    if (nl_web_start(server) != 0) {
        printf("错误: 无法启动服务器\n");
        nl_web_destroy(server);
        return 1;
    }
    
    // 保持运行
    while (1) {
        SLEEP(1000);
    }
    
    // 清理（不会执行到这里）
    nl_web_stop(server);
    nl_web_destroy(server);
    
    return 0;
}
