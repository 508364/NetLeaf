# NetLeaf v2.0.0 - 快速开始指南

## 目录

1. [编译和安装](#1-编译和安装)
2. [5分钟快速上手指南](#2-5分钟快速上手)
3. [API速查表](#3-api速查表)

---

## 1. 编译和安装

### Windows

在 Visual Studio Developer Command Prompt 中运行：

```cmd
build.bat
```

或构建全架构：

```cmd
build_all_windows.bat
```

### Linux/WSL

```bash
chmod +x build.sh
./build.sh
```

### 输出位置

编译完成后，库文件位于：

```
build/
├── bin/Release/
│   └── netleaf.dll (Windows) 或 libnetleaf.so (Linux)
└── lib/Release/
    └── netleaf.lib (Windows) 或 libnetleaf.a (Linux)
```

---

## 2. 5分钟快速上手

### 第1步：一行代码启动服务器

```c
#include <netleaf.h>

int main() {
    nl_serve_dashboard(8080, "我的数据面板");
    while (1) Sleep(1000);
    return 0;
}
```

访问 http://localhost:8080 即可看到界面！

---

### 第2步：创建自定义网页

```c
#include <netleaf.h>

int main() {
    nl_web_server_t* server = nl_web_create(8080);
    
    nl_web_add_html(server, "/", 
        "<!DOCTYPE html>"
        "<html><body>"
        "<h1>你好，NetLeaf！</h1>"
        "</body></html>");
    
    nl_web_start(server);
    
    while (1) Sleep(1000);
    nl_web_destroy(server);
    return 0;
}
```

---

### 第3步：使用预设组件

```c
nl_web_add_counter(server, "/count", "我的计数器");

nl_web_add_dashboard(server, "/dashboard", "数据分析面板");

const char* fields[] = {"name", "email", "message"};
nl_web_add_form(server, "/contact", "联系表单", fields, 3);
```

---

### 第4步：添加API路由

```c
nl_router_t* router = nl_router_create();

/* 添加API */
nl_router_add_route(router, "/api/hello", NL_METHOD_GET, my_handler, NULL);

/* 设置静态文件 */
nl_router_set_static_dir(router, "./public");

/* 启动 */
nl_router_serve(router, 8080);
```

---

### 第5步：TCP服务器

```c
void on_data(nl_conn_t* conn, const char* data, size_t len) {
    nl_conn_send(conn, data, len); /* 回显数据 */
}

int main() {
    nl_server_t* server = nl_server_create(NL_PROTO_TCP, 9000);
    nl_server_set_handler(server, on_data, NULL);
    nl_server_start(server);
    
    while (1) Sleep(1000);
    return 0;
}
```

---

## 3. API速查表

### Web服务器API

| 函数 | 说明 |
|------|------|
| `nl_web_create(port)` | 创建Web服务器 |
| `nl_web_start(server)` | 启动Web服务器 |
| `nl_web_stop(server)` | 停止Web服务器 |
| `nl_web_destroy(server)` | 释放服务器资源 |
| `nl_web_add_html(server, path, html)` | 添加HTML页面 |
| `nl_web_add_json(server, path, json)` | 添加JSON API |
| `nl_web_add_counter(server, path, title)` | 添加计数器组件 |
| `nl_web_add_dashboard(server, path, title)` | 添加数据面板 |
| `nl_web_add_form(server, path, title, fields, count)` | 添加表单组件 |
| `nl_serve_html(port, html)` | 快速提供HTML |
| `nl_serve_dashboard(port, title)` | 快速启动数据面板 |

### TCP/UDP服务器API

| 函数 | 说明 |
|------|------|
| `nl_server_create(proto, port)` | 创建服务器 |
| `nl_serve(port, handler, data)` | 一行代码启动服务器 |
| `nl_serve_files(dir, port)` | 启动文件服务器 |
| `nl_serve_dashboard(port, title)` | 启动数据面板 |

### 文件服务器API

| 函数 | 说明 |
|------|------|
| `nl_serve_files(dir, port)` | 一行代码启动文件服务器 |
| `nl_router_create()` | 创建路由器 |
| `nl_router_add_route(router, path, method, handler, data)` | 添加路由 |
| `nl_router_set_static_dir(router, dir)` | 设置静态文件目录 |
| `nl_router_serve(router, port)` | 启动路由服务器 |

### 协议类型

| 常量 | 说明 |
|------|------|
| `NL_PROTO_TCP` | TCP协议 |
| `NL_PROTO_UDP` | UDP协议 |
| `NL_PROTO_HTTP` | HTTP协议 |

### HTTP方法

| 常量 | 说明 |
|------|------|
| `NL_METHOD_GET` | GET请求 |
| `NL_METHOD_POST` | POST请求 |
| `NL_METHOD_PUT` | PUT请求 |
| `NL_METHOD_DELETE` | DELETE请求 |

---

## 4. 编译你的项目

### Windows (MSVC)

```cmd
cl your_app.c /I NetLeaf/include /link NetLeaf/build/lib/Release/netleaf.lib ws2_32.lib
```

### Linux (GCC)

```bash
gcc your_app.c -I NetLeaf/include -L NetLeaf/build/lib -lnetleaf -lpthread -o your_app
```

---

## 5. 完整示例代码

查看 `examples/` 目录中的示例代码：

| 文件 | 说明 |
|------|------|
| `example_basic_web.c` | 基础Web服务器示例 |
| `example_tcp_server.c` | TCP回显服务器 |

---

## 需要更多帮助？

- 查看 [README.md](README.md) 了解完整功能
- 查看代码注释获得详细说明
