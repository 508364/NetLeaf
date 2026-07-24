/*
 * NetLeaf v2.1.6 - 完整功能示例
 *
 * 编译:
 *   Windows: cl example_all_features.c /I ../include /link ../build/lib/Release/netleaf.lib ws2_32.lib
 *   Linux:   gcc example_all_features.c -I ../include -L ../build/lib -lnetleaf -lpthread -o example_all_features
 *
 * 启用调试日志:
 *   添加编译宏 DEBUG_LOGGING
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "netleaf.h"

#ifdef _WIN32
#include <windows.h>
#define SLEEP(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP(ms) usleep((ms) * 1000)
#endif

#ifdef DEBUG_LOGGING
#define LOG_DEBUG(fmt, ...) do { \
        char log_buffer[4096]; \
        time_t now = time(NULL); \
        struct tm* tm_info = localtime(&now); \
        char time_str[32]; \
        strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", tm_info); \
        snprintf(log_buffer, sizeof(log_buffer), "%s [DEBUG] " fmt "\n", time_str, ##__VA_ARGS__); \
        nl_encoding_console_output(log_buffer, "UTF-8"); \
} while(0)
#else
#define LOG_DEBUG(fmt, ...) ((void)0)
#endif

#define LOG_INFO(fmt, ...) do { \
    char log_buffer[4096]; \
    time_t now = time(NULL); \
    struct tm* tm_info = localtime(&now); \
    char time_str[32]; \
    strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S]", tm_info); \
    snprintf(log_buffer, sizeof(log_buffer), "%s [INFO] " fmt "\n", time_str, ##__VA_ARGS__); \
    nl_encoding_console_output(log_buffer, "UTF-8"); \
} while(0)

#define NL_PRINTF(fmt, ...) do { \
    char buffer[2048]; \
    snprintf(buffer, sizeof(buffer), fmt, ##__VA_ARGS__); \
    nl_encoding_console_output(buffer, "UTF-8"); \
} while(0)

const char* HOME_HTML =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <title>NetLeaf v2.2.2 - 完整功能演示</title>\n"
    "  <meta charset=\"UTF-8\">\n"
    "  <style>\n"
    "    * { margin:0; padding:0; box-sizing:border-box; }\n"
    "    body { font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif; background:linear-gradient(135deg,#667eea 0%,#764ba2 100%); min-height:100vh; padding:40px; }\n"
    "    .container { max-width:900px; margin:0 auto; background:#fff; border-radius:16px; box-shadow:0 20px 60px rgba(0,0,0,0.3); padding:40px; animation:fadeIn 0.5s ease-out; }\n"
    "    @keyframes fadeIn { from{opacity:0; transform:translateY(-20px);} to{opacity:1; transform:translateY(0);} }\n"
    "    h1 { color:#2d3748; font-size:2.5rem; margin-bottom:24px; text-align:center; }\n"
    "    .subtitle { text-align:center; color:#718096; margin-bottom:32px; font-size:1.1rem; }\n"
    "    .links { display:grid; grid-template-columns:repeat(auto-fit, minmax(220px,1fr)); gap:16px; }\n"
    "    .link { display:flex; align-items:center; gap:12px; padding:20px; background:#f7fafc; border-radius:12px; text-decoration:none; color:#2d3748; font-weight:600; transition:all 0.2s; border-left:4px solid #667eea; }\n"
    "    .link:hover { background:#667eea; color:white; transform:translateX(4px); }\n"
    "    .emoji { font-size:2rem; }\n"
    "    .info { margin-top:32px; padding:20px; background:#f7fafc; border-radius:12px; text-align:center; color:#4a5568; }\n"
    "    .feature-tag { display:inline-block; padding:4px 12px; background:#667eea; color:white; border-radius:20px; font-size:0.8rem; margin-left:8px; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div class=\"container\">\n"
    "    <h1>🚀 NetLeaf v2.1.6</h1>\n"
    "    <p class=\"subtitle\">高性能网络库 - 完整功能演示 <span class=\"feature-tag\">编码动态适配</span></p>\n"
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
    "      <a href=\"/encoding\" class=\"link\">\n"
    "        <span class=\"emoji\">🌐</span>\n"
    "        <span>编码测试</span>\n"
    "      </a>\n"
    "      <a href=\"/inline-vue\" class=\"link\">\n"
    "        <span class=\"emoji\">⚡</span>\n"
    "        <span>内联Vue测试</span>\n"
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

const char* ENCODING_TEST_HTML =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <title>编码动态适配测试</title>\n"
    "  <meta charset=\"UTF-8\">\n"
    "  <style>\n"
    "    body { font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif; max-width:800px; margin:40px auto; padding:20px; }\n"
    "    h1 { color:#2d3748; margin-bottom:24px; }\n"
    "    .test-box { padding:20px; margin:16px 0; background:#f7fafc; border-radius:12px; border-left:4px solid #667eea; }\n"
    "    .chinese-text { font-size:1.2rem; color:#2d3748; }\n"
    "    .description { color:#718096; margin-top:8px; font-size:0.9rem; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <h1>🌐 编码动态适配测试</h1>\n"
    "  \n"
    "  <div class=\"test-box\">\n"
    "    <div class=\"chinese-text\">你好世界！Hello World! こんにちは世界！안녕하세요!</div>\n"
    "    <div class=\"description\">测试多语言字符编码支持</div>\n"
    "  </div>\n"
    "  \n"
    "  <div class=\"test-box\">\n"
    "    <div class=\"chinese-text\">简体中文测试：北京、上海、广州</div>\n"
    "    <div class=\"description\">简体中文字符</div>\n"
    "  </div>\n"
    "  \n"
    "  <div class=\"test-box\">\n"
    "    <div class=\"chinese-text\">繁體中文測試：台北、香港、澳門</div>\n"
    "    <div class=\"description\">繁體中文字符</div>\n"
    "  </div>\n"
    "  \n"
    "  <div class=\"test-box\">\n"
    "    <div class=\"chinese-text\">Special Characters: サンプル テスト アプリケーション</div>\n"
    "    <div class=\"description\">日文假名测试</div>\n"
    "  </div>\n"
    "</body>\n"
    "</html>";

const char* INLINE_VUE_HTML = 
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <meta charset=\"UTF-8\">\n"
    "  <title>内联Vue测试</title>\n"
    "  <script src=\"https://unpkg.com/vue@3/dist/vue.global.js\"></script>\n"
    "  <style>\n"
    "    .container { max-width:600px; margin:40px auto; padding:20px; }\n"
    "    .card { background:#f7fafc; padding:20px; border-radius:12px; margin:16px 0; }\n"
    "    .btn { padding:10px 20px; background:#667eea; color:white; border:none; border-radius:8px; cursor:pointer; }\n"
    "    .btn:hover { background:#5a67d8; }\n"
    "    input { padding:8px; border:2px solid #e2e8f0; border-radius:8px; width:100%; }\n"
    "    .count { font-size:48px; text-align:center; color:#667eea; font-weight:bold; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div class=\"container\" id=\"app\">\n"
    "    <h1>⚡ 内联Vue测试</h1>\n"
    "    <p style=\"color:#718096;\">测试内联Vue组件功能，引入Vue CDN</p>\n"
    "    \n"
    "    <div class=\"card\">\n"
    "      <h3>计数器</h3>\n"
    "      <div class=\"count\">{{ count }}</div>\n"
    "      <div style=\"display:flex;gap:8px;margin-top:16px;\">\n"
    "        <button class=\"btn\" @click=\"count++\">+</button>\n"
    "        <button class=\"btn\" @click=\"count--\">-</button>\n"
    "      </div>\n"
    "    </div>\n"
    "    \n"
    "    <div class=\"card\">\n"
    "      <h3>双向绑定</h3>\n"
    "      <input v-model=\"message\" placeholder=\"输入消息...\" />\n"
    "      <p style=\"margin-top:12px;\">你输入的是: <strong>{{ message }}</strong></p>\n"
    "    </div>\n"
    "    \n"
    "    <div class=\"card\">\n"
    "      <h3>条件渲染</h3>\n"
    "      <button class=\"btn\" @click=\"show = !show\">切换显示</button>\n"
    "      <div v-if=\"show\" style=\"margin-top:12px;padding:12px;background:white;border-radius:8px;\">\n"
    "        ✨ 这是动态显示的内容！\n"
    "      </div>\n"
    "    </div>\n"
    "  </div>\n"
    "  <script>\n"
    "    const { createApp, ref } = Vue;\n"
    "    createApp({\n"
    "      setup() {\n"
    "        const count = ref(0);\n"
    "        const message = ref('');\n"
    "        const show = ref(true);\n"
    "        return { count, message, show };\n"
    "      }\n"
    "    }).mount('#app');\n"
    "  </script>\n"
    "</body>\n"
    "</html>";

int main() {
    // 使用编码适配输出，确保中文正确显示
    NL_PRINTF("========================================\n");
    NL_PRINTF("   NetLeaf v2.1.6 - 完整功能演示\n");
    NL_PRINTF("========================================\n\n");
    
    LOG_DEBUG("程序启动，初始化中...");
    
    // 启用自动清理（推荐开启）
    nl_web_set_auto_cleanup(1);
    LOG_DEBUG("自动清理功能已启用");
    
    // 创建Web服务器
    LOG_DEBUG("正在创建Web服务器，端口: 8080");
    nl_web_server_t* server = nl_web_create(8080);
    if (!server) {
        LOG_INFO("错误: 无法创建服务器");
        return 1;
    }
    LOG_INFO("Web服务器创建成功，端口: 8080");
    
    // 设置统一编码（v2.1.6新功能：自动为内联HTML添加charset标签）
    nl_web_set_encoding(server, "UTF-8");
    LOG_DEBUG("服务器编码已设置为: UTF-8");
    
    // 启用自动编码协商（v2.1.6新功能）
    nl_web_enable_auto_encoding(server, 1);
    nl_web_set_fallback_encoding(server, "UTF-8");
    LOG_DEBUG("自动编码协商已启用");
    
    // 使用编码适配输出中文内容
    NL_PRINTF("✅ 已启用编码动态适配功能\n");
    NL_PRINTF("   - 自动解析Accept-Charset请求头\n");
    NL_PRINTF("   - 运行时自动转码响应内容\n");
    NL_PRINTF("   - 仅对非ASCII字符进行转码优化\n");
    NL_PRINTF("   - 自动为内联HTML添加charset标签\n\n");
    
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
    
    // 添加编码测试页面（v2.1.6新功能）
    nl_web_add_html(server, "/encoding", ENCODING_TEST_HTML);
    
    // 添加内联Vue测试页面（v2.1.6新功能）
    nl_web_add_html(server, "/inline-vue", INLINE_VUE_HTML);
    
    // 测试编码转换API
    const char* test_chinese = "你好世界！Hello World!";
    char* gbk_output = nl_encoding_convert(test_chinese, strlen(test_chinese), "UTF-8", "GBK");
    if (gbk_output) {
        NL_PRINTF("✅ 编码转换测试成功\n");
        free(gbk_output);
    }
    
    const char* detected = nl_encoding_detect(test_chinese, strlen(test_chinese));
    NL_PRINTF("✅ 编码检测: %s\n", detected ? detected : "unknown");
    
    const char* sys_enc = nl_encoding_get_system_default();
    NL_PRINTF("✅ 系统默认编码: %s\n\n", sys_enc);
    
    NL_PRINTF("服务器已启动！\n\n");
    NL_PRINTF("访问地址:\n");
    NL_PRINTF("  首页:        http://localhost:8080/\n");
    NL_PRINTF("  计数器:      http://localhost:8080/counter\n");
    NL_PRINTF("  数据面板:    http://localhost:8080/dashboard\n");
    NL_PRINTF("  表单:        http://localhost:8080/form\n");
    NL_PRINTF("  待办:        http://localhost:8080/todo\n");
    NL_PRINTF("  聊天:        http://localhost:8080/chat\n");
    NL_PRINTF("  编码测试:    http://localhost:8080/encoding\n");
    NL_PRINTF("  内联Vue测试: http://localhost:8080/inline-vue\n\n");
    NL_PRINTF("按 Ctrl+C 停止...\n\n");
    
    // 保持运行
    while (1) {
        SLEEP(1000);
    }
    
    // 清理（不会执行到这里，自动清理会处理）
    nl_web_stop(server);
    nl_web_destroy(server);
    
    return 0;
}