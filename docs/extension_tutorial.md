# NetLeaf NL 扩展系统开发教程

> 版本：对应 NetLeaf `2.4.1` / NL 扩展系统 `NL_MODULE_VERSION = 2.4.1`
> 头文件：[include/netleaf_module.h](../include/netleaf_module.h)、[include/netleaf_lang.h](../include/netleaf_lang.h)
> 配套示例：[examples/extension_showcase/](../examples/extension_showcase/)
> 示例 API 使用指南：[docs/extension_showcase_api_guide.md](extension_showcase_api_guide.md)

本教程系统讲解 **NL 扩展系统（NL Extension System）** 的全部 API，并配合一个可编译、可运行的完整示例
[examples/extension_showcase/](../examples/extension_showcase/) 逐项演示。示例只演示扩展系统 API 本身
（扩展注册、生命周期、元数据、懒加载、依赖与函数解析，以及 lang 错误码注册与变量系统等），
不承载任何业务/负载逻辑。示例中的演示程序会对每一项 API 做 `PASS/FAIL` 断言，
运行结束打印 `PASS=192 FAIL=0`，可作为回归验证。

> 说明：该示例已从 NetLeaf 主构建中解耦，不再作为根工程的目标随 `-DBUILD_EXAMPLES=ON` 一起构建，
> 需按 [3.1 构建示例](#31-构建示例) 的方式独立构建，便于后续迁入独立仓库。

---

## 1. 概述

NL 扩展系统是 NetLeaf 的运行时扩展框架，核心理念是：

- **核心库精简**：主库（`netleaf.dll` / `libnetleaf.so`）只包含网络核心与扩展管理框架。
- **扩展独立编译**：每个扩展（`lang`、`ipc`、`vue`、以及第三方扩展）都是独立的动态库，
  运行时加载。
- **统一注册表**：所有扩展在运行期注册到主库的**扩展注册表**中，由主库统一管理生命周期、
  依赖、查询与调用。

> **关键约束**：扩展的全局状态由主库的注册表持有，因此扩展必须**动态加载**（或与主库同时链接到
> 同一个 `netleaf` 动态库实例），才能与宿主共享同一份注册表。

---

## 2. 架构与关键概念

| 概念 | 说明 |
| --- | --- |
| `nl_extension_info_t` | 扩展的信息结构体，描述元数据、能力、生命周期回调、依赖、函数解析器等 |
| `library_id` | 扩展的唯一标识字符串，例如 `"showcase_engine"` |
| `library_name` | 扩展显示名，例如 `"Showcase Engine Extension"` |
| `library_value` | 主库注册时**自动分配**的整型编号，从 `1` 开始（`0` 保留），只读 |
| 能力标志 `NL_CAP_*` | 位标志，描述扩展能力/平台约束 |
| 生命周期状态 `nl_module_status_t` | `UNINITIALIZED / INITIALIZED / ERROR / DISABLED / LOADING / STOPPED` |
| 懒加载状态 `nl_module_lazy_status_t` | `UNLOADED / LOADING / LOADED / STOPPING / STOPPED` |

数据流概览：

```
扩展库(.dll/.so)  --导出 get_extension_info-->  nl_extension_info_t*
        |                                              |
        | nl_extension_register(info)                  |
        v                                              v
  主库扩展注册表  <---- 生命周期/依赖/查询/调用 API ---- 宿主程序
```

---

## 3. 快速开始

### 3.1 构建示例

该示例已从主构建解耦，**不再**随根工程的 `-DBUILD_EXAMPLES=ON` 一起构建。先构建主库与
lang 扩展库（得到 `libnetleaf` 与 `libnetleaf_lang`）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**方式一（推荐）：把示例作为独立 CMake 工程构建**

```bash
cmake -S examples/extension_showcase -B build_showcase -DNETLEAF_ROOT=<netleaf 源码根目录>
cmake --build build_showcase -j
```

若示例就位于 NetLeaf 源码树内，可省略 `NETLEAF_ROOT`（默认取本示例的 `../..`）：

```bash
cmake -S examples/extension_showcase -B build_showcase
cmake --build build_showcase -j
```

**方式二：直接用编译器编译**（示例依赖 `include/` 头文件、`libnetleaf` 与 `libnetleaf_lang`）：

```bash
# Linux / macOS
gcc -shared -fPIC -std=c99 -Iinclude -Iexamples/extension_showcase -DNL_EXTENSION_EXPORTS \
    examples/extension_showcase/showcase_extension.c \
    -o libnetleaf_extension_showcase.so -Lbuild/lib -lnetleaf

gcc -std=c99 -Iinclude -Iexamples/extension_showcase \
    examples/extension_showcase/extension_showcase_demo.c \
    -o extension_showcase -Lbuild/lib -lnetleaf -lnetleaf_lang \
    -L. -lnetleaf_extension_showcase
```

```bat
:: Windows (MinGW)
gcc -shared -std=c99 -Iinclude -Iexamples\extension_showcase -DNL_EXTENSION_EXPORTS ^
    examples\extension_showcase\showcase_extension.c ^
    -o netleaf_extension_showcase.dll -Lbuild\lib -lnetleaf

gcc -std=c99 -Iinclude -Iexamples\extension_showcase ^
    examples\extension_showcase\extension_showcase_demo.c ^
    -o extension_showcase.exe -Lbuild\lib -lnetleaf -lnetleaf_lang ^
    -L. -lnetleaf_extension_showcase
```

产物：

- `libnetleaf.dll`（主库，Windows）/ `libnetleaf.so`（Linux/macOS）
- `libnetleaf_lang.dll` / `libnetleaf_lang.so`（lang 扩展库）
- `libnetleaf_extension_showcase.dll`（示例扩展库，定义 3 个扩展）
- `extension_showcase.exe`（宿主演示程序）

> 使用 MinGW / MSVC / GCC / Clang 均可。示例源码使用 C99 标准，仅依赖主库公开头文件。
> 仅做语法检查（无需先构建主库）：
> `gcc -fsyntax-only -std=c99 -Wall -Wextra -Wpedantic -Iinclude -Iexamples/extension_showcase examples/extension_showcase/extension_showcase_demo.c`

### 3.2 运行示例

```bash
./extension_showcase        # Windows: extension_showcase.exe
```

> Windows 下需保证 `netleaf.dll`、`netleaf_lang.dll`、`netleaf_extension_showcase.dll`
> 位于可执行文件同目录或 `PATH` 中。

预期结尾：

```
  示例结束: PASS=192 FAIL=0
```

### 3.3 目录结构

| 文件 | 作用 |
| --- | --- |
| [showcase_extension.h](../examples/extension_showcase/showcase_extension.h) | 扩展库公共接口（导出函数声明） |
| [showcase_extension.c](../examples/extension_showcase/showcase_extension.c) | 使用 `NL_EXTENSION_DEFINE` / `_LAZY` 定义并实现 3 个扩展 |
| [extension_showcase_demo.c](../examples/extension_showcase/extension_showcase_demo.c) | 宿主程序，分 18 节演示全部扩展与 lang API |
| [CMakeLists.txt](../examples/extension_showcase/CMakeLists.txt) | 独立构建脚本（扩展库 + 宿主程序） |
| [README.md](../examples/extension_showcase/README.md) | 示例简介、构建/运行说明与 API 覆盖清单 |
| [LICENSE](../examples/extension_showcase/LICENSE) | 示例自有许可证（MIT） |

示例定义了三个扩展：

| library_id | 版本 | 特点 | 依赖 |
| --- | --- | --- | --- |
| `showcase_codec` | 1.2.0 | 使用 `NL_EXTENSION_DEFINE_LAZY`，支持懒加载 | 无 |
| `showcase_engine` | 2.0.0 | 依赖 `showcase_codec`，提供函数解析器 | 依赖 codec |
| `showcase_metrics` | 1.0.0 | 平台串为 `"all"`，可独立卸载 | 无 |

---

## 4. 定义一个扩展

### 4.1 `NL_EXTENSION_DEFINE`（普通扩展）

```c
NL_EXTENSION_DEFINE(ext_id, ext_name, ext_version, ext_author, ext_desc,
                    platforms_str, caps,
                    init_fn, shutdown_fn, available_fn, version_fn)
```

它会生成一个 `static nl_extension_info_t nl_extension_info_<ext_id>` 结构体。示例：

```c
NL_EXTENSION_DEFINE(
    showcase_engine,                 // ext_id（同时作为 library_id 字符串）
    "Showcase Engine Extension",     // 显示名
    SHOWCASE_ENGINE_VERSION,         // 版本
    "NetLeaf Team",                  // 作者
    "引擎示例扩展：演示依赖声明、生命周期与扩展间调用",  // 描述（≤150 字节）
    "Windows,Linux,MacOS",           // 平台串，或 "all"
    NL_CAP_SERVER | NL_CAP_THREAD_SAFE,
    nl_showcase_engine_init,
    nl_showcase_engine_shutdown,
    nl_showcase_engine_is_available,
    nl_showcase_engine_version
);
```

> ⚠️ **注意**：宏本身**不含结尾分号**，调用后必须自己加 `;`。

要点：

- `ext_id` 会被 `#` 字符串化，成为 `library_id`；同时用于拼接变量名。
- `caps` 会自动附加 `NL_CAP_DYNAMIC | NL_CAP_LAZY_LOAD | NL_CAP_EXT_SYSTEM`。
- `platforms_str` 支持大小写不敏感、任意顺序，如 `"Windows,Linux,MacOS"`，或 `"all"`。
  注册时由主库解析为 `platform_windows/linux/macos` 三个字段。
- 描述建议不超过 `NL_EXTENSION_DESC_MAX_LEN`（150 字节，约 50 个汉字），
  可用 `nl_extension_validate_description()` 校验。

### 4.2 `NL_EXTENSION_DEFINE_LAZY`（支持懒加载）

```c
NL_EXTENSION_DEFINE_LAZY(ext_id, ext_name, ext_version, ext_author, ext_desc,
                         platforms_str, caps,
                         init_fn, shutdown_fn, available_fn, version_fn,
                         lazy_load_fn, lazy_unload_fn)
```

与 `NL_EXTENSION_DEFINE` 相同，额外填充 `lazy_load` / `lazy_unload` 回调，
并自动加上 `NL_CAP_LAZY_LOAD`。回调签名：

```c
void* (*lazy_load)(void);   // 成功返回非 NULL 上下文指针
void  (*lazy_unload)(void); // 释放资源
```

### 4.3 导出与获取

```c
#define NL_EXTENSION_GET_INFO(ext_id) (&nl_extension_info_<ext_id>)

// 导出给宿主（NL_EXT_API 在扩展库编译时展开为 dllexport）
NL_EXT_API nl_extension_info_t* nl_showcase_engine_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(showcase_engine);
}
```

`NL_EXT_API` 是扩展库导出宏：编译扩展库时定义 `NL_EXTENSION_EXPORTS` 即展开为 `__declspec(dllexport)`，
宿主侧则为 `__declspec(dllimport)`；非 Windows 平台为空。

### 4.4 平台串解析宏

```c
NL_PARSE_PLATFORMS_WIN(str)    // 识别 "windows" / "win"
NL_PARSE_PLATFORMS_LINUX(str)  // 识别 "linux" / "lin"
NL_PARSE_PLATFORMS_MACOS(str)  // 识别 "macos" / "mac" / "darwin"
```

### 4.5 能力标志

| 标志 | 含义 |
| --- | --- |
| `NL_CAP_NONE` | 无 |
| `NL_CAP_SERVER` / `NL_CAP_CLIENT` | 可创建服务器 / 客户端 |
| `NL_CAP_ASYNC` | 支持异步 |
| `NL_CAP_THREAD_SAFE` | 线程安全 |
| `NL_CAP_PLATFORM_WIN/LINUX/MACOS/ALL` | 平台约束 |
| `NL_CAP_LAZY_LOAD` | 支持懒加载 |
| `NL_CAP_DYNAMIC` | 可动态加载 |
| `NL_CAP_PLUGIN` | 是插件模块 |
| `NL_CAP_EXT_SYSTEM` | 属于 NL 扩展系统 |
| `NL_CAP_TLS` | 提供 TLS 能力 |

辅助宏：

```c
NL_CAP_HAS(caps, cap)     // 检测
NL_CAP_ADD(caps, cap)     // 置位
NL_CAP_REMOVE(caps, cap)  // 清位
```

---

## 5. 注册与注销

```c
int nl_extension_register(nl_extension_info_t* info);   // 成功返回 0；重复注册返回 0（幂等）
int nl_extension_unregister(const char* library_id);    // 成功 0；不存在 -1
int nl_extension_get_count(void);                       // 已注册扩展数量
nl_extension_info_t** nl_extension_get_all(int* count); // 返回 malloc 的数组，用完需 free()
int nl_extension_validate_description(const char* desc); // 合法 1，超长 0
```

注册时主库会：

1. 依据 `library_id` 去重；
2. **自动分配** `library_value`（从 `1` 开始，`0` 保留）；
3. 解析 `platforms` 字符串，回填三个平台字段。

按编号反查：

```c
int32_t     nl_extension_get_value_by_id(const char* library_id);
const char* nl_extension_get_id_by_value(int32_t library_value);
```

示例输出：`codec library_value = 1`，反查得到 `"showcase_codec"`。

---

## 6. 生命周期管理

```c
int  nl_extension_init(const char* library_id);            // 调用 ext->init()
int  nl_extension_shutdown(const char* library_id);        // 调用 ext->shutdown()
int  nl_extension_force_shutdown(const char* library_id);  // 关闭并注销（等价 unregister）
int  nl_extension_reload(const char* library_id);          // shutdown + init
int  nl_extension_reload_all(void);                        // 批量重载，返回成功数
int  nl_extension_init_all(void);                          // 批量 init，返回成功数
int  nl_extension_shutdown_all(void);                      // 批量 shutdown，返回执行数
int  nl_extension_force_shutdown_all(void);                // 清空注册表，返回清理数
```

典型生命周期：

```
register -> init -> 使用 -> shutdown -> unregister
```

热重载用于扩展升级或状态重置：`nl_extension_reload()` 内部执行 `shutdown` 后 `init`。

---

## 7. 状态与信息查询

```c
int                nl_extension_is_initialized(const char* library_id);
int                nl_extension_is_running(const char* library_id);
nl_module_status_t nl_extension_get_state(const char* library_id);
```

信息查询（便利函数）：

```c
const char* nl_extension_get_name(const char* library_id);
const char* nl_extension_get_author(const char* library_id);
const char* nl_extension_get_description(const char* library_id);
uint32_t    nl_extension_get_caps(const char* library_id);        // == nl_extension_get_capabilities
const char* nl_extension_get_platforms(const char* library_id);
uint32_t    nl_extension_get_capabilities(const char* library_id);
int         nl_extension_supports_platform(const char* library_id, const char* platform);
int         nl_extension_get_version_by_id(const char* id, char* buf, size_t buf_size);
nl_extension_info_t* nl_extension_get_info(const char* library_id);  // == nl_extension_access
```

`platform` 取值：`"windows"`/`"win"`、`"linux"`、`"macos"`/`"darwin"`（大小写不敏感）。

---

## 8. 搜索与过滤

```c
int nl_extension_find_by_capability(uint32_t cap, nl_extension_info_t** results, int max_count);
int nl_extension_find_by_platform(const char* platform, nl_extension_info_t** results, int max_count);
int nl_extension_find_by_name_pattern(const char* pattern, nl_extension_info_t** results, int max_count);
```

返回命中数量，结果写入调用者提供的数组（不超过 `max_count`）。
`find_by_name_pattern` 会对 `library_id` 与 `library_name` 做子串匹配 `strstr`。

```c
nl_extension_info_t* found[16];
int n = nl_extension_find_by_capability(NL_CAP_THREAD_SAFE, found, 16);
```

---

## 9. 依赖管理

每个扩展结构体内含依赖链表与计数：

```c
nl_extension_dependency_t* dependencies;   // 链表头
int dependency_count;                      // 总数
int required_dep_count;                    // 必需数量
int optional_dep_count;                    // 可选数量
```

依赖类型：

```c
NL_EXT_DEP_REQUIRED  // 必需依赖，缺失则视为加载失败
NL_EXT_DEP_OPTIONAL  // 可选依赖，缺失仅表示功能不可用
```

增删与查询：

```c
int nl_extension_add_dependency(nl_extension_info_t* ext, const char* dep_id,
                                nl_extension_dep_type_t type, const char* min_version);
int nl_extension_remove_dependency(nl_extension_info_t* ext, const char* dep_id);
int nl_extension_clear_dependencies(nl_extension_info_t* ext);

int nl_extension_get_dependency_count(nl_extension_info_t* ext);
nl_extension_dependency_t* nl_extension_get_dependencies(nl_extension_info_t* ext);
nl_extension_dependency_t* nl_extension_get_dependency_by_id(nl_extension_info_t* ext, const char* dep_id);
int nl_extension_has_dependency(nl_extension_info_t* ext, const char* dep_id);
int nl_extension_get_required_deps(nl_extension_info_t* ext, nl_extension_dependency_t** deps, int max_count);
int nl_extension_get_optional_deps(nl_extension_info_t* ext, nl_extension_dependency_t** deps, int max_count);
```

校验与解析：

```c
int nl_extension_check_dependencies(nl_extension_info_t* ext, int require_all); // 0 通过，-1 失败
int nl_extension_resolve_dependencies(nl_extension_info_t* ext);                // 优先调用 ext->resolve_dependencies
int nl_extension_are_dependencies_met(nl_extension_info_t* ext);                // 满足返回 1
```

示例：

```c
nl_extension_add_dependency(engine_info, "showcase_codec",  NL_EXT_DEP_REQUIRED, "1.0.0");
nl_extension_add_dependency(engine_info, "showcase_legacy", NL_EXT_DEP_OPTIONAL, NULL);
// 依赖总数 2，必需 1，可选 1；codec 已注册 => check/are_dependencies_met 通过
```

---

## 10. 懒加载

### 10.1 扩展级

使用 `NL_EXTENSION_DEFINE_LAZY` 定义后，`nl_extension_info_t` 会带有：

- `void* (*lazy_load)(void)`：按需加载，返回上下文指针
- `void  (*lazy_unload)(void)`：卸载
- `nl_module_lazy_status_t lazy_status`：当前懒加载状态

主库提供了 4 个扩展级懒加载函数来驱动上述回调并维护状态：

```c
int  nl_extension_lazy_load(const char* library_id);        // 触发 lazy_load，成功 0
int  nl_extension_lazy_unload(const char* library_id);      // 触发 lazy_unload，成功 0
nl_module_lazy_status_t nl_extension_lazy_get_status(const char* library_id);
int  nl_extension_lazy_is_loaded(const char* library_id);   // 1/0
```

行为约定（与实现一致）：

- `lazy_load`：无 `lazy_load` 回调返回 `-1`；已处于 `LOADED` 时幂等返回 `0`；
  否则置 `LOADING`，回调返回非 NULL 则置 `LOADED`（成功），返回 NULL 则回到 `UNLOADED`（失败）。
- `lazy_unload`：无回调或当前不在 `LOADED` 状态返回 `-1`；成功后状态置 `STOPPED`。
- 未注册的 `library_id` 查询状态统一返回 `NL_MODULE_LAZY_UNLOADED`。

状态机：

```
UNLOADED --lazy_load--> LOADED --lazy_unload--> STOPPED --lazy_load--> LOADED ...
```

示例写法（`showcase_codec` 使用 `NL_EXTENSION_DEFINE_LAZY` 定义）：

```c
nl_extension_lazy_load(SHOWCASE_CODEC_ID);                    // 0
nl_extension_lazy_get_status(SHOWCASE_CODEC_ID) == NL_MODULE_LAZY_LOADED;
nl_extension_lazy_is_loaded(SHOWCASE_CODEC_ID);               // 1
nl_extension_lazy_unload(SHOWCASE_CODEC_ID);                  // 0，状态变 STOPPED
```

### 10.2 模块级（批量懒加载）

模块级懒加载由主库统一管理，作用于注册表中的模块（含用 `NL_MODULE_DEFINE_LAZY` 定义的模块）：

```c
void nl_module_lazy_enable(int enable);                     // 全局开关
void nl_module_lazy_enable_module(nl_module_type_t type);
void nl_module_lazy_disable_module(nl_module_type_t type);
int  nl_module_lazy_is_enabled(nl_module_type_t type);
int  nl_module_lazy_load(nl_module_type_t type);            // 调用 mod->lazy_load
int  nl_module_lazy_unload(nl_module_type_t type);          // 调用 mod->lazy_unload
nl_module_lazy_status_t nl_module_lazy_get_status(nl_module_type_t type);
int  nl_module_lazy_is_loaded(nl_module_type_t type);
void nl_module_lazy_preload_all(void);
void nl_module_lazy_unload_all(void);
void nl_module_lazy_clear_cache(void);                      // 卸载所有处于 LOADED 的模块
```

示例（演示程序内部定义了一个 `NL_MODULE_DEFINE_LAZY` 模块）：

```c
nl_module_lazy_enable(1);
nl_module_lazy_enable_module(NL_MODULE_CUSTOM);
nl_module_lazy_load(NL_MODULE_CUSTOM);       // 触发 lazy_load
nl_module_lazy_is_loaded(NL_MODULE_CUSTOM);  // == 1
nl_module_lazy_unload(NL_MODULE_CUSTOM);     // 触发 lazy_unload
```

---

## 11. 元数据

元数据是一组“扩展 id + key → value”的键值对，用于挂载扩展的自定义信息。

```c
int nl_extension_set_metadata(const char* library_id, const char* key, const char* value);
int nl_extension_get_metadata(const char* library_id, const char* key, char* value, size_t val_size);
```

行为约定：

- `set`：`library_id` 或 `key` 为 NULL 返回 `-1`；否则写入/覆盖并返回 `0`。
- `get`：找到返回 `0` 并把 value 写为 NUL 结尾字符串；未找到返回 `-1`。
- **清空语义**：把 `value` 传 `NULL` 即可清空该 key 的值（key 仍保留，之后读取得到空串）。

```c
char buf[128];

nl_extension_set_metadata("showcase_engine", "homepage", "https://netleaf.local"); // 0
nl_extension_get_metadata("showcase_engine", "homepage", buf, sizeof(buf));        // 0，buf="https://netleaf.local"

nl_extension_get_metadata("showcase_engine", "no_such_key", buf, sizeof(buf));     // -1

nl_extension_set_metadata("showcase_engine", "homepage", NULL);                    // 清空该值
nl_extension_get_metadata("showcase_engine", "homepage", buf, sizeof(buf));        // 0，buf=""
```

---

## 12. 扩展间调用

扩展可暴露一个“函数解析器”，让宿主或其他扩展按名字取到函数指针：

```c
// 扩展信息结构体字段（由扩展填充）
void* (*get_extension_function)(const char* library_id, const char* func_name);
```

宿主侧取函数：

```c
void* nl_extension_get_func(nl_extension_info_t* ext, const char* func_name);
void* nl_extension_get_func_by_id(const char* ext_id, const char* func_name);
```

示例中 `showcase_engine` 在 `init()` 阶段自绑定解析器：

```c
int nl_showcase_engine_init(void) {
    nl_extension_info_t* self = NL_EXTENSION_GET_INFO(showcase_engine);
    self->get_extension_function = nl_showcase_engine_resolve; // 解析器
    return 0;
}
```

宿主调用（示例解析的是无业务语义的元数据查询函数）：

```c
typedef const char* (*version_fn)(void);
version_fn vfn = (version_fn)nl_extension_get_func(engine_info, "version");
const char* ver = vfn();   // 返回该扩展的版本字符串
```

访问与遍历：

```c
nl_extension_info_t* nl_extension_access(const char* library_id); // 只读访问
int nl_extension_is_loaded(const char* library_id);
int nl_extension_is_available(const char* library_id);
int nl_extension_iterate(int (*cb)(nl_extension_info_t*, void*), void* userdata);
```

> 说明：函数指针 <-> `void*` 的转换依赖平台 ABI，`-Wpedantic` 下可能产生告警，属预期行为。

---

## 13. 自动加载目录

主库在 `nl_modules_init()` 时会自动扫描扩展目录（默认 `extensions/`，Windows 下为相对
`netleaf.dll` 的 `extensions`）：

```c
int         nl_extension_auto_load(void);
int         nl_extension_auto_load_from_dir(const char* directory);
void        nl_extension_set_auto_load_dir(const char* directory);
const char* nl_extension_get_auto_load_dir(void);
```

自动加载会尝试若干标准导出符号名（如 `nl_lang_get_extension_info`、`nl_ipc_get_extension_info`
等）。目录不存在时返回 `0`（正常情况，不报错）。

---

## 14. 模块查询 API（同头文件）

除扩展 API 外，头文件还提供模块级查询，本示例一并演示：

```c
nl_module_info_t* nl_module_get_info(nl_module_type_t type);
nl_module_info_t* nl_module_get_info_by_name(const char* name);
nl_module_info_t* nl_get_module(nl_module_type_t type);
int nl_module_get_all(nl_module_info_t** modules, int max_count);
int nl_module_get_count(void);
int nl_get_module_count(void);
int nl_get_modules(const char** modules, int max_count);
int nl_module_is_platform_supported(nl_module_type_t type);
nl_module_status_t nl_module_get_status(nl_module_type_t type);
int nl_module_set_enabled(nl_module_type_t type, int enabled);
const char* nl_module_get_name(nl_module_type_t type);
const char* nl_module_get_version(nl_module_type_t type);
const char* nl_module_get_description(nl_module_type_t type);
int nl_module_has_capability(nl_module_type_t type, int cap);
int nl_module_get_capabilities(nl_module_type_t type);
int nl_module_register(nl_module_info_t* info);
int nl_module_unregister(nl_module_type_t type);
int nl_module_available(const char* module_name);
void nl_print_modules(void);
// 模块依赖
int nl_module_add_dependency(nl_module_type_t module, nl_module_type_t dependency);
int nl_module_remove_dependency(nl_module_type_t module, nl_module_type_t dependency);
int nl_module_check_dependencies(nl_module_type_t module);
nl_module_info_t* nl_module_get_dependencies(nl_module_type_t module);
```

`nl_modules_init()` 会在注册核心模块后执行一次扩展自动加载；`nl_modules_shutdown()` 会关闭并清空注册表。

---

## 15. Lang 错误码与变量系统（netleaf_lang）

`netleaf_lang` 扩展除多语言错误消息外，还提供**变量替换**能力。示例的 16、17 节完整演示了本节内容。

### 15.1 表驱动错误码注册

```c
#define SHOWCASE_LIB_ID 0x1000   // 自定义库编号，避开内置 NL_LIB_*

NL_ERROR_BEGIN(showcase_error_table)
    NL_ERROR(0,  "Success", "成功")
    NL_ERROR(-1, "Showcase operation failed", "示例操作失败")
    NL_ERROR(-3, "Connection limit {{<var>max_conn</var>}} exceeded",
                "连接数已超过上限 {{<var>max_conn</var>}}")
NL_ERROR_END
```

- `NL_ERROR_BEGIN(name)` → `static const nl_lang_error_def_t name[] = {`
- `NL_ERROR(code, en, zh)` → 追加 `{code, en_us, zh_cn}`
- `NL_ERROR_END` → `};`（**不要再补分号**）

```c
nl_lang_register_errors(SHOWCASE_LIB_ID, showcase_error_table); // 一次注册 en_us + zh_cn

nl_lang_is_registered(SHOWCASE_LIB_ID);          // 1
nl_lang_has_language(SHOWCASE_LIB_ID, "zh_cn");  // 1
nl_lang_has_error(SHOWCASE_LIB_ID, -1);          // 1

int code_n = 0;
int* codes = nl_lang_get_error_codes(SHOWCASE_LIB_ID, &code_n);  // 需 free(codes)

nl_lang_register_lib_name(SHOWCASE_LIB_ID, "showcase");
nl_lang_get_lib_name(SHOWCASE_LIB_ID);           // "showcase"
```

### 15.2 多语言取值

```c
nl_lang_set("en_us");
nl_lang_get_error(SHOWCASE_LIB_ID, -1);                    // "Showcase operation failed"

nl_lang_set("zh_cn");
nl_lang_get_error(SHOWCASE_LIB_ID, -1);                    // "示例操作失败"

nl_lang_get_error_for(SHOWCASE_LIB_ID, -1, "en_us");       // 不受当前语言影响

nl_error_is_success(0);                                    // 1
nl_lang_get_error_category(-1);                            // "Parameter"
nl_lang_set("en-US");                                      // 非法格式（缺下划线）→ -1
```

> 语言码必须是 `xx_xx` 形式（如 `en_us`、`zh_cn`），大小写不敏感、内部规范化为小写。

### 15.3 变量系统

静态与类型化变量：

```c
nl_lang_var_set("showcase_project", "NetLeaf");
nl_lang_var_set_int("max_conn", 1024);
nl_lang_var_set_float("ratio", 0.75);
nl_lang_var_set_bool("verbose", 1);

nl_lang_var_get("showcase_project");       // "NetLeaf"
nl_lang_var_get_int("max_conn", 0);        // 1024（缺失时返回默认值）
nl_lang_var_exists("showcase_project");    // 1
nl_lang_var_remove("showcase_project");
nl_lang_var_clear_all();
```

> `nl_lang_var_get` 返回指向内部静态缓冲区的指针，会被下一次调用覆盖，需要时请立即拷贝。

替换与条件求值：

```c
char buf[128];
nl_lang_var_replace("Hello {{showcase_project}}!", buf, sizeof(buf));         // "Hello NetLeaf!"
nl_lang_var_replace_html("port={{<var>max_conn</var>}}", buf, sizeof(buf));   // "port=1024"
nl_lang_var_condition_eval("max_conn > 100");                                // 1
```

**动态变量（provider）**——每次访问重新求值：

```c
static int counter = 0;
static int my_provider(const char* name, char* out, size_t out_size, void* userdata) {
    (void)name; (void)userdata;
    snprintf(out, out_size, "dynamic-%d", ++counter);
    return 1;   // 返回非 0 表示已写入值
}

nl_lang_var_set_provider("showcase_now", my_provider, NULL);
nl_lang_var_is_dynamic("showcase_now");     // 1
nl_lang_var_get("showcase_now");            // "dynamic-1" → 再取一次 "dynamic-2"
```

**外部变量（环境变量）**——每次访问读取进程环境：

```c
nl_lang_var_bind_env("showcase_token", "NL_SHOWCASE_TOKEN");  // 绑定到环境变量
nl_lang_var_is_dynamic("showcase_token");                     // 1
nl_lang_var_get("showcase_token");                            // 读取 NL_SHOWCASE_TOKEN 当前值

nl_lang_var_load_env("NL_SHOWCASE_DEMO_");   // 按前缀批量导入环境变量为普通变量
```

**带变量的错误消息**——消息模板中的 `{{<var>NAME</var>}}` 在输出时替换：

```c
char buf[256];
nl_lang_get_error_with_vars(SHOWCASE_LIB_ID, -3, buf, sizeof(buf)); // 例："Connection limit 1024 exceeded"
```

> 更完整的用法与示例节号对照见 [示例 API 使用指南](extension_showcase_api_guide.md)。

---

## 16. 完整 API 速查表（Extension）

| 分类 | 函数 |
| --- | --- |
| 定义 | `NL_EXTENSION_DEFINE`、`NL_EXTENSION_DEFINE_LAZY`、`NL_EXTENSION_GET_INFO`、`NL_EXT_API` |
| 注册 | `nl_extension_register`、`nl_extension_unregister`、`nl_extension_get_count`、`nl_extension_get_all`、`nl_extension_validate_description` |
| 编号 | `nl_extension_get_value_by_id`、`nl_extension_get_id_by_value` |
| 生命周期 | `nl_extension_init`、`nl_extension_shutdown`、`nl_extension_force_shutdown`、`nl_extension_init_all`、`nl_extension_shutdown_all`、`nl_extension_force_shutdown_all`、`nl_extension_reload`、`nl_extension_reload_all` |
| 状态 | `nl_extension_is_initialized`、`nl_extension_is_running`、`nl_extension_get_state` |
| 信息 | `nl_extension_get_info`、`nl_extension_get_name`、`nl_extension_get_author`、`nl_extension_get_description`、`nl_extension_get_caps`、`nl_extension_get_capabilities`、`nl_extension_get_platforms`、`nl_extension_supports_platform`、`nl_extension_get_version_by_id` |
| 搜索 | `nl_extension_find_by_capability`、`nl_extension_find_by_platform`、`nl_extension_find_by_name_pattern` |
| 元数据 | `nl_extension_set_metadata`、`nl_extension_get_metadata`（值传 `NULL` 即清空） |
| 访问/调用 | `nl_extension_access`、`nl_extension_is_loaded`、`nl_extension_is_available`、`nl_extension_get_func`、`nl_extension_get_func_by_id`、`nl_extension_iterate` |
| 依赖 | `nl_extension_add_dependency`、`nl_extension_remove_dependency`、`nl_extension_clear_dependencies`、`nl_extension_get_dependency_count`、`nl_extension_get_dependencies`、`nl_extension_get_dependency_by_id`、`nl_extension_has_dependency`、`nl_extension_get_required_deps`、`nl_extension_get_optional_deps`、`nl_extension_check_dependencies`、`nl_extension_resolve_dependencies`、`nl_extension_are_dependencies_met` |
| 自动加载 | `nl_extension_auto_load`、`nl_extension_auto_load_from_dir`、`nl_extension_set_auto_load_dir`、`nl_extension_get_auto_load_dir` |
| 扩展级懒加载 | `nl_extension_lazy_load`、`nl_extension_lazy_unload`、`nl_extension_lazy_get_status`、`nl_extension_lazy_is_loaded` |
| 模块查询/依赖 | `nl_module_register`、`nl_module_unregister`、`nl_module_get_info`、`nl_module_get_info_by_name`、`nl_module_get_all`、`nl_module_get_count`、`nl_module_get_name/version/description`、`nl_module_has_capability`、`nl_module_get_capabilities`、`nl_module_is_platform_supported`、`nl_module_get_status`、`nl_module_set_enabled`、`nl_module_available`、`nl_get_module`、`nl_get_module_count`、`nl_get_modules`、`nl_module_add_dependency`、`nl_module_remove_dependency`、`nl_module_check_dependencies`、`nl_module_get_dependencies`、`nl_print_modules` |
| 模块级懒加载 | `nl_module_lazy_enable`、`nl_module_lazy_enable_module`、`nl_module_lazy_disable_module`、`nl_module_lazy_is_enabled`、`nl_module_lazy_load`、`nl_module_lazy_unload`、`nl_module_lazy_get_status`、`nl_module_lazy_is_loaded`、`nl_module_lazy_preload_all`、`nl_module_lazy_unload_all`、`nl_module_lazy_clear_cache` |
| Lang 错误码 | `NL_ERROR_BEGIN`/`NL_ERROR`/`NL_ERROR_END`、`nl_lang_register_errors`、`nl_lang_get_error`、`nl_lang_get_error_for`、`nl_lang_set`、`nl_lang_is_registered`、`nl_lang_has_language`、`nl_lang_has_error`、`nl_lang_get_error_codes`、`nl_lang_register_lib_name`、`nl_lang_get_lib_name`、`nl_error_is_success`、`nl_lang_get_error_category` |
| Lang 变量 | `nl_lang_var_set/get/set_int/get_int/set_float/get_float/set_bool/get_bool`、`nl_lang_var_exists`、`nl_lang_var_remove`、`nl_lang_var_clear_all`、`nl_lang_var_set_provider`、`nl_lang_var_is_dynamic`、`nl_lang_var_bind_env`、`nl_lang_var_load_env`、`nl_lang_var_replace`、`nl_lang_var_replace_html`、`nl_lang_var_condition_eval`、`nl_lang_get_error_with_vars` |

---

## 17. 构建与验证

该示例已从主构建解耦，构建与运行方式见 [3.1 构建示例](#31-构建示例) / [3.2 运行示例](#32-运行示例)。
简要步骤：

```bash
# 1) 构建主库与 lang 扩展库
cmake -S . -B build
cmake --build build -j

# 2) 单独构建扩展示例（方式一：独立 CMake 工程；详见 3.1 节）
cmake -S examples/extension_showcase -B build_showcase
cmake --build build_showcase -j

# 3) 运行（结束应输出 PASS=192 FAIL=0）
./build_showcase/extension_showcase        # Windows: extension_showcase.exe
```

示例本身即是一个自检程序：所有 `[PASS]` 通过时进程返回 `0`，出现 `[FAIL]` 时返回 `1`，
可方便地接入 CI。若只想做语法检查（不需要先构建主库）：

```bash
gcc -fsyntax-only -std=c99 -Wall -Wextra -Wpedantic \
    -Iinclude -Iexamples/extension_showcase \
    examples/extension_showcase/extension_showcase_demo.c
```

---

## 18. 本期配套修复与注意事项

在编写本示例的过程中发现并修复了头文件/主库中的两处缺陷（均为最小改动、不改变既有正确用法）：

1. **`NL_MODULE_DEFINE` / `NL_MODULE_DEFINE_LAZY` 宏形参与字段同名**
   - 形参 `author` 与结构体字段 `.author` 同名，预处理会把字段名一并替换，
     生成 `."<值>" = "<值>"` 的非法初始化，导致所有使用该宏的代码（含 `NL_MODULE_*_INFO` 便捷宏）无法编译。
   - 修复：形参重命名为 `author_str`，宏体改为 `.author = author_str`。

2. **注册时 `library_value` 被错误分配为 `0`**（[netleaf_module.c](../src/netleaf_module.c)）
   - 原实现先把 `library_id` 写入注册表、再调用 `get_or_assign_library_value()`，
     该 helper 会命中“已插入但值仍为 0”的本槽位并返回 `0`，与 CHANGELOG
     中“从 1 开始，0 保留”的约定不符。
   - 修复：在把 id 写入注册表**之前**完成编号分配。

其他注意事项：

- 宏 `NL_EXTENSION_DEFINE` / `_LAZY` / `NL_MODULE_DEFINE` / `_LAZY`、
  以及 `NL_ERROR_BEGIN` / `NL_ERROR_END` 的写法需按文档补齐（扩展/模块宏结尾无分号，
  调用后补 `;`；`NL_ERROR_END` 自身已含 `;`，**不要**再补）。
- 扩展级懒加载请使用 `nl_extension_lazy_*`；模块级批量懒加载使用 `nl_module_lazy_*`，两者互不相同。
- 元数据已实现：`set` 写入/覆盖，`get` 命中返回 0、未命中返回 -1，值传 `NULL` 即清空。
- 函数指针与 `void*` 互转（`nl_extension_get_func*` 用法）在 `-Wpedantic` 下会告警，属预期。
- 示例中的环境变量演示使用运行期写入的**假数据**（如 `env-value-1`），不含任何真实密钥/令牌。

---

## 19. 相关文件

- 扩展头文件：[include/netleaf_module.h](../include/netleaf_module.h)
- 语言/变量头文件：[include/netleaf_lang.h](../include/netleaf_lang.h)
- 主库头文件：[include/netleaf.h](../include/netleaf.h)
- 综合示例：[examples/extension_showcase/](../examples/extension_showcase/)
- 示例 README：[examples/extension_showcase/README.md](../examples/extension_showcase/README.md)
- 示例 API 使用指南：[docs/extension_showcase_api_guide.md](extension_showcase_api_guide.md)
- 插件开发指南：[docs/plugin_development.md](plugin_development.md)
- 插件模板：[examples/plugin_template/](../examples/plugin_template/)
