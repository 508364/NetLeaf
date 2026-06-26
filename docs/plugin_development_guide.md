# NetLeaf 插件开发指南

## 概述

NetLeaf 提供了一个强大的插件系统，允许开发者创建自定义模块并动态加载到应用程序中。本指南详细介绍如何开发和集成自定义插件。

## 插件系统架构

### 核心组件

1. **模块注册表** - 管理所有已注册的模块
2. **懒加载系统** - 支持按需加载模块，减少启动时间
3. **插件加载器** - 动态加载外部插件库
4. **依赖管理器** - 处理模块间的依赖关系

### 模块类型

| 类型 | 说明 |
|------|------|
| `NL_MODULE_CORE` | 核心库模块 |
| `NL_MODULE_CUSTOM` | 自定义/第三方模块 |
| `NL_MODULE_IPC` | IPC通信模块 |
| `NL_MODULE_VUE` | Vue.js支持模块 |

## 开发步骤

### 第一步：创建插件源文件

```c
#define _GNU_SOURCE
#include "netleaf_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#define MY_PLUGIN_VERSION "1.0.0"

static int g_plugin_initialized = 0;

static int my_plugin_init(void) {
    if (g_plugin_initialized) return 0;
    g_plugin_initialized = 1;
    return 0;
}

static void my_plugin_shutdown(void) {
    if (!g_plugin_initialized) return;
    g_plugin_initialized = 0;
}

static int my_plugin_is_available(void) {
    return 1;
}

static const char* my_plugin_version(void) {
    return MY_PLUGIN_VERSION;
}

NL_MODULE_DEFINE_LAZY(
    NL_MODULE_CUSTOM,
    my_plugin,
    MY_PLUGIN_VERSION,
    NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    1, 1, 1,
    my_plugin_init,
    my_plugin_shutdown,
    my_plugin_is_available,
    my_plugin_version,
    "My custom plugin",
    "Author Name",
    NULL,
    NULL
);

static nl_plugin_descriptor_t g_plugin_descriptor = {
    .name = "my_plugin",
    .version = MY_PLUGIN_VERSION,
    .description = "My custom plugin description",
    .author = "Author Name",
    .init = my_plugin_init,
    .shutdown = my_plugin_shutdown,
    .register_module = NULL
};

EXPORT void nl_plugin_register(void) {
    nl_module_register(NL_MODULE_GET_INFO(my_plugin));
}

EXPORT nl_plugin_descriptor_t* nl_plugin_get_descriptor(void) {
    return &g_plugin_descriptor;
}
```

### 第二步：创建 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(my_plugin)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../include)

add_library(my_plugin SHARED my_plugin.c)

target_compile_definitions(my_plugin PRIVATE NL_EXPORTS)

if(WIN32)
    target_link_libraries(my_plugin netleaf)
else()
    target_link_libraries(my_plugin -L${CMAKE_CURRENT_SOURCE_DIR}/../../build -lnetleaf)
endif()

install(TARGETS my_plugin DESTINATION plugins)
```

### 第三步：编译插件

```bash
mkdir build && cd build
cmake ..
make
```

## API 参考

### 插件加载 API

| 函数 | 说明 |
|------|------|
| `nl_plugin_load()` | 加载插件库 |
| `nl_plugin_unload()` | 卸载插件库 |
| `nl_plugin_register()` | 注册插件模块 |
| `nl_plugin_get_descriptor()` | 获取插件描述信息 |

### 模块管理 API

| 函数 | 说明 |
|------|------|
| `nl_module_register()` | 注册模块 |
| `nl_module_unregister()` | 注销模块 |
| `nl_module_get_info()` | 获取模块信息 |
| `nl_module_get_status()` | 获取模块状态 |

### 懒加载 API

| 函数 | 说明 |
|------|------|
| `nl_module_lazy_enable()` | 启用/禁用懒加载 |
| `nl_module_lazy_load()` | 懒加载指定模块 |
| `nl_module_lazy_unload()` | 卸载懒加载模块 |
| `nl_module_lazy_preload_all()` | 预加载所有模块 |

## 使用示例

### 加载和使用插件

```c
#include "netleaf.h"
#include "netleaf_module.h"

int main() {
    nl_modules_init();
    
    nl_plugin_handle_t plugin = nl_plugin_load("./my_plugin.dll");
    if (!plugin) {
        printf("Failed to load plugin: %s\n", nl_plugin_get_error());
        return 1;
    }
    
    nl_plugin_descriptor_t* desc = nl_plugin_get_descriptor(plugin);
    printf("Loaded plugin: %s v%s\n", desc->name, desc->version);
    
    nl_plugin_register(plugin);
    
    nl_print_modules();
    
    nl_plugin_unload(plugin);
    nl_modules_shutdown();
    
    return 0;
}
```

## 模块能力标志

| 标志 | 说明 |
|------|------|
| `NL_CAP_SERVER` | 支持创建服务器 |
| `NL_CAP_CLIENT` | 支持创建客户端 |
| `NL_CAP_ASYNC` | 支持异步操作 |
| `NL_CAP_THREAD_SAFE` | 线程安全 |
| `NL_CAP_LAZY_LOAD` | 支持懒加载 |
| `NL_CAP_PLUGIN` | 是插件模块 |

## 平台支持

| 平台 | 标志 |
|------|------|
| Windows | `NL_CAP_PLATFORM_WIN` |
| Linux | `NL_CAP_PLATFORM_LINUX` |
| macOS | `NL_CAP_PLATFORM_MACOS` |

## 最佳实践

### 1. 使用懒加载

对于非关键路径的功能，使用懒加载可以显著减少启动时间：

```c
NL_MODULE_DEFINE_LAZY(
    NL_MODULE_CUSTOM,
    my_plugin,
    "1.0.0",
    NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    1, 1, 1,
    my_init, my_shutdown, my_available, my_version,
    "My plugin", "Author",
    my_lazy_load, my_lazy_unload
);
```

### 2. 处理依赖关系

如果你的插件依赖其他模块，使用依赖管理 API：

```c
nl_module_add_dependency(NL_MODULE_CUSTOM, NL_MODULE_VUE);
```

### 3. 错误处理

始终检查 API 返回值：

```c
nl_plugin_handle_t plugin = nl_plugin_load(path);
if (!plugin) {
    printf("Error: %s\n", nl_plugin_get_error());
    return NL_ERROR;
}
```

### 4. 线程安全

如果你的插件涉及多线程操作，确保线程安全：

```c
NL_MODULE_DEFINE_LAZY(
    NL_MODULE_CUSTOM,
    my_plugin,
    "1.0.0",
    NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    ...
);
```

## 示例插件

完整的示例插件位于 `examples/plugin_example/` 目录：

- `netleaf_example_plugin.c` - 插件实现
- `CMakeLists.txt` - 构建配置
- `test_plugin.c` - 使用示例

## 调试技巧

1. **启用调试模式**：调用 `nl_debug_enable(1)`
2. **打印模块状态**：调用 `nl_print_modules()`
3. **检查插件错误**：使用 `nl_plugin_get_error()`
4. **验证平台支持**：使用 `nl_module_is_platform_supported()`

## 发布指南

### 打包插件

1. 编译插件为共享库（.dll 或 .so）
2. 创建 README 文件说明插件功能
3. 提供示例代码
4. 添加版本号和变更日志

### 分发渠道

- GitHub Releases
- 官方插件仓库
- 包管理器

## 许可证

插件开发者可以选择自己的许可证，但建议与 NetLeaf 保持一致（MIT 许可证）。

---

**版本**: 2.2.2  
**最后更新**: 2024年  
**作者**: 508364
