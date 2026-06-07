# 快速入门 | Quickstart

本指南将帮助您在 5 分钟内启动并运行 NetLeaf。

---

## 前置条件 | Prerequisites

| 平台 | 要求 |
|------|------|
| **Windows** | Visual Studio 2022, CMake 3.15+ |
| **Linux** | GCC 8+, CMake 3.15+, Make |

---

## 第一步：获取代码 | Step 1: Get the Code

```bash
git clone https://github.com/solo-agent/NetLeaf.git
cd NetLeaf
```

---

## 第二步：构建 | Step 2: Build

### Windows

```powershell
# x64
.\build_all_windows.bat

# 或指定单个架构
.\build.bat x64
```

### Linux

```bash
chmod +x build_all_linux.sh
./build_all_linux.sh

# 或使用 WSL
.\build_wsl.sh
```

构建完成后，您将在 `build_<arch>/bin/Release/` 或 `build_wsl/bin/` 中找到编译好的库文件。

---

## 第三步：运行示例 | Step 3: Run Examples

### 基础 HTTP 服务器

```c
// examples/simple_server.c
#include "netleaf.h"

int main() {
    // 创建监听 8080 端口的服务器
    nl_server_t* server = nl_create_server(8080);
    if (!server) {
        printf("Failed to create server\n");
        return 1;
    }

    // 添加路由
    nl_add_get_route(server, "/", "text/html",
        "<html><body>"
        "<h1>Welcome to NetLeaf!</h1>"
        "<p>Powered by NetLeaf 2.0.0</p>"
        "</body></html>");

    nl_add_get_route(server, "/api", "application/json",
        "{\"message\":\"Hello from NetLeaf API\",\"version\":\"2.0.0\"}");

    // 启动服务器
    printf("Server started on http://localhost:8080\n");
    nl_start_server(server);

    return 0;
}
```

### 编译并运行

**Windows:**
```powershell
cd build_x64
cmake --build . --config Release --target simple_server
.\bin\Release\simple_server.exe
```

**Linux:**
```bash
cd build_wsl
make simple_server
./bin/simple_server
```

---

## 第四步：访问 Web UI | Step 4: Access the Web UI

启动服务器后，访问以下地址查看内置的管理界面：

- **HTTP UI**: http://localhost:8080/_netleaf
- **WebSocket 测试**: http://localhost:8080/_netleaf/ws

Web UI 提供：
- 实时连接监控
- API 测试控制台
- 配置管理
- 日志查看

---

## 更多示例 | More Examples

NetLeaf 提供了丰富的示例代码，位于 [examples](../examples/) 目录：

| 示例 | 描述 |
|------|------|
| `simple_server.c` | TCP 回显服务器 |
| `http_server.c` | 基础 HTTP 服务器 |
| `http1_server.c` | HTTP/1.1 服务器 |
| `http2_server.c` | HTTP/2 服务器 |
| `http3_server.c` | HTTP/3 服务器 |
| `tcp_server.c` | TCP 服务器 |
| `example_all_features.c` | 所有功能演示 |
| `example_basic_web.c` | Web UI 演示 |
| `example_config.c` | 配置文件使用 |

---

## 下一步 | Next Steps

- 阅读 [API Reference](./API-Reference) 了解完整 API
- 查看 [Features](./Features) 了解所有功能
- 浏览 [Examples](./Examples) 获取更多代码示例

---

[返回主页](./Home)

