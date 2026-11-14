# NL 扩展系统综合示例（extension_showcase）

本目录是 **NetLeaf NL 扩展系统** 的官方综合示例，用于完整示范 `include/` 下公开头文件中
与“扩展”相关的全部 API：

- [include/netleaf_module.h](../../include/netleaf_module.h)：NL 扩展系统主体（注册/生命周期/查询/依赖/懒加载/元数据…）
- [include/netleaf_lang.h](../../include/netleaf_lang.h)：多语言错误码注册与变量系统（含动态变量与外部环境变量）

示例只演示扩展系统 API 本身，**不承载任何业务/负载逻辑**；所有演示函数仅维护最小状态，
便于作为“可编译、可运行、可断言自检”的学习与回归模板。

> 说明：本示例已从 NetLeaf 主构建中**解耦**，主工程不再通过 `add_subdirectory` 引入它。
> 它可以按下面的方式独立构建，便于后续迁入独立仓库或独立发布。

---

## 1. 目录结构与产物

| 文件 | 作用 |
| --- | --- |
| [`showcase_extension.h`](showcase_extension.h) | 扩展库公共接口（`NL_EXT_API` 导出函数声明） |
| [`showcase_extension.c`](showcase_extension.c) | 用 `NL_EXTENSION_DEFINE` / `_LAZY` 定义并实现 3 个示例扩展 |
| [`extension_showcase_demo.c`](extension_showcase_demo.c) | 宿主演示程序，分 18 节演示全部扩展 API，逐项 PASS/FAIL 自检 |
| [`CMakeLists.txt`](CMakeLists.txt) | 独立构建脚本（扩展库 + 宿主程序） |
| [`LICENSE`](LICENSE) | 本示例自有许可证（MIT） |
| [`README.md`](README.md) | 本文件 |

构建产物：

- `netleaf_extension_showcase.dll/.so`：示例扩展库，定义了 3 个扩展
- `extension_showcase(.exe)`：宿主演示程序（自检可执行文件）

示例定义的三个扩展：

| `library_id` | 版本 | 特点 | 依赖 |
| --- | --- | --- | --- |
| `showcase_codec` | 1.2.0 | 使用 `NL_EXTENSION_DEFINE_LAZY`，支持扩展级懒加载 | 无 |
| `showcase_engine` | 2.0.0 | 声明依赖、提供函数解析器（`get_extension_function`） | 依赖 `showcase_codec` |
| `showcase_metrics` | 1.0.0 | 平台串为 `"all"`，可独立卸载 | 无 |

---

## 2. 构建

### 2.1 前置条件

- C99 编译器：GCC / Clang / MSVC（MinGW 亦可）
- CMake ≥ 3.14
- 已构建好的 **NetLeaf 主库 `netleaf`** 与 **lang 扩展库 `netleaf_lang`**
  （本示例演示 lang 错误码与变量 API，必须链接 `netleaf_lang`）

先构建 NetLeaf（在 NetLeaf 源码根目录执行）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### 2.2 独立构建本示例

在 NetLeaf 源码根目录执行：

```bash
cmake -S examples/extension_showcase -B build_showcase -DNETLEAF_ROOT=<netleaf 源码根目录>
cmake --build build_showcase -j
```

若 `examples/extension_showcase` 就位于 NetLeaf 源码树内，可省略 `NETLEAF_ROOT`
（其默认值为本目录的 `../..`）：

```bash
cmake -S examples/extension_showcase -B build_showcase
cmake --build build_showcase -j
```

`CMakeLists.txt` 的查找逻辑：

1. 若已存在 `netleaf_core` 目标（被父工程 `add_subdirectory` 引入），直接复用；
2. 否则按 `NETLEAF_ROOT/include` 找头文件、按 `NETLEAF_ROOT/build/lib` 等目录找
   `netleaf` / `netleaf_lang` 库；找不到会给出明确错误提示。

也可用 `-DNETLEAF_INCLUDE_DIR=`、`-DNETLEAF_CORE_LIBRARY=`、`-DNETLEAF_LANG_LIBRARY=`
直接指定路径。

### 2.3 手动编译（不使用 CMake）

若只想快速验证，可直接用编译器编译（以 GCC 为例）：

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

> 仅做语法检查（无需已构建主库）：
> ```bash
> gcc -fsyntax-only -std=c99 -Wall -Wextra -Wpedantic -Iinclude -Iexamples/extension_showcase \
>     examples/extension_showcase/extension_showcase_demo.c
> ```

---

