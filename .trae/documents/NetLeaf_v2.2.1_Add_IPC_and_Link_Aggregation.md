# NetLeaf v2.2.1 版本计划：添加 IPC 通讯服务和同端口链路聚合

## 版本信息

- 版本号：`2.2.1` (CMakeLists.txt 中更新)
- 新增两个独立库：`IPC` (跨平台进程间通信) 和 `Link Aggregation` (同端口链路聚合)
- 平台支持：IPC 和 Link Aggregation 仅支持 Windows 和 Linux，macOS 不支持
- 和主库共用版本号

## 新增模块

### 1. IPC 通讯服务 (netleaf_ipc)

**功能：** 跨进程通信，支持双向消息传递。
- Windows：命名管道 (Named Pipes)
- Linux：Unix Domain Socket

**文件结构：**
```
src/ipc/
  CMakeLists.txt            # 构建配置
  netleaf_ipc.c             # 公共接口 (API 宏、版本、enable API)
  netleaf_ipc_internal.h    # 内部结构体定义
src/linux/netleaf_ipc_linux.c    # Linux 实现 (Unix Domain Socket)
src/windows/netleaf_ipc_windows.c # Windows 实现 (Named Pipe)
include/netleaf_ipc.h         # 公共 API 头文件
```

**公共 API (`include/netleaf_ipc.h`)：**
```c
// 模块管理
NL_API void nl_ipc_enable(int enable);
NL_API int nl_ipc_is_enabled(void);

// 创建/销毁
NL_API nl_ipc_t* nl_ipc_create(const char* endpoint);  // endpoint: Windows pipe name or Linux socket path
NL_API void nl_ipc_destroy(nl_ipc_t* ipc);

// 服务端
NL_API int nl_ipc_listen(nl_ipc_t* ipc);
NL_API int nl_ipc_accept(nl_ipc_t* ipc, nl_ipc_conn_t** conn);

// 客户端
NL_API int nl_ipc_connect(nl_ipc_t* ipc);

// 通信
NL_API int nl_ipc_send(nl_ipc_conn_t* conn, const void* data, size_t len);
NL_API int nl_ipc_recv(nl_ipc_conn_t* conn, void* buf, size_t buf_len, size_t* out_len);
NL_API int nl_ipc_close(nl_ipc_conn_t* conn);

// 版本
NL_API const char* nl_ipc_version(void);
NL_API int nl_ipc_is_available(void);
```

**实现细节：**

**Linux (Unix Domain Socket)：**
- 使用 `socket(AF_UNIX, SOCK_STREAM, 0)` 创建 socket
- `bind("socket_path")` 绑定本地路径
- `listen()` + `accept()` 接受连接
- 使用 `read()`/`write()` 或 `send()`/`recv()` 传输数据
- `pthread_mutex_t` 保护并发访问

**Windows (Named Pipe)：**
- 使用 `CreateNamedPipe()` 创建命名管道
- `ConnectNamedPipe()` 等待连接
- 使用 `ReadFile()`/`WriteFile()` 传输数据
- `HANDLE` 和 `CRITICAL_SECTION` 管理线程和同步

### 2. 同端口链路聚合 (netleaf_linkagg)

**功能：** 多个后端服务器共享同一监听端口，自动负载均衡。
- 依赖 IPC 库进行后端服务器通信
- 复用主库的 socket/HTTP 基础设施

**文件结构：**
```
src/linkagg/
  CMakeLists.txt
  netleaf_linkagg.c
  netleaf_linkagg_internal.h
src/linux/netleaf_linkagg_linux.c
src/windows/netleaf_linkagg_windows.c
include/netleaf_linkagg.h
```

**公共 API (`include/netleaf_linkagg.h`)：**
```c
// 负载均衡策略
typedef enum {
    NL_LAGG_ROUND_ROBIN = 0,   // 轮询
    NL_LAGG_RANDOM,             // 随机
    NL_LAGG_LEAST_CONNECTIONS,  // 最少连接
    NL_LAGG_WEIGHTED_ROUND_ROBIN // 加权轮询
} nl_lagg_policy_t;

// 后端服务器
typedef struct nl_lagg_backend nl_lagg_backend_t;

// 创建/销毁
NL_API nl_lagg_server_t* nl_lagg_create(int port);
NL_API void nl_lagg_destroy(nl_lagg_server_t* server);

// 添加/移除后端
NL_API int nl_lagg_add_backend(nl_lagg_server_t* server, const char* endpoint, int weight);
NL_API int nl_lagg_remove_backend(nl_lagg_server_t* server, const char* endpoint);

// 配置
NL_API void nl_lagg_set_policy(nl_lagg_server_t* server, nl_lagg_policy_t policy);
NL_API nl_lagg_policy_t nl_lagg_get_policy(nl_lagg_server_t* server);

// 状态
NL_API int nl_lagg_start(nl_lagg_server_t* server);
NL_API void nl_lagg_stop(nl_lagg_server_t* server);
NL_API int nl_lagg_is_running(nl_lagg_server_t* server);
NL_API int nl_lagg_get_backend_count(nl_lagg_server_t* server);
```

**实现细节：**

**核心逻辑 (netleaf_linkagg.c)：**
- 监听端口，接收请求
- 根据策略选择后端服务器
- 通过 IPC 将请求转发给后端
- 后端将响应回传
- 将响应返回给客户端

**负载均衡策略：**
- Round Robin：简单轮询，维护索引计数器
- Random：随机选择
- Least Connections：选择当前连接数最少的后端
- Weighted Round Robin：按权重分配

**架构设计：**
- 监听线程 (复用主库 socket 代码)
- IPC 连接池 (维护与后端的长连接)
- 后端管理链表
- 策略调度器

## 具体修改步骤

### Step 1: 更新版本号

