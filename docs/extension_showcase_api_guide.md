# 扩展示例 API 使用指南（extension_showcase）

> 适用范围：NetLeaf `2.4.1` / NL 扩展系统 `NL_MODULE_VERSION = 2.4.1`
> 配套示例：[examples/extension_showcase/](../examples/extension_showcase/)
> 相关文档：[扩展系统开发教程](extension_tutorial.md) · [插件开发指南](plugin_development.md)

本指南以 [examples/extension_showcase/](../examples/extension_showcase/) 为蓝本，
按“**能做什么 → 怎么用 → 示例中的写法 → 注意事项**”的形式，讲解与扩展相关的公开 API 的
实际用法。示例本身是可编译、可运行、可自检的（结尾打印 `PASS=192 FAIL=0`），
建议边看文档边对照源码阅读。

---

## 0. 快速索引

| 主题 | 指南小节 | 示例节号 |
| --- | --- | --- |
| 定义扩展（宏） | [2](#2-定义扩展) | `showcase_extension.c` |
| 注册 / 注销 / 编号 | [3](#3-注册注销与编号) | 2、15 |
| 生命周期 | [4](#4-生命周期) | 3、13、14、15 |
| 状态与信息查询 | [5](#5-状态与信息查询) | 3、4 |
| 搜索过滤 | [6](#6-搜索过滤) | 5 |
| 元数据 | [7](#7-元数据) | 6 |
| 自动加载目录 | [8](#8-自动加载目录) | 7 |
| 访问与扩展间调用 | [9](#9-访问与扩展间调用) | 8 |
| 依赖管理 | [10](#10-依赖管理) | 9 |
| 扩展级懒加载 | [11](#11-扩展级懒加载) | 10 |
| 模块级懒加载与模块 API | [12](#12-模块级懒加载与模块-api) | 11、12 |
| Lang 错误码与多语言 | [13](#13-lang-错误码注册与多语言) | 16 |
| Lang 变量系统 | [14](#14-lang-变量系统) | 17 |

---

## 1. 整体结构

示例由三部分组成，正好对应扩展开发的最小闭环：

```
showcase_extension.c   ——  扩展库（.dll/.so）
        │  定义 3 个扩展，导出 get_extension_info
        ▼
extension_showcase_demo.c ——  宿主程序（可执行文件）
        │  注册扩展 → 使用 API → 断言自检
        ▼
主库注册表（netleaf.dll 内）——  所有扩展共享同一份注册表
```

> **关键前提**：扩展的全局状态由主库注册表持有，所以扩展必须与主库处于**同一动态库实例**。
> 示例的做法是：扩展库链接 `netleaf`，宿主也链接 `netleaf`，从而共享同一份注册表。

---

## 2. 定义扩展

### 2.1 `NL_EXTENSION_DEFINE`（普通扩展）

```c
NL_EXTENSION_DEFINE(
    showcase_engine,                 // ext_id → 同时作为 library_id 字符串
    "Showcase Engine Extension",     // 显示名
    SHOWCASE_ENGINE_VERSION,         // 版本
    "NetLeaf Team",                  // 作者
    "示例扩展：演示依赖声明、生命周期与扩展间函数解析",  // 描述（≤150 字节）
    "Windows,Linux,MacOS",           // 平台串，或 "all"
    NL_CAP_SERVER | NL_CAP_THREAD_SAFE,
    nl_showcase_engine_init,
    nl_showcase_engine_shutdown,
    nl_showcase_engine_is_available,
    nl_showcase_engine_version
);
```

要点：

- 宏**不带结尾分号**，调用后要自己写 `;`。
- 生成 `static nl_extension_info_t nl_extension_info_<ext_id>`；用 `NL_EXTENSION_GET_INFO(ext_id)` 取指针。
- `caps` 会自动追补 `NL_CAP_DYNAMIC | NL_CAP_LAZY_LOAD | NL_CAP_EXT_SYSTEM`。
- `library_value` 由主库在注册时分配，**不要手写**。

### 2.2 `NL_EXTENSION_DEFINE_LAZY`（带懒加载）

在上一节参数之后追加 `lazy_load_fn, lazy_unload_fn` 两个回调：

```c
NL_EXTENSION_DEFINE_LAZY(
    showcase_codec, "Showcase Codec Extension", SHOWCASE_CODEC_VERSION, "NetLeaf Team",
    "示例扩展：演示懒加载与依赖被依赖关系", "Windows,Linux,MacOS",
    NL_CAP_THREAD_SAFE | NL_CAP_ASYNC,
    nl_showcase_codec_init, nl_showcase_codec_shutdown,
    nl_showcase_codec_is_available, nl_showcase_codec_version,
    nl_showcase_codec_lazy_load, nl_showcase_codec_lazy_unload   // ← 懒加载回调
);
```

回调签名：`void* (*lazy_load)(void)` 返回非 NULL 视为成功；`void (*lazy_unload)(void)` 释放资源。

### 2.3 导出给宿主

```c
NL_EXT_API nl_extension_info_t* nl_showcase_engine_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(showcase_engine);
}
```

`NL_EXT_API` 在**扩展库编译期**（定义 `NL_EXTENSION_EXPORTS`）展开为 `__declspec(dllexport)`，
宿主侧为 `__declspec(dllimport)`，非 Windows 平台为空。

---

## 3. 注册、注销与编号

```c
int32_t codec_value = -1;

/* 注册：成功返回 0；对同一 id 重复注册返回 0（幂等） */
nl_extension_register(nl_showcase_codec_get_extension_info());

/* 编号：注册时自动分配 library_value（从 1 开始，0 保留） */
codec_value = nl_extension_get_value_by_id("showcase_codec");     // 例如 1
const char* id = nl_extension_get_id_by_value(codec_value);       // 反查 → "showcase_codec"

/* 计数与快照（get_all 返回 malloc 的数组，用完需 free） */
int count = nl_extension_get_count();
int all_n = 0;
nl_extension_info_t** all = nl_extension_get_all(&all_n);
free(all);

/* 描述长度校验：合法返回 1，超长返回 0（上限 NL_EXTENSION_DESC_MAX_LEN=150 字节） */
if (!nl_extension_validate_description(desc)) { /* 处理超长描述 */ }

/* 注销：成功 0，不存在 -1 */
nl_extension_unregister("showcase_metrics");
```

---

## 4. 生命周期

```c
nl_extension_init("showcase_codec");          // 调用 ext->init()，成功 0
nl_extension_shutdown("showcase_codec");      // 调用 ext->shutdown()
nl_extension_reload("showcase_engine");       // shutdown + init
nl_extension_reload_all();                    // 批量重载，返回成功数

nl_extension_init_all();                      // 批量 init，返回成功数
nl_extension_shutdown_all();                  // 批量 shutdown，返回执行数
nl_extension_force_shutdown("showcase_engine"); // 关闭并注销
nl_extension_force_shutdown_all();            // 清空注册表，返回清理数
```

推荐生命周期：`register → init → 使用 → shutdown → unregister`。

> 注意：`init/shutdown_all` 只遍历“当前已注册”的扩展；`force_shutdown_all` 会同时清空注册表。

---

## 5. 状态与信息查询

```c
nl_extension_is_initialized("showcase_codec");   // 1/0
nl_extension_is_running("showcase_engine");      // 1/0
nl_extension_get_state("showcase_engine");       // nl_module_status_t，如 NL_MODULE_STATUS_INITIALIZED
nl_extension_is_loaded("showcase_engine");       // 是否已注册
nl_extension_is_available("showcase_codec");     // 调用 ext->is_available()

nl_extension_get_name("showcase_engine");
nl_extension_get_author("showcase_engine");
nl_extension_get_description("showcase_engine");
nl_extension_get_caps("showcase_engine");        // 与 get_capabilities 等价
nl_extension_get_capabilities("showcase_engine");
nl_extension_get_platforms("showcase_codec");    // 原始平台串
nl_extension_supports_platform("showcase_engine", "linux");  // 1/0
nl_extension_get_version_by_id("showcase_codec", buf, sizeof(buf));
```

> 说明：`is_initialized` / `is_running` / `get_state` 内部以 `ext->is_available()` 为准，
> 因此对“始终可用”的示例扩展会返回 1 / `INITIALIZED`。

---

## 6. 搜索过滤

```c
nl_extension_info_t* results[16];

int n1 = nl_extension_find_by_capability(NL_CAP_THREAD_SAFE, results, 16);
int n2 = nl_extension_find_by_platform("windows", results, 16);
int n3 = nl_extension_find_by_name_pattern("showcase_", results, 16);
```

返回命中数量并写入调用者数组（不超过 `max_count`）。`find_by_name_pattern` 对
`library_id` 与 `library_name` 做子串匹配，`find_by_platform` 大小写不敏感。

---

## 7. 元数据

扩展级元数据是一组“扩展 id + key → value”的键值对，用于挂载扩展的自定义信息。

```c
char buf[128];

/* 写入 / 覆盖 */
nl_extension_set_metadata("showcase_engine", "homepage", "https://netleaf.local");

/* 读取：找到返回 0，未找到返回 -1；value 会被写为 NUL 结尾字符串 */
nl_extension_get_metadata("showcase_engine", "homepage", buf, sizeof(buf));

/* 清空某个 key 的值：把 value 传 NULL（key 仍保留，读取时得到空串） */
nl_extension_set_metadata("showcase_engine", "homepage", NULL);
nl_extension_get_metadata("showcase_engine", "homepage", buf, sizeof(buf)); // buf = ""
```

> 注意：`library_id` / `key` 为 NULL 时 `set` 直接返回 -1；`get` 对不存在的 key 返回 -1。

---

## 8. 自动加载目录

主库在 `nl_modules_init()` 时会尝试自动加载扩展目录（默认 `extensions/`）：

```c
nl_extension_set_auto_load_dir("extensions");
const char* dir = nl_extension_get_auto_load_dir();

int n1 = nl_extension_auto_load();                       // 从默认目录加载
int n2 = nl_extension_auto_load_from_dir("./plugins");   // 从指定目录加载
```

自动加载会按约定的导出符号名（如 `nl_lang_get_extension_info`）识别扩展库；
目录不存在时返回 0（正常情况，不报错）。

---

## 9. 访问与扩展间调用

```c
/* 只读访问 */
nl_extension_info_t* ext = nl_extension_access("showcase_engine");

/* 通过扩展自带的函数解析器按名字取函数指针 */
typedef const char* (*version_fn)(void);
version_fn v = (version_fn)nl_extension_get_func(ext, "version");
version_fn w = (version_fn)nl_extension_get_func_by_id("showcase_engine", "version");
```

扩展需在 `init()` 中把自己的解析器挂到信息结构体上：

```c
int nl_showcase_engine_init(void) {
    nl_extension_info_t* self = NL_EXTENSION_GET_INFO(showcase_engine);
    self->get_extension_function = nl_showcase_engine_resolve;  // 解析器
    return 0;
}
```

遍历：

```c
static int cb(nl_extension_info_t* ext, void* userdata) {
    (*(int*)userdata)++;
    return 0;   /* 返回非 0 可提前终止 */
}
int cnt = 0;
int rc = nl_extension_iterate(cb, &cnt);
```

> 说明：函数指针与 `void*` 互转依赖平台 ABI，`-Wpedantic` 下会产生告警，属预期行为。

---

## 10. 依赖管理

```c
nl_extension_info_t* engine = nl_extension_access("showcase_engine");

/* 声明依赖 */
nl_extension_add_dependency(engine, "showcase_codec",  NL_EXT_DEP_REQUIRED, "1.0.0");
nl_extension_add_dependency(engine, "showcase_legacy", NL_EXT_DEP_OPTIONAL, NULL);

/* 查询 */
nl_extension_get_dependency_count(engine);                 // 总数
nl_extension_get_dependencies(engine);                     // 链表头
nl_extension_get_dependency_by_id(engine, "showcase_codec");
nl_extension_has_dependency(engine, "showcase_codec");     // 1/0

nl_extension_dependency_t* deps[8];
nl_extension_get_required_deps(engine, deps, 8);
nl_extension_get_optional_deps(engine, deps, 8);

/* 校验与解析 */
nl_extension_check_dependencies(engine, 1);   // require_all=1：必需依赖缺失则 -1
nl_extension_resolve_dependencies(engine);
nl_extension_are_dependencies_met(engine);

/* 移除 / 清空 */
nl_extension_remove_dependency(engine, "showcase_legacy");
nl_extension_clear_dependencies(engine);
```

依赖类型：`NL_EXT_DEP_REQUIRED`（必需，缺失视为失败）、`NL_EXT_DEP_OPTIONAL`（可选）。

---

## 11. 扩展级懒加载

`NL_EXTENSION_DEFINE_LAZY` 定义的扩展可用下面 4 个函数按需加载/卸载：

```c
/* 按需加载：成功 0；无 lazy_load 回调返回 -1；已加载时幂等返回 0 */
nl_extension_lazy_load("showcase_codec");

/* 查询状态：UNLOADED / LOADING / LOADED / STOPPING / STOPPED */
nl_module_lazy_status_t st = nl_extension_lazy_get_status("showcase_codec");
int loaded = nl_extension_lazy_is_loaded("showcase_codec");   // == LOADED

/* 卸载：仅在 LOADED 状态下成功（返回 0），否则 -1 */
nl_extension_lazy_unload("showcase_codec");
```

状态机：

```
UNLOADED --lazy_load--> LOADED --lazy_unload--> STOPPED --lazy_load--> LOADED ...
```

> 这与“模块级懒加载”（见下节）是两套 API：本节的函数作用于**扩展**，
> `nl_module_lazy_*` 作用于**模块**。

---

## 12. 模块级懒加载与模块 API

模块是主库注册表里的更基础单元；示例用 `NL_MODULE_DEFINE_LAZY` 就地定义了一个演示模块。

```c
/* 定义（文件作用域，注意结尾分号） */
NL_MODULE_DEFINE_LAZY(NL_MODULE_CUSTOM, showcase_module, "1.0.0",
                      NL_CAP_THREAD_SAFE, 1, 1, 1,
                      NULL, NULL, NULL, showcase_module_version,
                      "演示模块懒加载与依赖", "NetLeaf Team",
                      showcase_module_lazy_load, showcase_module_lazy_unload);

/* 注册 / 注销 */
nl_module_register(NL_MODULE_GET_INFO(showcase_module));
nl_module_unregister(NL_MODULE_CUSTOM);

/* 懒加载 */
nl_module_lazy_enable(1);
nl_module_lazy_enable_module(NL_MODULE_CUSTOM);
nl_module_lazy_is_enabled(NL_MODULE_CUSTOM);
nl_module_lazy_load(NL_MODULE_CUSTOM);
nl_module_lazy_get_status(NL_MODULE_CUSTOM);
nl_module_lazy_is_loaded(NL_MODULE_CUSTOM);
nl_module_lazy_unload(NL_MODULE_CUSTOM);
nl_module_lazy_preload_all();
nl_module_lazy_unload_all();
nl_module_lazy_clear_cache();
nl_module_lazy_disable_module(NL_MODULE_CUSTOM);

/* 查询 */
nl_module_get_info(NL_MODULE_CORE);
nl_module_get_info_by_name("netleaf");
nl_module_get_all(mods, 16);
nl_module_get_count();
nl_get_module(NL_MODULE_CUSTOM);
nl_module_available("netleaf");
nl_get_modules(names, 16);
nl_module_is_platform_supported(NL_MODULE_CUSTOM);
nl_module_get_status(NL_MODULE_CUSTOM);
nl_module_set_enabled(NL_MODULE_CUSTOM, 0);   // 禁用
nl_module_get_name / _version / _description(NL_MODULE_CUSTOM);
nl_module_has_capability(NL_MODULE_CUSTOM, NL_CAP_THREAD_SAFE);
nl_module_get_capabilities(NL_MODULE_CUSTOM);

/* 模块依赖 */
nl_module_add_dependency(NL_MODULE_CUSTOM, NL_MODULE_CORE);
nl_module_check_dependencies(NL_MODULE_CUSTOM);
nl_module_get_dependencies(NL_MODULE_CUSTOM);
nl_module_remove_dependency(NL_MODULE_CUSTOM, NL_MODULE_CORE);

nl_print_modules();
```

---

## 13. Lang 错误码注册与多语言

`netleaf_lang` 提供了“表驱动”的错误码注册宏，一次性登记英文与中文两份消息。

### 13.1 定义错误码表

```c
#define SHOWCASE_LIB_ID 0x1000   /* 自定义库编号，避免与内置 NL_LIB_* 冲突 */

NL_ERROR_BEGIN(showcase_error_table)
    NL_ERROR(0,  "Success", "成功")
    NL_ERROR(-1, "Showcase operation failed", "示例操作失败")
    NL_ERROR(-2, "Required dependency missing", "缺少必需依赖")
    NL_ERROR(-3, "Connection limit {{<var>max_conn</var>}} exceeded",
                "连接数已超过上限 {{<var>max_conn</var>}}")
NL_ERROR_END
```

- `NL_ERROR_BEGIN(name)` 展开为 `static const nl_lang_error_def_t name[] = {`；
- `NL_ERROR(code, en, zh)` 增加一条 `{code, en_us, zh_cn}`；
- `NL_ERROR_END` 展开为 `};`（**不要再补分号**）。

### 13.2 注册与查询

```c
nl_lang_register_errors(SHOWCASE_LIB_ID, showcase_error_table);  // 一次注册 en_us + zh_cn

nl_lang_is_registered(SHOWCASE_LIB_ID);        // 1
nl_lang_has_language(SHOWCASE_LIB_ID, "zh_cn");// 1
nl_lang_has_error(SHOWCASE_LIB_ID, -1);        // 1

int code_n = 0;
int* codes = nl_lang_get_error_codes(SHOWCASE_LIB_ID, &code_n);  // 需 free(codes)

nl_lang_register_lib_name(SHOWCASE_LIB_ID, "showcase");
nl_lang_get_lib_name(SHOWCASE_LIB_ID);         // "showcase"
```

### 13.3 多语言取值

```c
nl_lang_set("en_us");
const char* en = nl_lang_get_error(SHOWCASE_LIB_ID, -1);        // "Showcase operation failed"

nl_lang_set("zh_cn");
const char* zh = nl_lang_get_error(SHOWCASE_LIB_ID, -1);        // "示例操作失败"

const char* en2 = nl_lang_get_error_for(SHOWCASE_LIB_ID, -1, "en_us"); // 不受当前语言影响
```

工具函数：

```c
nl_error_is_success(0);                     // 1
nl_lang_get_error_category(-1);             // "Parameter"
nl_lang_set("en-US");                       // 非法格式（缺下划线）→ -1
```

> 语言码必须是 `xx_xx` 形式（大小写不敏感、内部规范化为小写），例如 `en_us`、`zh_cn`、`ja_jp`。

### 13.4 注册真实扩展信息（可选）

```c
nl_extension_register(nl_lang_get_extension_info());   // 把 lang 扩展注册进主库
nl_extension_get_version_by_id("lang", buf, sizeof(buf));
```

---

## 14. Lang 变量系统

### 14.1 静态与类型化变量

```c
nl_lang_var_set("showcase_project", "NetLeaf");
nl_lang_var_get("showcase_project");           // "NetLeaf"
nl_lang_var_exists("showcase_project");        // 1

nl_lang_var_set_int("max_conn", 1024);
nl_lang_var_get_int("max_conn", 0);            // 1024（缺失时返回默认值 0）

nl_lang_var_set_float("ratio", 0.75);
nl_lang_var_get_float("ratio", 0.0);           // 0.75

nl_lang_var_set_bool("verbose", 1);
nl_lang_var_get_bool("verbose", 0);            // 1

nl_lang_var_remove("showcase_project");
nl_lang_var_clear_all();
```

> `nl_lang_var_get` 返回指向**内部静态缓冲区**的指针，会被下一次调用覆盖；
> 如需保留请立即拷贝。

### 14.2 变量替换

```c
char buf[128];
nl_lang_var_replace("Hello {{showcase_project}}!", buf, sizeof(buf));    // "Hello NetLeaf!"

nl_lang_var_replace_html("port={{<var>max_conn</var>}}", buf, sizeof(buf)); // "port=1024"
```

### 14.3 条件求值

```c
nl_lang_var_condition_eval("max_conn > 100");   // 1
nl_lang_var_condition_eval("max_conn < 100");   // 0
```

### 14.4 动态变量（provider）

provider 每次访问都会重新计算值：

```c
static int counter = 0;
static int my_provider(const char* name, char* out, size_t out_size, void* userdata) {
    (void)name; (void)userdata;
    snprintf(out, out_size, "dynamic-%d", ++counter);
    return 1;   /* 返回非 0 表示已写入值 */
}

nl_lang_var_set_provider("showcase_now", my_provider, NULL);
nl_lang_var_is_dynamic("showcase_now");         // 1
nl_lang_var_get("showcase_now");                // "dynamic-1"
nl_lang_var_get("showcase_now");                // "dynamic-2"（重新求值）
```

### 14.5 外部变量（环境变量）

```c
/* 绑定：变量每次访问都读取指定环境变量 */
nl_lang_var_bind_env("showcase_token", "NL_SHOWCASE_TOKEN");
nl_lang_var_is_dynamic("showcase_token");       // 1
nl_lang_var_get("showcase_token");              // 读取 NL_SHOWCASE_TOKEN 当前值

/* 按前缀批量导入环境变量为普通变量 */
nl_lang_var_load_env("NL_SHOWCASE_DEMO_");      // 导入 NL_SHOWCASE_DEMO_A / _B ...
nl_lang_var_exists("NL_SHOWCASE_DEMO_A");
```

### 14.6 带变量的错误消息

消息模板中的 `{{<var>NAME</var>}}` 会在输出时用变量值替换：

```c
char buf[256];
nl_lang_get_error_with_vars(SHOWCASE_LIB_ID, -3, buf, sizeof(buf));
/* 例："Connection limit 1024 exceeded"（max_conn=1024） */
```

> 环境变量演示使用运行期写入的**演示用假数据**（`env-value-1` 等），
> 不包含任何真实密钥或令牌。

---

## 15. 自检与验证

运行示例会逐项打印 `[PASS]`/`[FAIL]`，结尾统计：

```
  示例结束: PASS=192 FAIL=0
```

- 退出码 `0`：全部通过；
- 退出码 `1`：存在 `[FAIL]`，可直接用于 CI。

若只需做语法检查（不必先构建主库）：

```bash
gcc -fsyntax-only -std=c99 -Wall -Wextra -Wpedantic \
    -Iinclude -Iexamples/extension_showcase \
    examples/extension_showcase/extension_showcase_demo.c
```

`-Wpedantic` 下会出现若干“函数指针 ↔ `void*` 互转”的告警（`get_func` 系列用法导致），
属预期行为，不影响功能。