## 3. 运行

```bash
./extension_showcase        # Windows: extension_showcase.exe
```

> Windows 运行时需保证 `netleaf.dll`、`netleaf_lang.dll` 与
> `netleaf_extension_showcase.dll` 位于可执行文件同目录或 `PATH` 中。

程序按 18 个小节逐项打印 `[PASS]`/`[FAIL]`，结尾输出统计：

```
  示例结束: PASS=192 FAIL=0
```

全部 `[PASS]` 时进程返回 `0`，出现任一 `[FAIL]` 时返回 `1`，可直接接入 CI 作为回归自检。

---

## 4. API 覆盖清单

下表列出本示例覆盖的全部公开 API 及其演示位置（`extension_showcase_demo.c` 的小节号）。

### 4.1 宏定义（[netleaf_module.h](../../include/netleaf_module.h)）

| API / 宏 | 小节 | 说明 |
| --- | --- | --- |
| `NL_EXTENSION_DEFINE` | `showcase_extension.c` | 定义普通扩展 |
| `NL_EXTENSION_DEFINE_LAZY` | `showcase_extension.c` | 定义带懒加载回调的扩展 |
| `NL_EXTENSION_GET_INFO` | `showcase_extension.c` | 取扩展信息结构体指针 |
| `NL_EXT_API` | `showcase_extension.h` | 扩展库导出宏 |
| `NL_PARSE_PLATFORMS_WIN/LINUX/MACOS` | 2 | 平台串解析宏 |
| `NL_CAP_HAS / NL_CAP_ADD / NL_CAP_REMOVE` | 2 | 能力位辅助宏 |
| `NL_CAP_*` 常量族 | 2 | 能力/平台标志 |

### 4.2 注册、注销与编号

| API | 小节 |
| --- | --- |
| `nl_extension_register` | 2 |
| `nl_extension_unregister` | 15 |
| `nl_extension_get_count` | 2 / 15 |
| `nl_extension_get_all` | 8 |
| `nl_extension_get_info` | 4 |
| `nl_extension_validate_description` | 2 |
| `nl_extension_get_value_by_id` | 2 |
| `nl_extension_get_id_by_value` | 2 |

### 4.3 生命周期

| API | 小节 |
| --- | --- |
| `nl_extension_init` | 3 |
| `nl_extension_shutdown` | 14 |
| `nl_extension_force_shutdown` | 15 |
| `nl_extension_reload` | 13 |
| `nl_extension_reload_all` | 13 |
| `nl_extension_init_all` | 14 |
| `nl_extension_shutdown_all` | 14 |
| `nl_extension_force_shutdown_all` | 15 / 18 |

### 4.4 状态与信息查询

| API | 小节 |
| --- | --- |
| `nl_extension_is_initialized` | 3 |
| `nl_extension_is_running` | 3 |
| `nl_extension_get_state` | 3 |
| `nl_extension_is_loaded` | 8 / 14 |
| `nl_extension_is_available` | 8 / 16 |
| `nl_extension_get_name` | 4 |
| `nl_extension_get_author` | 4 |
| `nl_extension_get_description` | 4 |
| `nl_extension_get_caps` | 4 |
| `nl_extension_get_capabilities` | 4 |
| `nl_extension_get_platforms` | 2 |
| `nl_extension_supports_platform` | 4 |
| `nl_extension_get_version_by_id` | 4 / 16 |

### 4.5 搜索过滤

| API | 小节 |
| --- | --- |
| `nl_extension_find_by_capability` | 5 |
| `nl_extension_find_by_platform` | 5 |
| `nl_extension_find_by_name_pattern` | 5 |

### 4.6 元数据

| API | 小节 |
| --- | --- |
| `nl_extension_set_metadata` | 6 |
| `nl_extension_get_metadata` | 6 |
| 清空语义（`value = NULL`） | 6 |

### 4.7 自动加载

| API | 小节 |
| --- | --- |
| `nl_extension_set_auto_load_dir` | 7 |
| `nl_extension_get_auto_load_dir` | 7 |
| `nl_extension_auto_load` | 7 |
| `nl_extension_auto_load_from_dir` | 7 |

### 4.8 访问与调用

| API | 小节 |
| --- | --- |
| `nl_extension_access` | 8 |
| `nl_extension_get_func` | 8 |
| `nl_extension_get_func_by_id` | 8 / 13 |
| `nl_extension_iterate` | 8 |

### 4.9 依赖管理

