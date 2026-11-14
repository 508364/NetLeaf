# NetLeaf Plugin Development Guide

本指南介绍如何使用 NL 扩展系统开发第三方插件。

## 快速开始

### 1. 复制模板

将 `examples/plugin_template/` 目录复制到你的项目：

```bash
cp -r examples/plugin_template my_plugin
cd my_plugin
```

### 2. 修改配置

编辑 `netleaf_plugin_template.h`，修改以下常量：

```c
#define MY_PLUGIN_ID          "your_plugin_id"        // 唯一标识符
#define MY_PLUGIN_NAME        "Your Plugin Name"      // 显示名称
#define MY_PLUGIN_VERSION     "1.0.0"                 // 版本号
#define MY_PLUGIN_AUTHOR      "Your Name"             // 作者名
#define MY_PLUGIN_DESCRIPTION "Plugin description"    // 描述（最多100字符）
#define MY_PLUGIN_URL         "https://..."           // 可选，GitHub 仓库地址
#define MY_PLUGIN_LICENSE     "MIT"                   // 许可证类型
```

### 3. 实现功能

在 `plugin.c` 中实现你的插件逻辑。必须实现的函数：

- `plugin_get_info()` - 返回插件信息结构体
- `plugin_init()` - 初始化插件
- `plugin_shutdown()` - 关闭插件
- `plugin_is_available()` - 检查可用性
- `plugin_register_module()` - 注册为模块（可选）

### 4. 构建插件

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

构建输出：
- Windows: `my_plugin.dll`
- Linux: `libmy_plugin.so`
- macOS: `libmy_plugin.so`

### 5. 部署插件

将构建输出放到 NetLeaf 的 `extensions/` 目录：

```bash
# Windows
copy my_plugin.dll ..\..\extensions\

# Linux
cp libmy_plugin.so ../../extensions/

# macOS
cp libmy_plugin.so ../../extensions/
```

## v2.4.0 新增 API

v2.4.0 引入了完整的扩展生命周期管理、状态查询、搜索过滤和热重载 API。

### 生命周期管理

```c
#include "netleaf_module.h"

// 初始化单个扩展
int ret = nl_extension_init("my_plugin");
if (ret != NL_OK) {
    printf("Failed to init extension\n");
}

// 关闭单个扩展
nl_extension_shutdown("my_plugin");

// 强制关闭（忽略依赖）
nl_extension_force_shutdown("my_plugin");

// 批量初始化所有扩展
nl_extension_init_all();

// 批量关闭所有扩展
nl_extension_shutdown_all();
```

### 状态查询

```c
// 检查是否已初始化
int initialized = nl_extension_is_initialized("my_plugin");

// 检查是否正在运行
int running = nl_extension_is_running("my_plugin");

// 获取详细状态
nl_module_status_t state = nl_extension_get_state("my_plugin");
switch (state) {
    case NL_STATE_UNINITIALIZED: printf("Not initialized\n"); break;
    case NL_STATE_INITIALIZED:   printf("Initialized\n"); break;
    case NL_STATE_RUNNING:       printf("Running\n"); break;
    case NL_STATE_ERROR:         printf("Error\n"); break;
}
```

### 信息查询

```c
const char* name = nl_extension_get_name("my_plugin");
const char* author = nl_extension_get_author("my_plugin");
const char* desc = nl_extension_get_description("my_plugin");
uint32_t caps = nl_extension_get_caps("my_plugin");

// 检查平台支持
int supports_win = nl_extension_supports_platform("my_plugin", "windows");
int supports_linux = nl_extension_supports_platform("my_plugin", "linux");
```

### 搜索过滤

```c
// 按能力搜索
nl_extension_info_t* results[32];
int count = nl_extension_find_by_capability(NL_CAP_SERVER, results, 32);
for (int i = 0; i < count; i++) {
    printf("Found: %s\n", results[i]->name);
}

// 按平台搜索
count = nl_extension_find_by_platform("linux", results, 32);

// 按名称模式搜索
count = nl_extension_find_by_name_pattern("chat*", results, 32);
```

### 元数据管理

```c
char value[256] = {0};

// 获取元数据
nl_extension_get_metadata("my_plugin", "version", value, sizeof(value));
printf("Version: %s\n", value);

// 设置元数据
nl_extension_set_metadata("my_plugin", "build_date", "2026-01-01");
```

### 热重载

