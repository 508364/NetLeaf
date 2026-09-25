# NetLeaf 插件开发指南

NetLeaf v2.4.0 的 NL 扩展系统支持第三方开发者创建自定义插件，实现动态加载、热重载、事件系统等功能。

## 快速开始

### 1. 获取模板

```bash
cp -r examples/plugin_template my_plugin
cd my_plugin
```

### 2. 配置插件

编辑 `netleaf_plugin_template.h`：

```c
#define MY_PLUGIN_ID          "my_plugin_v1"        // 唯一标识符
#define MY_PLUGIN_NAME        "My Plugin"           // 显示名称
#define MY_PLUGIN_VERSION     "1.0.0"               // 版本号
#define MY_PLUGIN_AUTHOR      "Your Name"           // 作者名
#define MY_PLUGIN_DESCRIPTION "插件描述"             // 描述（最多100字符）
#define MY_PLUGIN_URL         "https://github.com/..." // 可选
#define MY_PLUGIN_LICENSE     "MIT"                 // 许可证类型
```

### 3. 实现插件逻辑

在 `plugin.c` 中实现所需功能。

### 4. 构建

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

### 5. 部署

将生成的 `.dll` (Windows) 或 `.so` (Linux/macOS) 文件放到 NetLeaf 的 `extensions/` 目录。

## v2.4.0 新增 API

### 扩展生命周期管理

```c
// 初始化扩展库
int ret = nl_extension_init("my_ext");

// 关闭扩展库
nl_extension_shutdown("my_ext");

// 强制关闭（清理资源）
nl_extension_force_shutdown("my_ext");

// 批量操作
nl_extension_init_all();           // 初始化所有扩展库
nl_extension_shutdown_all();       // 关闭所有扩展库
nl_extension_force_shutdown_all(); // 强制关闭所有扩展库
```

### 扩展状态查询

```c
// 检查是否已初始化
int initialized = nl_extension_is_initialized("my_ext");

// 检查是否正在运行
int running = nl_extension_is_running("my_ext");

// 获取状态
nl_module_status_t status = nl_extension_get_state("my_ext");
```

### 扩展信息查询

```c
// 获取基本信息
const char* name = nl_extension_get_name("my_ext");
const char* author = nl_extension_get_author("my_ext");
const char* desc = nl_extension_get_description("my_ext");
uint32_t caps = nl_extension_get_caps("my_ext");

// 检查平台支持
int supports_win = nl_extension_supports_platform("my_ext", "windows");
int supports_linux = nl_extension_supports_platform("my_ext", "linux");
```

### 扩展搜索过滤

```c
// 按能力搜索
nl_extension_info_t* results[32];
int count = nl_extension_find_by_capability(NL_CAP_THREAD_SAFE, results, 32);

// 按平台搜索
count = nl_extension_find_by_platform("windows", results, 32);

// 按名称模式搜索
count = nl_extension_find_by_name_pattern("*chat*", results, 32);
```

### 元数据管理

```c
// 设置元数据
nl_extension_set_metadata("my_ext", "github_url", "https://github.com/user/repo");

// 获取元数据
char value[256];
nl_extension_get_metadata("my_ext", "github_url", value, sizeof(value));
```

### 热重载

```c
// 重载单个扩展库
nl_extension_reload("my_ext");

// 重载所有扩展库
nl_extension_reload_all();
```

## 插件发现与搜索

```c
// 发现指定目录下的所有插件
nl_plugin_handle_t** plugins;
int count = nl_plugin_discover("extensions/", &plugins, 64);

// 搜索所有已安装插件
int all_count = nl_plugin_discover_all(&plugins, 64);

// 按关键字搜索插件
nl_plugin_handle_t** results;
int found = nl_plugin_search("chat", &results, 10);
```

## 插件信息查询

```c
const char* id = nl_plugin_get_id(handle);        // 插件ID
const char* name = nl_plugin_get_name(handle);     // 显示名称
const char* version = nl_plugin_get_version(handle); // 版本号
const char* author = nl_plugin_get_author(handle); // 作者
const char* description = nl_plugin_get_description(handle); // 描述
nl_plugin_state_t state = nl_plugin_get_state(handle); // 状态
```

## 依赖验证

```c
// 检查插件依赖是否满足
int missing_deps = 0;
int valid = nl_plugin_check_dependencies(handle, &missing_deps);

// 获取详细错误信息
char error[256];
nl_plugin_validate(handle, error, sizeof(error));
```

## 版本兼容性检查

```c
// 检查插件是否与当前 NL 版本兼容
int compatible = nl_plugin_check_version_compatibility(handle, NETLEAF_VERSION);

// 获取插件要求的最低 NL 版本
const char* min_version = nl_plugin_get_min_version(handle);
```

## 事件系统

```c
// 订阅事件
nl_plugin_subscribe(handle, "network.connect");

// 主机发送事件
nl_plugin_emit_event("network.connect", &data, sizeof(data));

// 取消订阅
nl_plugin_unsubscribe(handle, "network.connect");
```

## 沙盒模式（可选）

```c
// 启用/禁用沙盒
nl_plugin_enable_sandbox(handle, 1);

// 检查是否启用沙盒
int is_sandboxed = nl_plugin_is_sandboxed(handle);
```

## 插件能力标志

| 标志 | 说明 |
|------|------|
| `NL_CAP_NONE` | 无能力 |
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

## 插件状态

```c
typedef enum {
    NL_PLUGIN_STATE_UNLOADED = 0,   // 未加载
    NL_PLUGIN_STATE_LOADING,         // 加载中
    NL_PLUGIN_STATE_LOADED,          // 已加载
    NL_PLUGIN_STATE_ERROR,           // 错误
    NL_PLUGIN_STATE_UNLOADING        // 卸载中
} nl_plugin_state_t;
```

## 完整示例

参考 `examples/plugin_template/` 目录。

## 发布插件

### 1. 打包

```bash
mkdir release
cp *.dll release/           # Windows
# 或 cp *.so release/       # Linux/macOS
cp README.md release/
zip -r my-plugin-v1.0.0.zip release/
```

### 2. 上传

将插件上传到 GitHub 仓库，提供详细的安装和使用说明。

## 常见问题

**Q: 插件无法加载**
A: 检查：
1. 文件名与插件 ID 是否匹配
2. 是否导出了 `plugin_get_info` 函数
3. 是否在 `extensions/` 目录
4. 依赖库是否可用

**Q: 版本不兼容**
A: 设置正确的 `min_nl_version`，确保与当前 NetLeaf 版本兼容。

**Q: 线程安全问题**
A: 如果支持多线程，使用互斥锁保护共享数据。

## 相关文档

- [NL 扩展系统头文件](../include/netleaf_module.h)
- [主库头文件](../include/netleaf.h)
- [扩展库开发指南](Extension.md)
- [示例插件](../examples/plugin_template/)