| API | 小节 |
| --- | --- |
| `nl_extension_add_dependency` | 9 |
| `nl_extension_remove_dependency` | 9 |
| `nl_extension_clear_dependencies` | 9 |
| `nl_extension_get_dependency_count` | 9 |
| `nl_extension_get_dependencies` | 9 |
| `nl_extension_get_dependency_by_id` | 9 |
| `nl_extension_has_dependency` | 9 |
| `nl_extension_get_required_deps` | 9 |
| `nl_extension_get_optional_deps` | 9 |
| `nl_extension_check_dependencies` | 9 |
| `nl_extension_resolve_dependencies` | 9 |
| `nl_extension_are_dependencies_met` | 9 |

### 4.10 懒加载

| API | 小节 |
| --- | --- |
| `nl_extension_lazy_load` | 10 |
| `nl_extension_lazy_unload` | 10 |
| `nl_extension_lazy_get_status` | 10 |
| `nl_extension_lazy_is_loaded` | 10 |
| `nl_module_lazy_enable` | 11 |
| `nl_module_lazy_enable_module` | 11 |
| `nl_module_lazy_disable_module` | 11 |
| `nl_module_lazy_is_enabled` | 11 |
| `nl_module_lazy_load` | 11 |
| `nl_module_lazy_unload` | 11 |
| `nl_module_lazy_get_status` | 11 |
| `nl_module_lazy_is_loaded` | 11 |
| `nl_module_lazy_preload_all` | 11 |
| `nl_module_lazy_unload_all` | 11 |
| `nl_module_lazy_clear_cache` | 11 |

### 4.11 模块查询与模块依赖

| API | 小节 |
| --- | --- |
| `NL_MODULE_DEFINE_LAZY` / `NL_MODULE_GET_INFO` | 11 |
| `nl_module_register` | 11 |
| `nl_module_unregister` | 12 |
| `nl_module_get_info` | 12 |
| `nl_module_get_info_by_name` | 12 |
| `nl_module_get_all` | 12 |
| `nl_module_get_count` / `nl_get_module_count` | 11 / 12 |
| `nl_module_is_platform_supported` | 12 |
| `nl_module_get_status` | 12 |
| `nl_module_set_enabled` | 12 |
| `nl_module_get_name` / `_version` / `_description` | 12 |
| `nl_module_has_capability` / `_get_capabilities` | 12 |
| `nl_get_module` | 12 |
| `nl_module_available` | 12 |
| `nl_get_modules` | 12 |
| `nl_module_add_dependency` / `remove_dependency` / `check_dependencies` / `get_dependencies` | 12 |
| `nl_print_modules` | 18 |

### 4.12 Lang 错误码注册与多语言（[netleaf_lang.h](../../include/netleaf_lang.h)）

| API / 宏 | 小节 |
| --- | --- |
| `NL_ERROR_BEGIN` / `NL_ERROR` / `NL_ERROR_END` | 16（表定义在文件顶部） |
| `nl_lang_register_errors` | 16 |
| `nl_lang_get_error` | 16 |
| `nl_lang_get_error_for` | 16 |
| `nl_lang_set` / 语言切换 | 16 |
| `nl_lang_is_registered` | 16 |
| `nl_lang_has_language` | 16 |
| `nl_lang_has_error` | 16 |
| `nl_lang_get_error_codes` | 16 |
| `nl_lang_register_lib_name` / `nl_lang_get_lib_name` | 16 |
| `nl_error_is_success` | 16 |
| `nl_lang_get_error_category` | 16 |
| `nl_lang_version` / `nl_lang_get_extension_info` | 16 |

### 4.13 Lang 变量系统

| API | 小节 |
| --- | --- |
| `nl_lang_var_set` / `nl_lang_var_get` | 17 |
| `nl_lang_var_set_int` / `nl_lang_var_get_int` | 17 |
| `nl_lang_var_set_float` / `nl_lang_var_get_float` | 17 |
| `nl_lang_var_set_bool` / `nl_lang_var_get_bool` | 17 |
| `nl_lang_var_exists` | 17 |
| `nl_lang_var_remove` / `nl_lang_var_clear_all` | 17 |
| `nl_lang_var_set_provider` / `nl_lang_var_is_dynamic` | 17（动态变量） |
| `nl_lang_var_bind_env` | 17（外部变量 env 绑定） |
| `nl_lang_var_load_env` | 17（按前缀批量导入 env） |
| `nl_lang_var_replace` | 17 |
| `nl_lang_var_replace_html` | 17 |
| `nl_lang_var_condition_eval` | 17 |
| `nl_lang_get_error_with_vars` | 17 |

