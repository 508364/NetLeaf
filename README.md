# NetLeaf v2.1.0

![NetLeaf Logo](Logo.svg)

高性能跨平台网络库，支持TCP/UDP/HTTP/HTTP2/HTTP3，以及内联HTML/Vue响应式Web服务器。

## 新增功能 (v2.1.0)

- ✅ **简化启动**: `nl_web_create(port)` 创建即启动，无需单独调用 `nl_web_start`
- ✅ **按端口停止**: `nl_web_stop_by_port(port)` 支持通过端口号停止服务器
- ✅ **自动清理**: `nl_web_set_auto_cleanup(1)` 程序退出时自动释放所有服务器资源
- ✅ **变量硬编码**: 服务器端变量替换，用户F12无法看到原始变量名
- ✅ **自定义编码**: 支持设置响应编码（UTF-8等）
- ✅ **完整日志系统**: 支持DEBUG/INFO/WARN/ERROR级别
- ✅ **自动交叉编译检测**: Linux脚本自动检测可用工具链
- ✅ **Debug模式**: 一键启用调试日志
- ✅ **异步并发优化**: 完整的异步IO支持，优化性能消耗

## 快速开始

### Windows

在 Visual Studio Developer Command Prompt 中运行：

```cmd
build.bat
```

或指定架构：

```cmd
build.bat x86
build.bat arm64
```

### Linux/WSL

```bash
chmod +x build.sh
./build.sh
```

或指定架构：

```bash
./build.sh x86
./build.sh arm
./build.sh arm64
```

### 全架构构建

```bash
# Linux - 自动检测可用交叉编译工具链
./build_all_linux.sh

# Windows
build_all_windows.bat
```

## 输出位置

构建完成后：

```
build/
├── bin/Release/netleaf.dll (Windows) | libnetleaf.so (Linux)
└── lib/Release/netleaf.lib (Windows) | libnetleaf.a (Linux)
```

## 使用示例

### 1. 一行代码启动Web服务器

```c
#include "netleaf.h"

int main() {
    nl_serve_dashboard(8080, "My Dashboard");
    
    while (1) {
        Sleep(1000);  // Windows
        // sleep(1);  // Linux
    }
    return 0;
}
```

### 2. 内联HTML

```c
#include "netleaf.h"

int main() {
    nl_web_server_t* server = nl_web_create(8080);
    nl_web_add_html(server, "/", 
        "<!DOCTYPE html>"
        "<html><body>"
        "<h1>Hello NetLeaf!</h1>"
        "</body></html>");
    nl_web_start(server);
    
    while (1) Sleep(1000);
    nl_web_destroy(server);
    return 0;
}
```

### 3. 变量硬编码（新增）

**变量在服务器端替换，用户F12看不到原始变量名：**

```c
#include "netleaf.h"

int main() {
    nl_web_server_t* server = nl_web_create(8080);
    
    // 设置编码
    nl_web_set_encoding(server, "UTF-8");
    
    // 定义变量
    const char* vars[] = {"username", "user_id"};
    const char* values[] = {"张三", "12345"};
    
    // 添加带变量的页面
    nl_web_add_html_with_vars(server, "/profile",
        "<h1>欢迎, {{<var>username</var>}}</h1>"
        "<p>用户ID: {{<var>user_id</var>}}</p>",
        vars, values, 2);
    
    nl_web_start(server);
    while (1) Sleep(1000);
    return 0;
}
```

**输出到客户端：**
```html
<h1>欢迎, 张三</h1>
<p>用户ID: 12345</p>
```

### 4. 使用预设组件

```c
// 计数器
nl_web_add_counter(server, "/", "Counter Demo");

// 数据面板
nl_web_add_dashboard(server, "/dashboard", "Analytics");

// 表单
const char* fields[] = {"name", "email", "message"};
nl_web_add_form(server, "/contact", "Contact Us", fields, 3);
```

### 5. TCP服务器

```c
#include "netleaf.h"
#include <stdio.h>

void on_data(const char* data, size_t len, char** response, size_t* response_size, void* user_data) {
    *response = (char*)malloc(len + 1);
    memcpy(*response, data, len);
    (*response)[len] = '\0';
    *response_size = len;
}

int main() {
    nl_server_t* server = nl_server_create(NL_PROTO_TCP, 8080);
    nl_serve(8080, on_data, NULL);
    
    while (1) Sleep(1000);
    return 0;
}
```

## 更新日志

### v2.1.0

**新增功能:**
- `nl_web_create(port)` - 创建即启动Web服务器
- `nl_web_stop_by_port(port)` - 按端口号停止服务器
- `nl_web_set_auto_cleanup(enable)` - 程序退出时自动清理所有服务器
- `nl_web_set_encoding()` - 设置响应编码
- `nl_web_add_html_with_vars()` - 带变量的HTML页面
- `nl_web_add_vue_with_vars()` - 带变量的Vue页面
- `nl_debug_enable()` - 启用调试模式
- `nl_log_debug()`, `nl_log_info()`, `nl_log_warn()`, `nl_log_error()` - 日志函数
- 自动检测交叉编译工具链
- 异步并发优化，完整的异步IO支持

