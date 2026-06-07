# 示例代码 | Examples

NetLeaf 2.0.0 示例代码集合。

---

## 目录 | Table of Contents

- [基础服务器](#基础服务器)
- [HTTP 服务器](#http-服务器)
- [REST API 服务器](#rest-api-服务器)
- [Web 服务器 (预设组件)](#web-服务器-预设组件)
- [WebSocket 服务器](#websocket-服务器)
- [静态文件服务](#静态文件服务)
- [配置文件使用](#配置文件使用)
- [JSON 处理](#json-处理)

---

## 基础服务器 | Basic Server

### TCP 回显服务器

```c
#include "netleaf.h"
#include <stdio.h>

void on_data(const char* data, size_t len, char** response, size_t* response_size, void* user_data) {
    // 回显数据
    *response = (char*)malloc(len + 1);
    memcpy(*response, data, len);
    (*response)[len] = '\0';
    *response_size = len;
}

int main() {
    nl_server_t* server = nl_server_create(NL_PROTO_TCP, 8080);
    if (!server) {
        printf("Failed to create server\n");
        return 1;
    }
    
    nl_serve(8080, on_data, NULL);
    
    while (1) {
        Sleep(1000); // Windows
        // sleep(1); // Linux
    }
    
    return 0;
}
```

---

## HTTP 服务器 | HTTP Server

### 基础 HTTP 服务器

```c
#include "netleaf.h"
#include <stdio.h>

void handle_request(const char* path, nl_http_method_t method,
                   const char* body, size_t body_size,
                   char** response, size_t* response_size,
                   void* user_data) {
    
    const char* html = "<html>"
        "<head><title>NetLeaf Demo</title></head>"
        "<body>"
        "<h1>Welcome to NetLeaf!</h1>"
        "<p>Powered by NetLeaf 2.0.0</p>"
        "<p>Path: /</p>"
        "</body></html>";
    
    *response_size = strlen(html);
    *response = (char*)malloc(*response_size + 1);
    strcpy(*response, html);
}

int main() {
    nl_http_server_t* server = nl_http_server_create(8080);
    if (!server) {
        printf("Failed to create server\n");
        return 1;
    }
    
    nl_http_server_set_handler(server, handle_request, NULL);
    
    printf("Server running on http://localhost:8080\n");
    nl_http_server_start(server);
    
    return 0;
}
```

---

## REST API 服务器 | REST API Server

```c
#include "netleaf.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    int id;
    char name[64];
} User;

User users[10];
int user_count = 0;

void handle_request(const char* path, nl_http_method_t method,
                   const char* body, size_t body_size,
                   char** response, size_t* response_size,
                   void* user_data) {
    
    // GET /api/users
    if (strcmp(path, "/api/users") == 0 && method == NL_METHOD_GET) {
        char json[2048] = "[";
        for (int i = 0; i < user_count; i++) {
            char item[256];
            sprintf(item, "{\"id\":%d,\"name\":\"%s\"}", users[i].id, users[i].name);
            strcat(json, item);
            if (i < user_count - 1) strcat(json, ",");
        }
        strcat(json, "]");
        
        *response_size = strlen(json);
        *response = (char*)malloc(*response_size + 1);
        strcpy(*response, json);
        return;
    }
    
    // POST /api/users
    if (strcmp(path, "/api/users") == 0 && method == NL_METHOD_POST) {
        if (user_count < 10) {
            users[user_count].id = user_count + 1;
            strncpy(users[user_count].name, "User", 63);
            user_count++;
            
            const char* resp = "{\"status\":\"created\"}";
            *response_size = strlen(resp);
            *response = (char*)malloc(*response_size + 1);
            strcpy(*response, resp);
        } else {
            const char* resp = "{\"error\":\"too many users\"}";
            *response_size = strlen(resp);
            *response = (char*)malloc(*response_size + 1);
            strcpy(*response, resp);
        }
        return;
    }
    
    // Default response
    const char* resp = "{\"error\":\"not found\"}";
    *response_size = strlen(resp);
    *response = (char*)malloc(*response_size + 1);
    strcpy(*response, resp);
}

int main() {
    nl_http_server_t* server = nl_http_server_create(8080);
    nl_http_server_set_handler(server, handle_request, NULL);
    nl_http_server_start(server);
    return 0;
}
```

---

## Web 服务器 (预设组件) | Web Server (Preset Components)

### 使用预设组件

```c
#include "netleaf.h"

int main() {
    nl_web_server_t* server = nl_web_create(8080);
    
    // 添加计数器
    nl_web_add_counter(server, "/counter", "Page Views");
    
    // 添加数据面板
    nl_web_add_dashboard(server, "/dashboard", "Analytics");
    
    // 添加表单
    const char* fields[] = {"name", "email", "message"};
    nl_web_add_form(server, "/contact", "Contact Us", fields, 3);
    
    // 添加待办事项
    nl_web_add_todo(server, "/todo", "My Tasks");
    
    // 添加聊天室
    nl_web_add_chat(server, "/chat", "Live Chat");
    
    nl_web_start(server);
    return 0;
}
```

### 自定义 HTML 页面

```c
#include "netleaf.h"

int main() {
    nl_web_server_t* server = nl_web_create(8080);
    
    nl_web_add_html(server, "/", 
        "<!DOCTYPE html>"
        "<html><body>"
        "<h1>Hello NetLeaf!</h1>"
        "<p>Welcome to my custom page</p>"
        "</body></html>");
    
    nl_web_start(server);
    return 0;
}
```

---

## WebSocket 服务器 | WebSocket Server

### 基础 WebSocket 服务器

```c
#include "netleaf.h"
#include <stdio.h>

void on_message(const char* data, size_t len, nl_ws_opcode_t opcode, void* user_data) {
    printf("Received: %.*s\n", (int)len, data);
}

int main() {
    nl_websocket_server_t* server = nl_ws_server_create(8080);
    if (!server) {
        printf("Failed to create server\n");
        return 1;
    }
    
    nl_ws_server_set_on_message(server, on_message, NULL);
    
    printf("WebSocket server on ws://localhost:8080\n");
    nl_ws_server_start(server);
    
    return 0;
}
```

---

## 静态文件服务 | Static File Serving

### 一行代码服务静态文件

```c
#include "netleaf.h"

int main() {
    // 服务当前目录的静态文件
    nl_serve_files("./public", 8080);
    return 0;
}
```

### 路由器 + 静态文件

```c
#include "netleaf.h"

void api_handler(const char* path, nl_http_method_t method,
                const char* body, size_t body_size,
                char** response, size_t* response_size,
                void* user_data) {
    
    const char* resp = "{\"message\":\"Hello from API!\"}";
    *response_size = strlen(resp);
    *response = (char*)malloc(*response_size + 1);
    strcpy(*response, resp);
}

int main() {
    nl_router_t* router = nl_router_create();
    
    // 添加 API 路由
    nl_router_add_route(router, "/api/hello", NL_METHOD_GET, api_handler, NULL);
    
    // 设置静态文件目录
    nl_router_set_static_dir(router, "./public");
    
    // 启动服务器
    nl_router_serve(router, 8080);
    
    return 0;
}
```

---

## 配置文件使用 | Configuration File Usage

**config.toml**
```toml
[server]
port = 8080
host = "0.0.0.0"

[http]
max_connections = 1000
```

**main.c**
```c
#include "netleaf.h"
#include <stdio.h>

int main() {
    nl_status_t err;
    nl_toml_t* config = nl_toml_parse_file("config.toml", &err);
    if (!config) {
        printf("Failed to parse config\n");
        return 1;
    }
    
    int port = (int)nl_toml_get_int(config, "server.port");
    nl_toml_free(config);
    
    nl_http_server_t* server = nl_http_server_create(port);
    nl_http_server_start(server);
    
    return 0;
}
```

---

## JSON 处理 | JSON Processing

```c
#include "netleaf.h"
#include <stdio.h>

int main() {
    const char* json_str = "{\"name\":\"NetLeaf\",\"version\":\"2.0.0\",\"features\":[\"http\",\"websocket\"]}";
    
    nl_json_t* json = nl_json_parse(json_str, NULL, NULL, NULL);
    if (json) {
        printf("Name: %s\n", nl_json_get_string(json, "name"));
        printf("Version: %s\n", nl_json_get_string(json, "version"));
        nl_json_free(json);
    }
    
    return 0;
}
```

---

## 一行代码快速启动

### 启动数据面板

```c
#include "netleaf.h"

int main() {
    nl_serve_dashboard(8080, "My Dashboard");
    while (1) Sleep(1000);
    return 0;
}
```

### 启动文件服务器

```c
#include "netleaf.h"

int main() {
    nl_serve_files("./public", 8080);
    while (1) Sleep(1000);
    return 0;
}
```

---

[返回主页](./Home)
