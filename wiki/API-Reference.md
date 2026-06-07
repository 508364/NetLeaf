# API 参考 | API Reference

NetLeaf 2.0.0 完整 API 文档。

---

## 目录 | Table of Contents

- [服务器 API](#服务器-api)
- [HTTP 服务器 API](#http-服务器-api)
- [Web 服务器 API](#web-服务器-api)
- [路由器 API](#路由器-api)
- [文件服务 API](#文件服务-api)
- [WebSocket API](#websocket-api)
- [JSON API](#json-api)
- [TOML API](#toml-api)
- [工具函数](#工具函数)

---

## 服务器 API | Server API

### nl_server_create

创建 TCP/UDP 服务器实例。

```c
nl_server_t* nl_server_create(nl_protocol_t protocol, int port);
```

| 参数 | 类型 | 描述 |
|------|------|------|
| protocol | nl_protocol_t | 协议类型 (NL_PROTO_TCP, NL_PROTO_UDP) |
| port | int | 监听端口 |

**返回:** 服务器指针，失败返回 NULL。

**示例:**
```c
nl_server_t* server = nl_server_create(NL_PROTO_TCP, 8080);
```

---

### nl_server_destroy

释放服务器资源。

```c
void nl_server_destroy(nl_server_t* server);
```

---

### nl_server_start

启动服务器（阻塞）。

```c
int nl_server_start(nl_server_t* server);
```

**返回:** 成功返回 0，失败返回 -1。

---

### nl_server_stop

停止服务器。

```c
void nl_server_stop(nl_server_t* server);
```

---

### 协议类型

```c
typedef enum {
    NL_PROTO_TCP,  // TCP 协议
    NL_PROTO_UDP   // UDP 协议
} nl_protocol_t;
```

---

## HTTP 服务器 API | HTTP Server API

### nl_http_server_create

创建 HTTP 服务器实例。

```c
nl_http_server_t* nl_http_server_create(int port);
```

| 参数 | 类型 | 描述 |
|------|------|------|
| port | int | 监听端口 |

**返回:** HTTP 服务器指针，失败返回 NULL。

---

### nl_http_server_destroy

释放 HTTP 服务器资源。

```c
void nl_http_server_destroy(nl_http_server_t* server);
```

---

### nl_http_server_start

启动 HTTP 服务器（阻塞）。

```c
int nl_http_server_start(nl_http_server_t* server);
```

---

### nl_http_server_stop

停止 HTTP 服务器。

```c
void nl_http_server_stop(nl_http_server_t* server);
```

---

### nl_http_server_set_handler

设置请求处理回调。

```c
typedef void (*nl_http_handler)(const char* path, nl_http_method_t method,
                                const char* body, size_t body_size,
                                char** response, size_t* response_size,
                                void* user_data);

void nl_http_server_set_handler(nl_http_server_t* server,
                                nl_http_handler handler,
                                void* user_data);
```

**示例:**
```c
void handle_request(const char* path, nl_http_method_t method,
                    const char* body, size_t body_size,
                    char** response, size_t* response_size,
                    void* user_data) {
    const char* resp = "{\"message\":\"Hello!\"}";
    *response_size = strlen(resp);
    *response = (char*)malloc(*response_size + 1);
    strcpy(*response, resp);
}

nl_http_server_set_handler(server, handle_request, NULL);
```

---

### nl_http_server_enable_http2

启用 HTTP/2 支持。

```c
void nl_http_server_enable_http2(nl_http_server_t* server, int enable);
```

---

### nl_http_server_enable_http3

启用 HTTP/3 支持。

```c
void nl_http_server_enable_http3(nl_http_server_t* server, int enable);
```

---

## Web 服务器 API | Web Server API

Web 服务器提供现代化的 Web UI 支持，包括预设组件。

### nl_web_create

创建 Web 服务器实例。

```c
nl_web_server_t* nl_web_create(int port);
```

---

### nl_web_destroy

释放 Web 服务器资源。

```c
void nl_web_destroy(nl_web_server_t* server);
```

---

### nl_web_start

启动 Web 服务器（阻塞）。

```c
int nl_web_start(nl_web_server_t* server);
```

---

### nl_web_add_html

添加自定义 HTML 页面。

```c
void nl_web_add_html(nl_web_server_t* server,
                     const char* path,
                     const char* html);
```

---

### nl_web_add_vue

添加 Vue 组件页面。

```c
void nl_web_add_vue(nl_web_server_t* server,
                     const char* path,
                     const char* vue_code);
```

---

### nl_web_add_json

添加 JSON API 端点。

```c
void nl_web_add_json(nl_web_server_t* server,
                     const char* path,
                     const char* json);
```

---

### nl_web_add_counter

添加计数器组件。

```c
void nl_web_add_counter(nl_web_server_t* server,
                         const char* path,
                         const char* title);
```

---

### nl_web_add_dashboard

添加数据面板组件。

```c
void nl_web_add_dashboard(nl_web_server_t* server,
                           const char* path,
                           const char* title);
```

---

### nl_web_add_form

添加表单组件。

```c
void nl_web_add_form(nl_web_server_t* server,
                      const char* path,
                      const char* title,
                      const char** fields,
                      int field_count);
```

**示例:**
```c
const char* fields[] = {"name", "email", "message"};
nl_web_add_form(server, "/contact", "Contact Us", fields, 3);
```

---

### nl_web_add_todo

添加待办事项组件。

```c
void nl_web_add_todo(nl_web_server_t* server,
                      const char* path,
                      const char* title);
```

---

### nl_web_add_chat

添加聊天室组件。

```c
void nl_web_add_chat(nl_web_server_t* server,
                      const char* path,
                      const char* title);
```

---

### nl_web_add_gallery

添加图片画廊组件。

```c
void nl_web_add_gallery(nl_web_server_t* server,
                          const char* path,
                          const char* title,
                          const char** image_urls,
                          int count);
```

---

### nl_serve_dashboard

一行代码启动数据面板。

```c
int nl_serve_dashboard(int port, const char* title);
```

**示例:**
```c
nl_serve_dashboard(8080, "My Dashboard");
```

---

## 路由器 API | Router API

路由器提供灵活的路由配置和静态文件服务。

### nl_router_create

创建路由器实例。

```c
nl_router_t* nl_router_create(void);
```

---

### nl_router_destroy

释放路由器资源。

```c
void nl_router_destroy(nl_router_t* router);
```

---

### nl_router_add_route

添加自定义路由。

```c
typedef void (*nl_http_handler_t)(const char* path, nl_http_method_t method,
                                   const char* body, size_t body_size,
                                   char** response, size_t* response_size,
                                   void* user_data);

void nl_router_add_route(nl_router_t* router,
                         const char* path,
                         nl_http_method_t method,
                         nl_http_handler_t handler,
                         void* user_data);
```

**示例:**
```c
void hello_handler(const char* path, nl_http_method_t method,
                   const char* body, size_t body_size,
                   char** response, size_t* response_size,
                   void* user_data) {
    const char* resp = "{\"message\":\"Hello!\"}";
    *response_size = strlen(resp);
    *response = (char*)malloc(*response_size + 1);
    strcpy(*response, resp);
}

nl_router_add_route(router, "/api/hello", NL_METHOD_GET, hello_handler, NULL);
```

---

### nl_router_set_static_dir

设置静态文件目录。

```c
void nl_router_set_static_dir(nl_router_t* router, const char* directory);
```

**示例:**
```c
nl_router_set_static_dir(router, "./public");
```

---

### nl_router_serve

启动路由服务器。

```c
void nl_router_serve(nl_router_t* router, int port);
```

---

### nl_serve

一行代码启动简单服务器。

```c
int nl_serve(int port, nl_http_handler_t default_handler, void* user_data);
```

---

## HTTP 方法类型

```c
typedef enum {
    NL_METHOD_GET,
    NL_METHOD_POST,
    NL_METHOD_PUT,
    NL_METHOD_DELETE,
    NL_METHOD_PATCH,
    NL_METHOD_HEAD,
    NL_METHOD_OPTIONS
} nl_http_method_t;
```

---

## 文件服务 API | File Server API

### nl_serve_files

一行代码启动静态文件服务器。

```c
int nl_serve_files(const char* directory, int port);
```

**示例:**
```c
// 服务当前目录
nl_serve_files("./public", 8080);

// 或绝对路径
nl_serve_files("C:/mywebsite", 8080);
```

---

## WebSocket API

### nl_ws_server_create

创建 WebSocket 服务器实例。

```c
nl_websocket_server_t* nl_ws_server_create(int port);
```

---

### nl_ws_server_destroy

释放 WebSocket 服务器资源。

```c
void nl_ws_server_destroy(nl_websocket_server_t* server);
```

---

### nl_ws_server_start

启动 WebSocket 服务器。

```c
int nl_ws_server_start(nl_websocket_server_t* server);
```

---

### nl_ws_server_stop

停止 WebSocket 服务器。

```c
void nl_ws_server_stop(nl_websocket_server_t* server);
```

---

### nl_ws_server_set_on_connect

设置连接回调。

```c
typedef void (*nl_ws_connect_handler)(void* user_data);

void nl_ws_server_set_on_connect(nl_websocket_server_t* server,
                                  nl_ws_connect_handler handler,
                                  void* user_data);
```

---

### nl_ws_server_set_on_message

设置消息回调。

```c
typedef void (*nl_ws_message_handler)(const char* data, size_t len,
                                       nl_ws_opcode_t opcode,
                                       void* user_data);

void nl_ws_server_set_on_message(nl_websocket_server_t* server,
                                  nl_ws_message_handler handler,
                                  void* user_data);
```

---

### nl_ws_server_set_on_close

设置关闭回调。

```c
typedef void (*nl_ws_close_handler)(void* user_data);

void nl_ws_server_set_on_close(nl_websocket_server_t* server,
                                 nl_ws_close_handler handler,
                                 void* user_data);
```

---

### nl_ws_server_broadcast

广播消息到所有连接。

```c
int nl_ws_server_broadcast(nl_websocket_server_t* server,
                            const char* data,
                            size_t len,
                            nl_ws_opcode_t opcode);
```

---

### nl_ws_server_send_text

发送文本消息。

```c
int nl_ws_server_send_text(nl_websocket_server_t* server,
                            const char* data,
                            size_t len);
```

---

### nl_ws_server_send_binary

发送二进制消息。

```c
int nl_ws_server_send_binary(nl_websocket_server_t* server,
                               const void* data,
                               size_t len);
```

---

### WebSocket 操作码

```c
typedef enum {
    NL_WS_CONTINUATION = 0x0,
    NL_WS_TEXT = 0x1,
    NL_WS_BINARY = 0x2,
    NL_WS_CLOSE = 0x8,
    NL_WS_PING = 0x9,
    NL_WS_PONG = 0xA
} nl_ws_opcode_t;
```

---

## JSON API

### nl_json_parse

解析 JSON 字符串。

```c
nl_json_t* nl_json_parse(const char* json_str,
                         nl_status_t* error_code,
                         int* error_line,
                         int* error_col);
```

---

### nl_json_parse_file

解析 JSON 文件。

```c
nl_json_t* nl_json_parse_file(const char* file_path, nl_status_t* error_code);
```

---

### nl_json_get_string

获取字符串值。

```c
const char* nl_json_get_string(nl_json_t* json, const char* key);
```

---

### nl_json_get_int

获取整数值。

```c
int64_t nl_json_get_int(nl_json_t* json, const char* key);
```

---

### nl_json_get_double

获取浮点数值。

```c
double nl_json_get_double(nl_json_t* json, const char* key);
```

---

### nl_json_get_bool

获取布尔值。

```c
int nl_json_get_bool(nl_json_t* json, const char* key);
```

---

### nl_json_free

释放 JSON 对象。

```c
void nl_json_free(nl_json_t* json);
```

---

## TOML API

### nl_toml_parse

解析 TOML 字符串。

```c
nl_toml_t* nl_toml_parse(const char* toml_str,
                         nl_status_t* error_code,
                         int* error_line,
                         int* error_col);
```

---

### nl_toml_parse_file

解析 TOML 配置文件。

```c
nl_toml_t* nl_toml_parse_file(const char* file_path, nl_status_t* error_code);
```

---

### nl_toml_get_int

获取整数值。

```c
int64_t nl_toml_get_int(nl_toml_t* toml, const char* key);
```

---

### nl_toml_get_string

获取字符串值。

```c
const char* nl_toml_get_string(nl_toml_t* toml, const char* key);
```

---

### nl_toml_get_bool

获取布尔值。

```c
int nl_toml_get_bool(nl_toml_t* toml, const char* key);
```

---

### nl_toml_free

释放 TOML 对象。

```c
void nl_toml_free(nl_toml_t* toml);
```

---

## 工具函数 | Utility Functions

### nl_version

获取 NetLeaf 版本。

```c
const char* nl_version(void);
```

---

### nl_set_log_level

设置日志级别。

```c
void nl_set_log_level(int level);
```

| 级别 | 值 | 描述 |
|------|----|------|
| NL_LOG_DEBUG | 0 | 调试信息 |
| NL_LOG_INFO | 1 | 一般信息 |
| NL_LOG_WARN | 2 | 警告 |
| NL_LOG_ERROR | 3 | 错误 |

---

[返回主页](./Home)