```c
// 重载单个扩展
nl_extension_reload("my_plugin");

// 重载所有扩展
nl_extension_reload_all();
```

## 插件开发最佳实践

### 插件信息结构

```c
typedef struct {
    const char* name;              // 插件显示名
    const char* id;                // 唯一标识符
    const char* version;           // 语义化版本
    const char* description;       // 短描述
    const char* author;            // 作者
    const char* url;               // 插件仓库
    const char* license;           // 许可证
    
    uint32_t capabilities;         // 插件能力标志
    uint32_t required_capabilities; // 需要的宿主能力
    
    int platforms;                 // 平台支持位掩码
                                   // NL_PLATFORM_WINDOWS = 1
                                   // NL_PLATFORM_LINUX = 2
                                   // NL_PLATFORM_MACOS = 4
    
    const char* min_nl_version;    // 最低 NL 版本要求
    
    int (*init)(void);
    void (*shutdown)(void);
    int (*register_module)(nl_module_info_t**);
    int (*is_available)(void);
    void (*on_event)(const char*, void*, void*); // 事件处理
    void* userdata;
} nl_extension_info_t;
```

### 能力标志

| 标志 | 说明 |
|------|------|
| `NL_CAP_SERVER` | 可创建服务器 |
| `NL_CAP_CLIENT` | 可创建客户端 |
| `NL_CAP_ASYNC` | 支持异步操作 |
| `NL_CAP_THREAD_SAFE` | 线程安全 |
| `NL_CAP_PLATFORM_WIN` | Windows 支持 |
| `NL_CAP_PLATFORM_LINUX` | Linux 支持 |
| `NL_CAP_PLATFORM_MACOS` | macOS 支持 |
| `NL_CAP_LAZY_LOAD` | 支持懒加载 |
| `NL_CAP_DYNAMIC` | 可动态加载 |
| `NL_CAP_PLUGIN` | 是插件模块 |
| `NL_CAP_EXT_SYSTEM` | NL 扩展系统组件 |

### 平台支持位掩码

| 标志 | 值 | 说明 |
|------|-----|------|
| `NL_PLATFORM_WINDOWS` | 1 | Windows |
| `NL_PLATFORM_LINUX` | 2 | Linux |
| `NL_PLATFORM_MACOS` | 4 | macOS |
| `NL_PLATFORM_ALL` | 7 | 所有平台 |

### 常用宏

```c
// 组合能力标志
#define MY_CAPABILITIES \
    (NL_CAP_THREAD_SAFE | NL_CAP_DYNAMIC | NL_CAP_PLUGIN | NL_CAP_EXT_SYSTEM)

// 组合平台标志
#define MY_PLATFORMS \
    (NL_PLATFORM_WINDOWS | NL_PLATFORM_LINUX | NL_PLATFORM_MACOS)
```

## 使用 NL_EXTENSION_DEFINE 宏

v2.4.0 推荐使用 `NL_EXTENSION_DEFINE` 宏来定义扩展，它会自动生成符号解析和加载逻辑。

```c
// 在你的插件代码中
NL_EXTENSION_DEFINE(
    "my_plugin",          // library_id
    MY_PLUGIN_NAME,       // name
    MY_PLUGIN_VERSION,    // version
    MY_PLUGIN_AUTHOR,     // author
    MY_PLUGIN_DESCRIPTION,// description
    MY_CAPABILITIES,      // capabilities
    0,                    // required_capabilities
    MY_PLATFORMS,         // platforms
    MY_PLUGIN_MIN_NL_VERSION, // min_nl_version
    plugin_init,          // init
    plugin_shutdown,      // shutdown
    NULL,                 // register_module (可选)
    plugin_is_available,  // is_available
    NULL,                 // on_event (可选)
    NULL                  // userdata
);
```

## 插件示例：聊天插件

以下是一个简单的聊天插件示例：

