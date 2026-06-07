# NetLeaf v2.0.0 构建指南

## 快速开始

### Windows

在 Visual Studio Developer Command Prompt 中运行：

```cmd
# 同时构建所有架构 (x86, x64, ARM64)
build_all_windows.bat

# 或者只构建单个架构
cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### Linux/WSL

```bash
# 添加执行权限
chmod +x build_all_linux.sh build_wsl.sh

# 同时构建所有架构
./build_all_linux.sh

# 或者只构建当前架构
./build_wsl.sh
```

## 单架构构建

### Windows

```cmd
# x64
mkdir build_x64 && cd build_x64
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release

# x86
mkdir build_x86 && cd build_x86
cmake .. -G "Visual Studio 17 2022" -A Win32
cmake --build . --config Release

# ARM64
mkdir build_arm64 && cd build_arm64
cmake .. -G "Visual Studio 17 2022" -A ARM64
cmake --build . --config Release
```

### Linux

```bash
# x64 (默认)
mkdir build_x64 && cd build_x64
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# x86
mkdir build_x86 && cd build_x86
cmake .. -DCMAKE_C_FLAGS=-m32 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# ARM
mkdir build_arm && cd build_arm
cmake .. -DCMAKE_C_FLAGS=-march=armv7-a -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# ARM64
mkdir build_arm64 && cd build_arm64
cmake .. -DCMAKE_C_FLAGS=-march=armv8-a -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## 输出文件位置

### Windows

```
build/
├── bin/Windows/x64/Release/
│   ├── netleaf.dll
│   ├── http1_server.exe
│   ├── http2_server.exe
│   ├── http3_server.exe
│   ├── http_server.exe
│   └── simple_server.exe
└── lib/Windows/x64/Release/
    ├── netleaf.lib
    └── netleaf.exp
```

### Linux

```
build_wsl/
├── bin/Linux/x64/
│   ├── libnetleaf.so
│   ├── http1_server
│   ├── http2_server
│   ├── http3_server
│   ├── http_server
│   └── simple_server
└── lib/Linux/x64/
    └── libnetleaf.a
```

## 构建选项

### CMake 选项

```bash
# 禁用共享库构建
cmake .. -DBUILD_SHARED_LIBS=OFF

# 禁用静态库构建
cmake .. -DBUILD_STATIC_LIBS=OFF

# 禁用示例程序构建
cmake .. -DBUILD_EXAMPLES=OFF

# 组合选项
cmake .. -DBUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Release
```

## 交叉编译 (Linux)

### 安装交叉编译工具链

```bash
# Debian/Ubuntu
sudo apt install gcc-multilib
sudo apt install gcc-arm-linux-gnueabihf
sudo apt install gcc-aarch64-linux-gnu
```

### 交叉编译

```bash
# x86
cmake .. -DCMAKE_C_COMPILER=gcc -DCMAKE_C_FLAGS=-m32

# ARM
cmake .. -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc

# ARM64
cmake .. -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc
```

## 高级服务器API

v2.0.0 新增的高级服务器API支持：

### 1. 静态文件服务

```c
#include "netleaf.h"

int main() {
    // 一行代码启动文件服务器
    nl_serve_files("./public", 8080);
    
    while (1) {
        // 保持运行
        Sleep(1000);  // Windows
        // sleep(1);  // Linux
    }
    return 0;
}
```

### 2. API路由

```c
#include "netleaf.h"

void hello_handler(const char* path, nl_http_method_t method,
                   const char* body, size_t body_size,
                   char** response, size_t* response_size,
                   void* user_data) {
    const char* resp = "{\"message\":\"Hello NetLeaf!\"}";
    *response_size = strlen(resp);
    *response = (char*)malloc(*response_size + 1);
    strcpy(*response, resp);
}

int main() {
    nl_router_t* router = nl_router_create();
    
    // 添加路由
    nl_router_add_route(router, "/api/hello", NL_METHOD_GET, hello_handler, NULL);
    
    // 设置静态文件目录 (可选)
    nl_router_set_static_dir(router, "./public");
    
    // 启动服务器
    nl_router_serve(router, 8080);
    
    while (1) {
        Sleep(1000);
    }
    nl_router_destroy(router);
    return 0;
}
```

### 3. 简单服务器

```c
#include "netleaf.h"

void my_handler(const char* path, nl_http_method_t method,
                const char* body, size_t body_size,
                char** response, size_t* response_size,
                void* user_data) {
    // 处理请求...
}

int main() {
    nl_serve(8080, my_handler, NULL);
    
    while (1) {
        Sleep(1000);
    }
    return 0;
}
```

## 完整特性列表

### TCP/UDP支持
- TCP 服务器/客户端
- UDP 服务器/客户端
- Socket选项配置
- 多线程异步处理

### HTTP支持
- HTTP/1.1
- HTTP/2 (HPACK压缩)
- HTTP/3 (QUIC基础)

### 高级服务器API
- 静态文件服务
- API路由
- 支持HTML/Vue/React等前端
- MIME类型自动识别
- 相对路径/绝对路径支持

### 跨平台
- Windows (IOCP)
- Linux (epoll)
- x86/x64/ARM/ARM64

## 故障排除

### Windows: 找不到 cl.exe

请确保在 **Visual Studio Developer Command Prompt** 中运行，而不是普通的命令提示符。

### Linux: 缺少 pthread

确保链接时包含 pthread：
```bash
gcc -o myserver myserver.c -lnetleaf -lpthread
```

### 交叉编译失败

安装相应的交叉编译工具链：
```bash
sudo apt install gcc-arm-linux-gnueabihf
```

## 下一步

查看 [ADVANCED_API.md](ADVANCED_API.md) 了解详细的API使用说明，或查看 examples/ 目录下的示例程序。
