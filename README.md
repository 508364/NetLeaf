# NetLeaf v2.0.0

高性能跨平台网络库，支持TCP/UDP/HTTP/HTTP2/HTTP3，以及内联HTML/Vue响应式Web服务器。

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

### 3. 使用预设组件

```c
// 计数器
nl_web_add_counter(server, "/", "Counter Demo");

// 数据面板
nl_web_add_dashboard(server, "/dashboard", "Analytics");

// 表单
const char* fields[] = {"name", "email", "message"};
nl_web_add_form(server, "/contact", "Contact Us", fields, 3);
```

### 4. TCP服务器

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