---

## 5. API 接口与使用方法（NL 扩展系统）

> 本目录是“扩展示例”所在位置，下面给出 **NL 扩展系统** 三大 API 族的最小可用示例与说明，
> 与 `docs/` 中的指南保持同步（详见 [API 使用指南](../../docs/extension_showcase_api_guide.md)、
> [插件开发指南](../../docs/plugin_development.md)）。三大族：
>
> - `nl_extension_*` —— 扩展（extension）：动态扩展库的注册 / 生命周期 / 依赖 / 懒加载
> - `nl_module_*` —— 模块（module）：主库注册表里的更基础单元与模块级懒加载
> - `nl_plugin_*` —— 插件（plugin）：第三方 `.dll/.so` 动态加载（`dlopen/LoadLibrary`）

### 5.1 扩展（`nl_extension_*`）

**① 在扩展库里定义一个扩展**（本目录 [`showcase_extension.c`](showcase_extension.c) 的同款写法）：

```c
#include "netleaf_module.h"

int nl_my_ext_init(void)       { return 0; }
int nl_my_ext_shutdown(void)   { return 0; }
int nl_my_ext_is_available(void){ return 1; }
const char* nl_my_ext_version(void){ return "1.0.0"; }

/* 普通扩展：不带分号，调用后补 ';'。caps 自动补 NL_CAP_DYNAMIC|NL_CAP_LAZY_LOAD|NL_CAP_EXT_SYSTEM */
NL_EXTENSION_DEFINE(
    my_ext,                          /* ext_id → library_id */
    "My Extension",                  /* 显示名 */
    "1.0.0",                         /* 版本 */
    "NetLeaf Team",                  /* 作者 */
    "演示扩展",                       /* 描述，≤150 字节 */
    "all",                           /* 平台串，或 "Windows,Linux,MacOS" */
    NL_CAP_THREAD_SAFE,
    nl_my_ext_init,
    nl_my_ext_shutdown,
    nl_my_ext_is_available,
    nl_my_ext_version
);

/* 导出给宿主：宿主/自动加载靠此符号发现扩展 */
NL_EXT_API nl_extension_info_t* nl_my_ext_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(my_ext);
}
```

**② 在宿主程序里注册 → 使用 → 注销**（对应 [`extension_showcase_demo.c`](extension_showcase_demo.c) 第 2/3/15 节）：

```c
#include "netleaf_module.h"

/* 注册：成功 0；同一 id 重复注册幂等 0；缺必需依赖时返回 -2（拒绝加载） */
nl_extension_register(NL_EXTENSION_GET_INFO(my_ext));

/* 生命周期 */
nl_extension_init("my_ext");         /* 调 ext->init()，成功 0 */
nl_extension_shutdown("my_ext");     /* 调 ext->shutdown() */
nl_extension_reload("my_ext");       /* shutdown + init */

/* 状态查询 */
if (nl_extension_is_loaded("my_ext")) {
    char buf[64];
    nl_extension_get_version_by_id("my_ext", buf, sizeof(buf));
    printf("my_ext v%s\n", buf);
}

/* 注销：成功 0，不存在 -1 */
nl_extension_unregister("my_ext");
```

**③ 懒加载扩展**（用 `NL_EXTENSION_DEFINE_LAZY` 多接两个回调）：

```c
void* nl_my_ext_lazy_load(void)  { /* 分配资源 */ return NULL; }
void  nl_my_ext_lazy_unload(void){ /* 释放资源 */ }

NL_EXTENSION_DEFINE_LAZY(
    my_ext, "My Extension", "1.0.0", "NetLeaf Team",
    "演示扩展", "all", NL_CAP_THREAD_SAFE,
    nl_my_ext_init, nl_my_ext_shutdown, nl_my_ext_is_available, nl_my_ext_version,
    nl_my_ext_lazy_load, nl_my_ext_lazy_unload      /* ← 懒加载回调 */
);

/* 宿主机上按需加载/卸载/查状态 */
nl_extension_lazy_load("my_ext");                  /* 成功 0；已加载幂等 0 */
nl_extension_lazy_get_status("my_ext");            /* UNLOADED/LOADING/LOADED/STOPPING/STOPPED */
nl_extension_lazy_is_loaded("my_ext");             /* == LOADED */
nl_extension_lazy_unload("my_ext");                /* 仅在 LOADED 下成功 */
```