**文件：** `CMakeLists.txt`
- 第 2 行：`project(NetLeaf VERSION 2.2.0 ...)` → `project(NetLeaf VERSION 2.2.1 ...)`

### Step 2: 更新 CMakeLists.txt 添加新模块

**文件：** `CMakeLists.txt`
- 第 15 行后添加：
  ```cmake
  option(BUILD_IPC "Build IPC communication module (Windows/Linux only)" ON)
  option(BUILD_LINKAGG "Build link aggregation module (Windows/Linux only)" ON)
  ```
- 第 186-188 行 (构建摘要) 添加：
  ```cmake
  message(STATUS "Build ipc: ${BUILD_IPC}")
  message(STATUS "Build linkagg: ${BUILD_LINKAGG}")
  ```
- 第 173 行后添加 IPC 模块集成：
  ```cmake
  if(BUILD_IPC)
      message(STATUS "Building ipc module...")
      add_subdirectory(src/ipc)
  endif()
  ```
- 第 IPC 集成后添加 Link Aggregation 模块集成：
  ```cmake
  if(BUILD_LINKAGG)
      message(STATUS "Building linkagg module...")
      add_subdirectory(src/linkagg)
  endif()
  ```

### Step 3: 创建 IPC 模块

#### 3.1 公共头文件 `include/netleaf_ipc.h`

遵循现有独立库头文件模式：
- DLL 导出/导入宏 (仅 Windows)
- `extern "C"` 保护
- 版本宏 `NL_IPC_VERSION`
- 完整的 API 声明

#### 3.2 公共源文件 `src/ipc/netleaf_ipc.c`

- 模块版本返回 `nl_ipc_version()`
- Enable/Disable API: `nl_ipc_enable()`, `nl_ipc_is_enabled()`, `nl_ipc_enable_ex()`
- 可用性检查: `nl_ipc_is_available()`

#### 3.3 内部头文件 `src/ipc/netleaf_ipc_internal.h`

- `struct nl_ipc` - IPC 服务器/客户端统一结构
- `struct nl_ipc_conn` - 连接结构
- 平台无关的类型定义

#### 3.4 Linux 实现 `src/linux/netleaf_ipc_linux.c`

- `nl_ipc_create()` - 创建 Unix Domain Socket
- `nl_ipc_listen()` - bind + listen
- `nl_ipc_accept()` - accept 连接
- `nl_ipc_connect()` - connect 到服务端
- `nl_ipc_send()`/`nl_ipc_recv()` - 读写数据
- `nl_ipc_close()`/`nl_ipc_destroy()` - 清理资源

#### 3.5 Windows 实现 `src/windows/netleaf_ipc_windows.c`

- `nl_ipc_create()` - 创建命名管道
- `nl_ipc_listen()` - `CreateNamedPipe()` + `ConnectNamedPipe()`
- `nl_ipc_accept()` - `ConnectNamedPipe()`
- `nl_ipc_connect()` - `CreateFile()` 连接管道
- `nl_ipc_send()`/`nl_ipc_recv()` - `WriteFile()`/`ReadFile()`
- `nl_ipc_close()`/`nl_ipc_destroy()` - `CloseHandle()`

### Step 4: 创建 Link Aggregation 模块

#### 4.1 公共头文件 `include/netleaf_linkagg.h`

- DLL 导出/导入宏
- `struct nl_lagg_server` / `struct nl_lagg_backend` 前向声明
- 完整的 API 声明

#### 4.2 公共源文件 `src/linkagg/netleaf_linkagg.c`

- 后端服务器管理 (链表操作)
- 负载均衡策略调度逻辑
- 版本/enable API

#### 4.3 内部头文件 `src/linkagg/netleaf_linkagg_internal.h`

- `struct nl_lagg_server` - 包含监听 socket、IPC 连接池、后端链表
- `struct nl_lagg_backend` - 后端信息 (endpoint, weight, current_connections)
- 策略状态结构

#### 4.4 Linux 实现 `src/linux/netleaf_linkagg_linux.c`

- 监听 socket (复用主库 socket 代码模式)
- 使用 IPC 连接池转发请求
- 负载均衡调度器

#### 4.5 Windows 实现 `src/windows/netleaf_linkagg_windows.c`

- 监听 socket (Windows 平台模式)
- 使用 IPC 连接池转发请求
- 负载均衡调度器

### Step 5: 更新构建脚本

**文件：** `build_all.bat` (Windows)
- 打包时包含新头文件：`include/netleaf_ipc.h`、`include/netleaf_linkagg.h`

**文件：** `build_all.sh` (Linux)
- 打包时包含新头文件
- 不需要修改交叉编译器检测逻辑

### Step 6: 更新主库集成 (可选)

在 `include/netleaf.h` 中添加简短说明：
```c
// 新增模块头文件
// #include "netleaf_ipc.h"      // IPC communication (Windows/Linux)
// #include "netleaf_linkagg.h"  // Link aggregation (Windows/Linux)
```

## 平台宏使用约定

- IPC 和 Link Aggregation 仅在 Windows 和 Linux 上构建
- macOS 跳过构建 (CMakeLists.txt 中通过 `BUILD_IPC`/`BUILD_LINKAGG` 控制)
- 代码中使用 `#ifdef _WIN32` / `#ifdef __linux__` 区分平台

## 依赖关系

- Link Aggregation 依赖 IPC 库
- 两者都独立于主库 (可解耦使用)
- 使用相同的 `LINK_LIBS` (平台特定库)

## 验证步骤

1. Windows x64 编译 - 确保零警告零错误
2. Linux amd64 编译 - 确保零警告零错误
3. 运行示例程序验证 IPC 功能
4. 运行示例程序验证链路聚合功能
5. 打包验证 - 确认新头文件和新库包含在发布包中