**Bug修复:**
- 修复服务器端口冲突问题（重复创建相同端口返回现有实例）
- 修复内存泄漏风险（通过`atexit`注册自动清理）
- 修复多线程安全问题（全局服务器列表添加互斥锁保护）

**安全更新:**
- 完善的空指针检查
- 缓冲区溢出防护
- 字符串格式化安全

### v2.0.0
- 初始版本发布
- 支持HTTP/1.1, HTTP/2, HTTP/3
- WebSocket支持

## Wiki - 最佳实践

### 推荐配置

#### 1. 启用自动清理（强烈推荐）

为了防止程序退出时内存泄漏，建议在程序初始化时启用自动清理功能：

```c
#include "netleaf.h"

int main() {
    // 启用程序退出时自动清理所有服务器资源
    nl_web_set_auto_cleanup(1);
    
    // 创建服务器（创建即启动）
    nl_web_server_t* server = nl_web_create(8080);
    
    // ... 添加路由和业务逻辑 ...
    
    while (1) {
        // 主循环
    }
    
    // 程序退出时会自动调用 nl_web_destroy 清理所有服务器
    return 0;
}
```

**为什么推荐启用：**
- ✅ 防止内存泄漏
- ✅ 自动释放所有服务器资源（socket、线程、内存）
- ✅ 确保程序优雅退出
- ✅ 特别适合守护进程或服务类应用

**默认行为：**
- 默认关闭（`nl_web_set_auto_cleanup(0)`）
- 如需手动管理，使用 `nl_web_destroy(server)` 或 `nl_web_stop_by_port(port)`

#### 2. 多服务器管理

```c
// 创建多个服务器
nl_web_server_t* server1 = nl_web_create(8080);
nl_web_server_t* server2 = nl_web_create(8081);

// 按端口停止指定服务器
nl_web_stop_by_port(8080);

// 程序退出时自动清理所有剩余服务器
```

#### 3. 编译选项

**Debug模式（开发阶段）：**
```c
nl_debug_enable();  // 启用详细日志输出
```

**Release模式（生产环境）：**
```c
// 默认关闭调试日志，仅输出错误信息
```

#### 4. 内联HTML/Vue支持

### 5. 文件服务

```c
// 一行代码服务静态文件
nl_serve_files("./public", 8080);
```

### 6. API路由

```c
nl_router_t* router = nl_router_create();
nl_router_add_route(router, "/api/hello", NL_METHOD_GET, my_handler, NULL);
nl_router_set_static_dir(router, "./public");
nl_router_serve(router, 8080);
```

## 链接你的项目

### Windows (MSVC)

```cmd
cl myapp.c /I include /link build/lib/Release/netleaf.lib ws2_32.lib
```

### Linux (GCC)

```bash
gcc myapp.c -I include -L build/lib -lnetleaf -lpthread -o myapp
```

## 功能特性

✅ **TCP/UDP** - 完整的网络协议支持  
✅ **HTTP/1.1, HTTP/2, HTTP/3** - 全HTTP协议栈  
✅ **内联HTML/Vue** - 直接在代码中嵌入响应式界面  
✅ **预设组件** - 计数器、面板、表单等开箱即用  
✅ **现代CSS** - 美观的渐变和动画效果  
✅ **跨平台** - Windows (IOCP) + Linux (epoll)  
✅ **宽可用库** - 简洁统一的输出结构  
✅ **无外部依赖** - 仅使用系统API  
✅ **多架构支持** - x86, x64, ARM, ARM64

## API参考

### Web服务器API

```c
// 创建服务器
nl_web_server_t* nl_web_create(int port);
void nl_web_destroy(nl_web_server_t* server);
int nl_web_start(nl_web_server_t* server);
void nl_web_stop(nl_web_server_t* server);

// 添加路由
void nl_web_add_html(nl_web_server_t* server, const char* path, const char* html);
void nl_web_add_vue(nl_web_server_t* server, const char* path, const char* vue_code);
void nl_web_add_json(nl_web_server_t* server, const char* path, const char* json);

// 预设组件
void nl_web_add_counter(nl_web_server_t* server, const char* path, const char* title);
void nl_web_add_dashboard(nl_web_server_t* server, const char* path, const char* title);
void nl_web_add_form(nl_web_server_t* server, const char* path, const char* title, const char** fields, int field_count);

// 一行代码启动
int nl_serve_html(int port, const char* html);
int nl_serve_vue(int port, const char* vue_code);
int nl_serve_dashboard(int port, const char* title);
```

更多API请查看 `include/netleaf.h`。