> **依赖管理**：用 `nl_extension_add_dependency(ext, "dep_id", NL_EXT_DEP_REQUIRED, "1.0.0")`
> 声明依赖（必需缺失则注册/校验失败），`NL_EXT_DEP_OPTIONAL` 表示可选；
> 详见 [`extension_showcase_api_guide.md`](../../docs/extension_showcase_api_guide.md) 第 10 节。

### 5.2 模块（`nl_module_*`）

模块比扩展更基础；本示例第 11/12 节用 `NL_MODULE_DEFINE_LAZY` 就地定义了一个演示模块：

```c
/* 定义模块（文件作用域，注意结尾分号） */
NL_MODULE_DEFINE_LAZY(NL_MODULE_CUSTOM, my_module, "1.0.0",
                      NL_CAP_THREAD_SAFE, 1, 1, 1,
                      NULL, NULL, NULL, my_module_version,
                      "演示模块", "NetLeaf Team",
                      my_module_lazy_load, my_module_lazy_unload);

/* 注册 / 注销 */
nl_module_register(NL_MODULE_GET_INFO(my_module));
nl_module_unregister(NL_MODULE_CUSTOM);

/* 模块级懒加载 */
nl_module_lazy_enable_module(NL_MODULE_CUSTOM);
nl_module_lazy_load(NL_MODULE_CUSTOM);
nl_module_lazy_is_loaded(NL_MODULE_CUSTOM);
nl_module_lazy_unload(NL_MODULE_CUSTOM);

/* 查询 */
nl_module_get_info(NL_MODULE_CORE);
nl_module_get_count();
nl_module_set_enabled(NL_MODULE_CUSTOM, 0);      /* 禁用 */
char* plats = nl_module_get_platforms(NL_MODULE_CUSTOM); /* 返回 malloc 串，需 free */
free(plats);
nl_print_modules();                              /* 打印全部模块 */
```

### 5.3 插件（`nl_plugin_*`，第三方动态库）

插件是“第三方 `.dll/.so` 动态加载”（`dlopen/LoadLibrary`），handle 制，与上面的扩展/模块
（主库注册表单元）不同。最小流程（与 [examples/plugin_example/test_extension.c](../plugin_example/test_extension.c) 一致）：

```c
#include "netleaf_module.h"

/* 1. 加载插件动态库（失败返回 NULL，原因用 nl_plugin_get_error() 取） */
nl_plugin_handle_t ext = nl_plugin_load("./netleaf_extension.so");  /* Windows: netleaf_extension.dll */
if (!ext) { printf("load failed: %s\n", nl_plugin_get_error()); }

/* 2. 取扩展信息并注册进主库 */
nl_extension_info_t* info = ...; /* 从动态库解析 nl_example_get_extension_info 后调用（GetProcAddress/dlsym） */
nl_extension_register(info);

/* 3. 查描述符 / 已加载状态 */
nl_plugin_descriptor_t* desc = nl_plugin_get_descriptor(ext);
nl_plugin_is_loaded(ext);

/* 4. 卸载 */
nl_plugin_unload(ext);
```

增强插件管理 API（同一族，按需使用）：

```c
nl_plugin_handle_t* found = NULL;
int n = nl_plugin_discover("./plugins", &found, 16);   /* 扫描目录列出可用插件 */
nl_plugin_search("com.example.%", &found, 16);         /* 按关键字搜索 */
nl_plugin_reload(ext);                                   /* 热重载 */
nl_plugin_check_dependencies(ext, &missing);             /* 依赖校验 */
nl_plugin_enable_sandbox(ext, 1);                        /* 沙箱（可选） */
```

> 完整第三方插件最小模板见 [examples/plugin_template/](../plugin_template/)（1 个 C 文件即可构建为
> 第三方插件，导出 `nl_<module>_get_extension_info` 入口 + `NL_PLUGIN_EXPORT` 宏）；
> 详见 [插件开发指南](../../docs/plugin_development.md)。

---

## 6. 相关文档

- 扩展系统开发教程：[docs/extension_tutorial.md](../../docs/extension_tutorial.md)
- 本示例 API 使用指南：[docs/extension_showcase_api_guide.md](../../docs/extension_showcase_api_guide.md)
- 插件开发指南：[docs/plugin_development.md](../../docs/plugin_development.md)

---

## 7. 许可证

本示例目录（`examples/extension_showcase/`）拥有**自有许可证**，见 [LICENSE](LICENSE)（MIT）。
NetLeaf 主工程及其它部分的许可证见源码树根目录的 [LICENSE](../../LICENSE)。
