# NetLeaf v2.0.0 - 高级服务器API

## 🚀 新功能概述

NetLeaf v2.0.0 带来了全新的高级服务器API，支持：
- ✅ 静态文件服务（HTML、Vue、React等）
- ✅ API路由配置
- ✅ 相对/绝对路径支持
- ✅ 异步/并发处理
- ✅ 可配置的端口
- ✅ 一行代码快速启动

## 📦 新增API

### 1. 静态文件服务器

#### 一行代码启动文件服务器
```c
#include "netleaf.h"

int main() {
    // 相对路径或绝对路径都支持
    nl_serve_files("./public", 8080);
    
    // 或者绝对路径
    // nl_serve_files("C:/mywebsite", 8080);
    
    while (1) {
        Sleep(1000); // Windows
    }
    return 0;
}
```

#### 完整的文件服务器API
```c
// 创建文件服务器
nl_file_server_t* server = nl_file_server_create("./website", 8080);

// 设置默认首页（默认是 index.html）
nl_file_server_set_index(server, "home.html");

// 启动服务器
nl_file_server_start(server);

// 停止服务器
nl_file_server_stop(server);

// 销毁服务器
nl_file_server_destroy(server);
```

### 2. API路由器

#### 创建和使用路由器
```c
#include "netleaf.h"

// 处理 GET /api/hello
void hello_handler(const char* path, nl_http_method_t method,
                  const char* body, size_t body_size,
                  char** response, size_t* response_size,
                  void* user_data) {
    const char* resp = "{\"message\":\"Hello, World!\"}";
    *response_size = strlen(resp);
    *response = (char*)malloc(*response_size + 1);
    strcpy(*response, resp);
}

// 处理 POST /api/data
void data_handler(const char* path, nl_http_method_t method,
                 const char* body, size_t body_size,
                 char** response, size_t* response_size,
                 void* user_data) {
    // 处理请求体...
    char* resp = (char*)malloc(256);
    *response_size = sprintf(resp, "{\"received\":\"%.*s\"}", (int)body_size, body);
    *response = resp;
}

int main() {
    // 创建路由器
    nl_router_t* router = nl_router_create();
    
    // 添加路由
    nl_router_add_route(router, "/api/hello", NL_METHOD_GET, hello_handler, NULL);
    nl_router_add_route(router, "/api/data", NL_METHOD_POST, data_handler, NULL);
    
    // 设置静态文件目录（可选）
    nl_router_set_static_dir(router, "./public");
    
    // 启动服务器
    nl_router_serve(router, 8080);
    
    // 运行...
    
    // 清理
    nl_router_destroy(router);
    return 0;
}
```

### 3. 简单服务器启动

```c
#include "netleaf.h"

void custom_handler(const char* path, nl_http_method_t method,
                   const char* body, size_t body_size,
                   char** response, size_t* response_size,
                   void* user_data) {
    const char* resp = "{\"status\":\"ok\"}";
    *response_size = strlen(resp);
    *response = (char*)malloc(*response_size + 1);
    strcpy(*response, resp);
}

int main() {
    // 一行代码启动，带自定义处理器
    nl_serve(8080, custom_handler, NULL);
    
    while (1) {
        Sleep(1000);
    }
    return 0;
}
```

## 📋 支持的HTTP方法

```c
typedef enum {
    NL_METHOD_GET,     // GET
    NL_METHOD_POST,    // POST
    NL_METHOD_PUT,     // PUT
    NL_METHOD_DELETE,  // DELETE
    NL_METHOD_PATCH,   // PATCH
    NL_METHOD_HEAD,    // HEAD
    NL_METHOD_OPTIONS  // OPTIONS
} nl_http_method_t;
```

## 📁 MIME类型支持

自动识别的文件类型：
- HTML/CSS/JS/JSON
- 图片（PNG, JPG, GIF, SVG, ICO）
- 文档（PDF, TXT, XML）
- 压缩包（ZIP）
- 其他（application/octet-stream）

## 🔒 安全特性

- ✅ 路径遍历防护（禁止访问父目录）
- ✅ 规范化路径处理
- ✅ Windows/Linux路径分隔符自动转换

## 📊 使用示例

### 示例1: 简单的HTML网站
```c
// 只需一行代码，托管整个网站
nl_serve_files("./my-website", 8080);
```

### 示例2: REST API + 静态文件
```c
nl_router_t* router = nl_router_create();

// API路由
nl_router_add_route(router, "/api/users", NL_METHOD_GET, get_users, NULL);
nl_router_add_route(router, "/api/users", NL_METHOD_POST, create_user, NULL);

// 静态文件
nl_router_set_static_dir(router, "./frontend/dist");

nl_router_serve(router, 3000);
```

### 示例3: Vue/React SPA
```c
// 为SPA配置路由和静态文件
nl_router_t* router = nl_router_create();

// API接口
nl_router_add_route(router, "/api/config", NL_METHOD_GET, get_config, NULL);

// 前端文件（Vue/React）
nl_router_set_static_dir(router, "./dist");

nl_router_serve(router, 8080);
```

## 🚀 完整示例代码

查看 `examples/simple_server.c` 获取完整的示例代码。

```bash
# 运行示例
cd build/bin/Windows/x64/Release
simple_server.exe ../../../../examples
```

## 📝 版本更新

- **v2.0.0**: 新增高级服务器API、文件服务、路由系统
- **v1.9.x**: TCP/UDP/HTTP/2/3基础支持

## 💡 提示

1. 所有路径支持相对路径和绝对路径
2. 所有网络操作都是异步的，使用多线程处理并发
3. 不需要外部依赖，只使用系统API
4. 跨平台支持（Windows/Linux）
