# 功能介绍 | Features

详细介绍 NetLeaf 2.0.0 的所有功能。

---

## 目录 | Table of Contents

- [HTTP 服务器](#http-服务器)
- [WebSocket 支持](#websocket-支持)
- [TCP/UDP 套接字](#tcpudp-套接字)
- [JSON/TOML 解析](#jsontoml-解析)
- [Web UI](#web-ui)
- [配置管理](#配置管理)
- [线程安全](#线程安全)

---

## HTTP 服务器 | HTTP Server

### HTTP/1.1

- 完整的 HTTP/1.1 协议实现
- 支持 GET、POST、PUT、DELETE、PATCH、HEAD、OPTIONS
- 请求解析与响应生成
- Keep-Alive 连接复用
- Gzip/Deflate 压缩
- 路由系统 (Routing)

### HTTP/2

- 多路复用 (Multiplexing)
- 服务器推送 (Server Push)
- 头部压缩 (HPACK)
- 流优先级

### HTTP/3

- QUIC 传输协议
- 0-RTT 握手
- 连接迁移
- 内置 TLS 加密

---

## WebSocket 支持 | WebSocket Support

- RFC 6455 标准兼容
- 文本与二进制消息
- 分片消息
- Ping/Pong 心跳
- 子协议协商
- 事件驱动 API

```c
void on_message(nl_websocket_t* ws, const char* msg, size_t len, int is_text) {
    printf("Received: %s\n", msg);
    nl_ws_send(ws, "Hello!", 6, 1);
}

int main() {
    nl_server_t* server = nl_create_server(8080);
    nl_add_websocket(server, "/ws", on_message);
    nl_start_server(server);
    return 0;
}
```

---

## TCP/UDP 套接字 | TCP/UDP Sockets

### TCP

- 异步 TCP 服务器
- 异步 TCP 客户端
- 连接池管理
- 非阻塞 I/O

### UDP

- UDP 服务器与客户端
- 数据包收发
- 广播/多播支持

---

## JSON/TOML 解析 | JSON/TOML Parsing

### JSON

```c
// 解析 JSON
nl_json_t* json = nl_json_parse("{\"key\":\"value\"}", NULL, NULL, NULL);
const char* val = nl_json_get_string(json, "key");
nl_json_free(json);
```

### TOML

```c
nl_toml_t* toml = nl_toml_parse_file("config.toml", NULL);
int port = (int)nl_toml_get_int(toml, "server.port");
nl_toml_free(toml);
```

---

## Web UI

### 功能

- Vue.js 3 + Tailwind CSS
- 响应式设计
- 实时连接监控
- API 测试控制台
- 配置编辑器
- 日志查看器

### 访问地址

- UI: http://localhost:8080/_netleaf
- WebSocket 测试: http://localhost:8080/_netleaf/ws

---

## 配置管理 | Configuration Management

- TOML 配置文件
- 运行时配置更新
- 环境变量覆盖

```toml
# config.toml
[server]
port = 8080
host = "0.0.0.0"

[http]
max_connections = 1000
keep_alive = true
```

---

## 线程安全 | Thread Safety

- 所有 API 线程安全
- 互斥锁与原子操作
- 线程池支持

---

## 彩蛋功能 | Easter Eggs

### 愚人节彩蛋 (April Fools' Day)

NetLeaf 包含一个有趣的愚人节彩蛋！当启用彩蛋功能后，在4月1日访问 `/tea` 路径将返回 HTTP 418 "I'm a teapot" 响应。

```c
#include "netleaf.h"

int main() {
    nl_file_server_t* server = nl_file_server_create("./public", 8080);
    nl_file_server_set_easter_egg(server, 1); // 启用彩蛋
    nl_file_server_start(server);
    return 0;
}
```

**触发条件：**
- 必须通过 `nl_file_server_set_easter_egg(server, 1)` 启用
- 当前日期必须是4月1日
- 访问路径必须是 `/tea`

**响应：**
- HTTP 状态码: `418 I'm a teapot`
- 自定义响应头: `X-Teapot: Yes, this is a teapot!`
- 精美HTML页面展示茶壶动画

---

[返回主页](./Home)