```c
// chat_plugin.h
#ifndef CHAT_PLUGIN_H
#define CHAT_PLUGIN_H

#include "netleaf_module.h"

#define CHAT_PLUGIN_ID    "chat_plugin"
#define CHAT_PLUGIN_NAME  "Chat Plugin"
#define CHAT_PLUGIN_VER   "1.0.0"
#define CHAT_PLUGIN_AUTH  "Developer"

#ifdef _WIN32
    #ifdef NL_PLUGIN_EXPORTS
        #define CHAT_API __declspec(dllexport)
    #else
        #define CHAT_API __declspec(dllimport)
    #endif
#else
    #define CHAT_API
#endif

CHAT_API nl_extension_info_t* plugin_get_info(void);
CHAT_API int plugin_init(void);
CHAT_API void plugin_shutdown(void);
CHAT_API int plugin_is_available(void);
CHAT_API int send_chat_message(const char* user, const char* msg);
CHAT_API const char* get_last_message(void);

// 使用宏定义扩展
NL_EXTENSION_DEFINE(
    CHAT_PLUGIN_ID,
    CHAT_PLUGIN_NAME,
    CHAT_PLUGIN_VER,
    CHAT_PLUGIN_AUTH,
    "Simple chat plugin for NetLeaf",
    NL_CAP_THREAD_SAFE | NL_CAP_DYNAMIC | NL_CAP_PLUGIN,
    0,
    NL_PLATFORM_WINDOWS | NL_PLATFORM_LINUX | NL_PLATFORM_MACOS,
    "2.4.0",
    plugin_init,
    plugin_shutdown,
    NULL,
    plugin_is_available,
    NULL,
    NULL
);

#endif

// chat_plugin.c
#include "chat_plugin.h"
#include <string.h>

static char g_last_msg[1024] = {0};

CHAT_API int plugin_init(void) {
    printf("[Chat] Initialized\n");
    return 0;
}

CHAT_API void plugin_shutdown(void) {
    printf("[Chat] Shutdown\n");
}

CHAT_API int plugin_is_available(void) {
    return 1;
}

CHAT_API int send_chat_message(const char* user, const char* msg) {
    snprintf(g_last_msg, sizeof(g_last_msg), "[%s]: %s", user, msg);
    printf("[Chat] %s\n", g_last_msg);
    return 0;
}

CHAT_API const char* get_last_message(void) {
    return g_last_msg;
}
```

## 使用扩展库 API

```c
#include "netleaf_module.h"
#include <stdio.h>

int main() {
    // 初始化扩展系统
    nl_module_system_init();
    
    // 查找并初始化聊天插件
    nl_extension_info_t* chat = nl_extension_find_by_name_pattern("chat*", NULL, 1);
    if (chat) {
        nl_extension_init(chat->library_id);
        
        // 查询信息
        printf("Name: %s\n", nl_extension_get_name(chat->library_id));
        printf("Author: %s\n", nl_extension_get_author(chat->library_id));
        printf("State: %d\n", nl_extension_get_state(chat->library_id));
        
        // 设置元数据
        nl_extension_set_metadata(chat->library_id, "custom_key", "custom_value");
        
        // 热重载
        nl_extension_reload(chat->library_id);
        
        // 关闭
        nl_extension_shutdown(chat->library_id);
    }
    
    // 关闭扩展系统
    nl_module_system_shutdown();
    return 0;
}
```

## 插件发布

### 1. 创建发行版本

```bash
# Windows
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Linux/macOS
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 2. 打包插件

```bash
# 创建发行目录
mkdir -p release/extensions
cp *.dll release/extensions/      # Windows
# 或
cp *.so release/extensions/       # Linux/macOS

# 创建 README
cat > release/README.md << 'EOF'
# My Plugin

Description here...

## Installation
Place the .dll/.so file in the extensions/ directory next to netleaf.dll/.so

## Usage
See documentation...
EOF

# 打包
zip -r my-plugin-v1.0.0.zip release/
```

### 3. 上传到 GitHub

```bash
git init
git add .
git commit -m "Initial release v1.0.0"
git remote add origin https://github.com/youruser/my-plugin.git
git push -u origin main
```

## 常见问题

**Q: 插件无法加载**
A: 检查以下几点：
1. 插件文件是否在 `extensions/` 目录
2. 插件名称是否与文件名匹配
3. 是否导出了 `NL_EXTENSION_DEFINE` 宏定义的符号
4. 依赖的库是否可用

**Q: 版本不兼容**
A: 检查 `min_nl_version` 设置是否正确，确保与当前 NetLeaf 版本兼容。

**Q: 线程安全问题**
A: 如果你的插件支持多线程，确保使用互斥锁保护共享数据。

**Q: 如何检查平台支持？**
A: 使用 `nl_extension_supports_platform()` 函数检查插件是否支持当前平台。

## 进一步阅读

- [NL 扩展系统文档](../docs/netleaf_module.md)
- [插件开发示例](./plugin_example/)
- [API 参考](../include/netleaf_module.h)
