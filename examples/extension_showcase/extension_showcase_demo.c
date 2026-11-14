/**
 * @file extension_showcase_demo.c
 * @brief NL 扩展系统综合示例 - 宿主演示程序
 *
 * 本程序是 examples/extension_showcase 示例的可执行宿主，逐节演示
 * include/netleaf_module.h 与 include/netleaf_lang.h 中与“扩展”相关的全部公开 API。
 * 每一节都会做 PASS/FAIL 自检；进程返回值 0 表示全部通过，1 表示存在失败项，
 * 因此该程序本身即可作为回归自检使用。
 *
 * 覆盖范围（与 include/ 下公开头文件一一对应）：
 *   1.  宏定义       NL_EXTENSION_DEFINE / NL_EXTENSION_DEFINE_LAZY / NL_EXTENSION_GET_INFO /
 *                    NL_EXT_API / NL_PARSE_PLATFORMS_* / NL_CAP_*
 *   2.  注册与注销   nl_extension_register / unregister / get_count / get_all /
 *                    validate_description / get_value_by_id / get_id_by_value
 *   3.  生命周期     init / shutdown / force_shutdown / reload / reload_all /
 *                    init_all / shutdown_all / force_shutdown_all
 *   4.  状态查询     is_initialized / is_running / get_state / is_available / is_loaded
 *   5.  信息查询     get_info / get_name / get_author / get_description / get_caps /
 *                    supports_platform / get_version_by_id / get_platforms / get_capabilities
 *   6.  搜索过滤     find_by_capability / find_by_platform / find_by_name_pattern
 *   7.  元数据       set_metadata / get_metadata / 值置空（清空语义）
 *   8.  自动加载     set_auto_load_dir / get_auto_load_dir / auto_load / auto_load_from_dir
 *   9.  访问与调用   access / get_func / get_func_by_id / iterate
 *   10. 依赖管理     add/remove/clear_dependency、get_dependency_count / get_dependencies /
 *                    get_dependency_by_id / has_dependency / get_required_deps /
 *                    get_optional_deps / check_dependencies / resolve_dependencies /
 *                    are_dependencies_met
 *   11. 扩展级懒加载 nl_extension_lazy_load / lazy_unload / lazy_get_status / lazy_is_loaded
 *   12. 模块级懒加载 nl_module_lazy_*（NL_MODULE_DEFINE_LAZY 定义的演示模块）
 *   13. 模块查询     nl_module_* / nl_get_module* / nl_module_available / nl_print_modules
 *   14. 模块依赖     nl_module_add_dependency / remove_dependency / check_dependencies /
 *                    get_dependencies
 *   15. Lang 错误码   NL_ERROR_BEGIN / NL_ERROR / NL_ERROR_END、nl_lang_register_errors、
 *                    nl_lang_get_error / get_error_for / is_registered / has_error /
 *                    register_lib_name / get_error_codes、多语言切换 nl_lang_set
 *   16. Lang 变量     静态/类型化变量、动态变量 provider、外部环境变量 env、
 *                    变量替换 replace / replace_html、条件求值 condition_eval、
 *                    带变量的错误消息 get_error_with_vars
 *
 * 运行说明：本程序与扩展库 netleaf_extension_showcase、NetLeaf 的 lang 扩展库
 *          （netleaf_lang）链接，并与 netleaf 主库共享同一进程内的扩展注册表。
 *          本示例不包含任何业务/负载逻辑，仅演示扩展系统 API 的用法。
 */

/* 让 setenv/unsetenv 在非 Windows 平台可见（用于环境变量演示） */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "netleaf.h"
#include "netleaf_module.h"
#include "netleaf_lang.h"
#include "showcase_extension.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================
 * 自检辅助
 * ========================================= */

static int g_pass = 0;
static int g_fail = 0;

static void check(const char* what, int cond) {
    printf("    [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (cond) g_pass++; else g_fail++;
}

static void section(const char* title) {
    printf("\n--------------------------------------------------\n");
    printf(">> %s\n", title);
    printf("--------------------------------------------------\n");
}

/* =========================================
 * 环境变量写入辅助（仅用于演示外部变量 env，值为演示用假数据，非真实密钥）
 * ========================================= */

static void showcase_set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void showcase_unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

/* =========================================
 * 演示模块：用 NL_MODULE_DEFINE_LAZY 定义一个支持懒加载的模块
 * （用于演示模块级懒加载 nl_module_lazy_* 与模块注册/注销）
 * ========================================= */

static int g_demo_module_ready = 0;

static void* showcase_module_lazy_load(void) {
    g_demo_module_ready = 1;
    printf("    [module] showcase_module: lazy_load 已执行\n");
    return &g_demo_module_ready;
}

static void showcase_module_lazy_unload(void) {
    g_demo_module_ready = 0;
    printf("    [module] showcase_module: lazy_unload 已执行\n");
}

static const char* showcase_module_version(void) {
    return "1.0.0";
}

NL_MODULE_DEFINE_LAZY(
    NL_MODULE_CUSTOM,
    showcase_module,
    "1.0.0",
    NL_CAP_THREAD_SAFE,
    1, 1, 1,
    NULL, NULL, NULL,
    showcase_module_version,
    "演示模块懒加载与依赖",
    "NetLeaf Team",
    showcase_module_lazy_load,
    showcase_module_lazy_unload
);

/* =========================================
 * Lang 错误码表（v2.4.1 表驱动注册）
 *
 * NL_ERROR_BEGIN / NL_ERROR / NL_ERROR_END 会在文件作用域生成一张
 * static const nl_lang_error_def_t[] 表（英文 en_us + 中文 zh_cn），
 * 之后用 nl_lang_register_errors() 一次性注册。
 * ========================================= */

#define SHOWCASE_LIB_ID 0x1000

NL_ERROR_BEGIN(showcase_error_table)
    NL_ERROR(0,  "Success", "成功")
    NL_ERROR(-1, "Showcase operation failed", "示例操作失败")
    NL_ERROR(-2, "Required dependency missing", "缺少必需依赖")
    NL_ERROR(-3, "Connection limit {{<var>max_conn</var>}} exceeded",
                "连接数已超过上限 {{<var>max_conn</var>}}")
NL_ERROR_END

/* =========================================
 * 动态变量 provider：每次访问时重新计算值
 * ========================================= */

static int g_provider_calls = 0;

static int showcase_dynamic_provider(const char* name, char* out, size_t out_size, void* userdata) {
    (void)name;
    (void)userdata;
    g_provider_calls++;
    snprintf(out, out_size, "dynamic-%d", g_provider_calls);
    return 1; /* 返回非 0 表示已写入值 */
}

/* =========================================
 * 扩展遍历回调
 * ========================================= */

static int iterate_cb(nl_extension_info_t* ext, void* userdata) {
    int* counter = (int*)userdata;
    if (counter) (*counter)++;
    printf("      迭代到扩展: id=%s name=%s v%s\n",
           ext->library_id,
           ext->library_name ? ext->library_name : "(null)",
           ext->version ? ext->version : "0.0.0");
    return 0; /* 返回 0 继续迭代 */
}

/* =========================================
 * main
 * ========================================= */

int main(void) {
    printf("\n");
    printf("==================================================\n");
    printf("  NetLeaf NL 扩展系统综合示例 (NL_MODULE_VERSION=%s)\n", NL_MODULE_VERSION);
    printf("  系统名称: %s  主库版本: %s\n", NL_SYSTEM_NAME, nl_version_string());
    printf("==================================================\n");

    /* ==================================================
     * 1. 初始化主库
     * ================================================== */
    section("1. 初始化主库 nl_modules_init()");
    nl_modules_init();
    printf("    模块数量: %d\n", nl_get_module_count());
    printf("    扩展数量: %d\n", nl_extension_get_count());
    check("主库已初始化，核心模块已注册", nl_get_module_count() >= 1);

    /* ==================================================
     * 2. 注册扩展 (nl_extension_register)
     * ================================================== */
    section("2. 注册扩展 (nl_extension_register)");

    /* 通过扩展库导出的 get_extension_info 取到信息结构体，再注册到主库注册表 */
    nl_extension_info_t* codec_info   = nl_showcase_codec_get_extension_info();
    nl_extension_info_t* engine_info  = nl_showcase_engine_get_extension_info();
    nl_extension_info_t* metrics_info = nl_showcase_metrics_get_extension_info();

    check("获取 codec 扩展信息成功",  codec_info   != NULL);
    check("获取 engine 扩展信息成功", engine_info  != NULL);
    check("获取 metrics 扩展信息成功", metrics_info != NULL);

    /* 被依赖的扩展先注册 */
    check("注册 codec 扩展",   nl_extension_register(codec_info)   == 0);
    check("注册 engine 扩展",  nl_extension_register(engine_info)  == 0);
    check("注册 metrics 扩展", nl_extension_register(metrics_info) == 0);
    check("重复注册返回 0（幂等）", nl_extension_register(codec_info) == 0);
    printf("    当前扩展数量: %d\n", nl_extension_get_count());
    check("扩展数量为 3", nl_extension_get_count() == 3);

    /* 主库在注册时自动分配 library_value（只读，从 1 开始） */
    int32_t codec_value = nl_extension_get_value_by_id(SHOWCASE_CODEC_ID);
    printf("    codec library_value = %d\n", (int)codec_value);
    check("get_value_by_id 返回有效值", codec_value > 0);
    const char* id_from_value = nl_extension_get_id_by_value(codec_value);
    check("get_id_by_value 反查一致",
          id_from_value && strcmp(id_from_value, SHOWCASE_CODEC_ID) == 0);

    /* 描述长度校验（NL_EXTENSION_DESC_MAX_LEN = 150 字节） */
    char long_desc[200];
    memset(long_desc, 'A', sizeof(long_desc) - 1);
    long_desc[sizeof(long_desc) - 1] = '\0';
    check("较短描述校验通过", nl_extension_validate_description("简短描述") == 1);
    check("超长描述校验失败", nl_extension_validate_description(long_desc) == 0);

    /* NL_PARSE_PLATFORMS_* 平台串解析宏 */
    const char* plats = nl_extension_get_platforms(SHOWCASE_CODEC_ID);
    printf("    codec 平台串: %s\n", plats ? plats : "(null)");
    check("NL_PARSE_PLATFORMS_WIN 识别 Windows",
          plats && NL_PARSE_PLATFORMS_WIN(plats) != 0);
    check("NL_PARSE_PLATFORMS_LINUX 识别 Linux",
          plats && NL_PARSE_PLATFORMS_LINUX(plats) != 0);
    check("NL_PARSE_PLATFORMS_MACOS 识别 MacOS",
          plats && NL_PARSE_PLATFORMS_MACOS(plats) != 0);

    /* NL_CAP_* 能力位辅助宏 */
    uint32_t engine_caps = engine_info->capabilities;
    printf("    engine 能力位: 0x%x\n", engine_caps);
    check("NL_CAP_HAS 检测 NL_CAP_THREAD_SAFE",
          NL_CAP_HAS(engine_caps, NL_CAP_THREAD_SAFE) != 0);
    check("扩展自动附带 NL_CAP_DYNAMIC / NL_CAP_EXT_SYSTEM",
          NL_CAP_HAS(engine_caps, NL_CAP_DYNAMIC) && NL_CAP_HAS(engine_caps, NL_CAP_EXT_SYSTEM));
    {
        uint32_t tmp = engine_caps;
        NL_CAP_ADD(tmp, NL_CAP_ASYNC);
        check("NL_CAP_ADD 添加能力位", NL_CAP_HAS(tmp, NL_CAP_ASYNC) != 0);
        NL_CAP_REMOVE(tmp, NL_CAP_ASYNC);
        check("NL_CAP_REMOVE 移除能力位", NL_CAP_HAS(tmp, NL_CAP_ASYNC) == 0);
    }

    /* ==================================================
     * 3. 生命周期: init / 状态查询
     * ================================================== */
    section("3. 生命周期: init / 状态查询");
    check("init codec 扩展",   nl_extension_init(SHOWCASE_CODEC_ID)   == 0);
    check("init engine 扩展",  nl_extension_init(SHOWCASE_ENGINE_ID)  == 0);
    check("init metrics 扩展", nl_extension_init(SHOWCASE_METRICS_ID) == 0);
    check("重复 init 幂等",     nl_extension_init(SHOWCASE_CODEC_ID)   == 0);

    check("codec is_initialized", nl_extension_is_initialized(SHOWCASE_CODEC_ID) == 1);
    check("engine is_running",    nl_extension_is_running(SHOWCASE_ENGINE_ID) == 1);
    printf("    engine get_state = %d (NL_MODULE_STATUS_INITIALIZED=%d)\n",
           (int)nl_extension_get_state(SHOWCASE_ENGINE_ID), (int)NL_MODULE_STATUS_INITIALIZED);
    check("engine get_state == INITIALIZED",
          nl_extension_get_state(SHOWCASE_ENGINE_ID) == NL_MODULE_STATUS_INITIALIZED);
    check("未注册扩展 get_state == UNINITIALIZED",
          nl_extension_get_state("not_registered") == NL_MODULE_STATUS_UNINITIALIZED);

    /* ==================================================
     * 4. 信息查询 (get_info / get_name / get_author / ...)
     * ================================================== */
    section("4. 信息查询 (get_info / get_name / get_author / ...)");
    nl_extension_info_t* found = nl_extension_get_info(SHOWCASE_ENGINE_ID);
    check("get_info 找到 engine", found == engine_info);
    printf("    name        = %s\n", nl_extension_get_name(SHOWCASE_ENGINE_ID));
    printf("    author      = %s\n", nl_extension_get_author(SHOWCASE_ENGINE_ID));
    printf("    description = %s\n", nl_extension_get_description(SHOWCASE_ENGINE_ID));
    printf("    caps        = 0x%x\n", nl_extension_get_caps(SHOWCASE_ENGINE_ID));
    check("get_name 正确",
          strcmp(nl_extension_get_name(SHOWCASE_ENGINE_ID), "Showcase Engine Extension") == 0);
    check("get_author 正确",
          strcmp(nl_extension_get_author(SHOWCASE_ENGINE_ID), "NetLeaf Team") == 0);
    check("get_caps 与 get_capabilities 一致",
          nl_extension_get_caps(SHOWCASE_ENGINE_ID) == nl_extension_get_capabilities(SHOWCASE_ENGINE_ID));

    check("engine 支持 windows", nl_extension_supports_platform(SHOWCASE_ENGINE_ID, "windows") == 1);
    check("engine 支持 linux",   nl_extension_supports_platform(SHOWCASE_ENGINE_ID, "linux") == 1);
    check("engine 支持 macos",   nl_extension_supports_platform(SHOWCASE_ENGINE_ID, "macos") == 1);
    check("未知平台返回 0",       nl_extension_supports_platform(SHOWCASE_ENGINE_ID, "plan9") == 0);

    /* metrics 使用平台串 "all"，注册时应被解析为全平台 */
    check("metrics 平台串 'all' 解析为全平台",
          metrics_info->platform_windows && metrics_info->platform_linux && metrics_info->platform_macos);

    char ver_buf[32];
    check("get_version_by_id 成功",
          nl_extension_get_version_by_id(SHOWCASE_CODEC_ID, ver_buf, sizeof(ver_buf)) == 0);
    printf("    codec 版本(经 buf 获取) = %s\n", ver_buf);
    check("版本号一致", strcmp(ver_buf, SHOWCASE_CODEC_VERSION) == 0);

    /* ==================================================
     * 5. 搜索过滤 (find_by_capability / platform / name_pattern)
     * ================================================== */
    section("5. 搜索过滤 (find_by_capability / platform / name_pattern)");
    nl_extension_info_t* results[16];

    int n = nl_extension_find_by_capability(NL_CAP_THREAD_SAFE, results, 16);
    printf("    find_by_capability(NL_CAP_THREAD_SAFE) => %d\n", n);
    check("按能力搜索至少命中 3 个", n >= 3);

    n = nl_extension_find_by_platform("windows", results, 16);
    printf("    find_by_platform(windows) => %d\n", n);
    check("按平台 windows 搜索命中 3 个", n == 3);

    n = nl_extension_find_by_name_pattern("showcase_", results, 16);
    printf("    find_by_name_pattern(\"showcase_\") => %d\n", n);
    check("按名称模式搜索命中 3 个", n == 3);

    n = nl_extension_find_by_name_pattern("engine", results, 16);
    printf("    find_by_name_pattern(\"engine\") => %d\n", n);
    check("按名称模式 engine 命中 1 个", n == 1);

    /* ==================================================
     * 6. 元数据 (set_metadata / get_metadata / 清空)
     * ================================================== */
    section("6. 元数据 (nl_extension_set_metadata / get_metadata)");
    char meta_val[128];
    memset(meta_val, 0, sizeof(meta_val));
    check("set_metadata 成功",
          nl_extension_set_metadata(SHOWCASE_ENGINE_ID, "homepage", "https://netleaf.local") == 0);
    check("get_metadata 成功",
          nl_extension_get_metadata(SHOWCASE_ENGINE_ID, "homepage", meta_val, sizeof(meta_val)) == 0);
    printf("    读取到 homepage = \"%s\"\n", meta_val);
    check("元数据值一致", strcmp(meta_val, "https://netleaf.local") == 0);

    check("覆盖写入同一 key",
          nl_extension_set_metadata(SHOWCASE_ENGINE_ID, "homepage", "https://example.org") == 0);
    memset(meta_val, 0, sizeof(meta_val));
    nl_extension_get_metadata(SHOWCASE_ENGINE_ID, "homepage", meta_val, sizeof(meta_val));
    check("覆盖后读到新值", strcmp(meta_val, "https://example.org") == 0);

    check("不存在的 key 返回 -1",
          nl_extension_get_metadata(SHOWCASE_ENGINE_ID, "no_such_key", meta_val, sizeof(meta_val)) == -1);
    check("非法参数返回 -1", nl_extension_set_metadata(NULL, "k", "v") == -1);

    /* 清空语义：把 value 传 NULL，主库会把该 key 的值清空（保留 key） */
    check("清空元数据（值传 NULL）",
          nl_extension_set_metadata(SHOWCASE_ENGINE_ID, "homepage", NULL) == 0);
    meta_val[0] = 'X';
    meta_val[1] = '\0';
    int clear_rc = nl_extension_get_metadata(SHOWCASE_ENGINE_ID, "homepage", meta_val, sizeof(meta_val));
    check("清空后读取为空串", clear_rc == 0 && meta_val[0] == '\0');

    /* ==================================================
     * 7. 自动加载目录 (set/get_auto_load_dir / auto_load)
     * ================================================== */
    section("7. 自动加载目录 (set/get_auto_load_dir / auto_load)");
    nl_extension_set_auto_load_dir("extensions");
    const char* auto_dir = nl_extension_get_auto_load_dir();
    printf("    自动加载目录 = %s\n", auto_dir ? auto_dir : "(null)");
    check("get_auto_load_dir 与设置一致",
          auto_dir && strcmp(auto_dir, "extensions") == 0);

    int loaded = nl_extension_auto_load();
    printf("    从默认目录自动加载数量 = %d\n", loaded);
    check("auto_load 安全返回", loaded >= 0);

    loaded = nl_extension_auto_load_from_dir("./not_exist_dir");
    printf("    从不存在目录自动加载数量 = %d\n", loaded);
    check("auto_load_from_dir 对不存在目录返回 0", loaded == 0);

    /* ==================================================
     * 8. 访问与调用 (access / get_func / iterate / get_all)
     * ================================================== */
    section("8. 访问与调用 (access / get_func / iterate / get_all)");
    nl_extension_info_t* accessed = nl_extension_access(SHOWCASE_ENGINE_ID);
    check("access 返回有效指针", accessed == engine_info);
    check("access 未注册扩展返回 NULL", nl_extension_access("nope") == NULL);

    check("is_loaded(engine)", nl_extension_is_loaded(SHOWCASE_ENGINE_ID) == 1);
    check("is_available(codec)", nl_extension_is_available(SHOWCASE_CODEC_ID) == 1);
    check("is_loaded(未注册) == 0", nl_extension_is_loaded("nope") == 0);

    /* 通过扩展提供的函数解析器获取其元数据查询函数（不含业务负载） */
    typedef const char* (*version_fn)(void);
    typedef int (*available_fn)(void);

    version_fn vfn   = (version_fn)nl_extension_get_func(engine_info, "version");
    available_fn afn = (available_fn)nl_extension_get_func_by_id(SHOWCASE_ENGINE_ID, "is_available");
    void* unknown    = nl_extension_get_func(engine_info, "no_such_func");

    check("get_func(\"version\") 解析成功", vfn != NULL);
    check("get_func_by_id(\"is_available\") 解析成功", afn != NULL);
    check("get_func 未知函数返回 NULL", unknown == NULL);

    if (vfn) {
        printf("    engine.version() => %s\n", vfn());
        check("经函数指针获取版本一致", strcmp(vfn(), SHOWCASE_ENGINE_VERSION) == 0);
    }
    if (afn) {
        check("经函数指针获取可用性 == 1", afn() == 1);
    }

    /* 遍历所有扩展 */
    int iter_count = 0;
    int iter_rc = nl_extension_iterate(iterate_cb, &iter_count);
    printf("    iterate 返回 %d, 遍历计数 %d\n", iter_rc, iter_count);
    check("iterate 遍历全部扩展", iter_count == 3 && iter_rc == 3);

    /* 获取全部扩展快照（返回 malloc 数组，需 free） */
    int all_count = 0;
    nl_extension_info_t** all = nl_extension_get_all(&all_count);
    check("get_all 返回 3 个扩展", all != NULL && all_count == 3);
    if (all) free(all);

    /* ==================================================
     * 9. 依赖管理 (add / query / check / resolve)
     * ================================================== */
    section("9. 依赖管理 (add / query / check / resolve)");

    /* engine 依赖 codec（必需），并声明一个可选的、并不存在的扩展 */
    check("添加必需依赖 codec",
          nl_extension_add_dependency(engine_info, SHOWCASE_CODEC_ID,
                                      NL_EXT_DEP_REQUIRED, "1.0.0") == 0);
    check("添加可选依赖 showcase_legacy",
          nl_extension_add_dependency(engine_info, "showcase_legacy",
                                      NL_EXT_DEP_OPTIONAL, NULL) == 0);

    int dep_count = nl_extension_get_dependency_count(engine_info);
    printf("    依赖总数 = %d (required=%d, optional=%d)\n",
           dep_count, engine_info->required_dep_count, engine_info->optional_dep_count);
    check("依赖总数 == 2", dep_count == 2);
    check("必需依赖计数 == 1", engine_info->required_dep_count == 1);
    check("可选依赖计数 == 1", engine_info->optional_dep_count == 1);

    nl_extension_dependency_t* dep = nl_extension_get_dependency_by_id(engine_info, SHOWCASE_CODEC_ID);
    check("get_dependency_by_id 找到 codec", dep != NULL);
    check("get_dependencies 返回链表头", nl_extension_get_dependencies(engine_info) != NULL);
    check("has_dependency(codec) == 1", nl_extension_has_dependency(engine_info, SHOWCASE_CODEC_ID) == 1);
    check("has_dependency(legacy) == 1", nl_extension_has_dependency(engine_info, "showcase_legacy") == 1);

    nl_extension_dependency_t* deps[8];
    int req_n = nl_extension_get_required_deps(engine_info, deps, 8);
    printf("    get_required_deps => %d\n", req_n);
    check("必需依赖列表数量 == 1", req_n == 1);
    int opt_n = nl_extension_get_optional_deps(engine_info, deps, 8);
    printf("    get_optional_deps => %d\n", opt_n);
    check("可选依赖列表数量 == 1", opt_n == 1);

    check("check_dependencies(require_all=1) 通过",
          nl_extension_check_dependencies(engine_info, 1) == 0);
    check("are_dependencies_met == 1", nl_extension_are_dependencies_met(engine_info) == 1);
    check("resolve_dependencies 成功", nl_extension_resolve_dependencies(engine_info) == 0);

    /* 移除可选依赖后再查询 */
    check("移除可选依赖 showcase_legacy",
          nl_extension_remove_dependency(engine_info, "showcase_legacy") == 0);
    printf("    移除后依赖总数 = %d\n", nl_extension_get_dependency_count(engine_info));
    check("移除后依赖总数 == 1", nl_extension_get_dependency_count(engine_info) == 1);
    check("被移除依赖 has_dependency == 0",
          nl_extension_has_dependency(engine_info, "showcase_legacy") == 0);

    /* 清空全部依赖 */
    check("clear_dependencies 成功", nl_extension_clear_dependencies(engine_info) == 0);
    check("清空后依赖总数为 0", nl_extension_get_dependency_count(engine_info) == 0);

    /* ==================================================
     * 10. 扩展级懒加载 (nl_extension_lazy_*)
     * ================================================== */
    section("10. 扩展级懒加载 (nl_extension_lazy_load / unload / get_status / is_loaded)");

    check("codec 绑定了 lazy_load 回调", codec_info->lazy_load != NULL);
    check("codec 绑定了 lazy_unload 回调", codec_info->lazy_unload != NULL);
    check("codec 具备 NL_CAP_LAZY_LOAD 能力",
          NL_CAP_HAS(codec_info->capabilities, NL_CAP_LAZY_LOAD) != 0);
    printf("    codec lazy_status(初始) = %d (UNLOADED=%d)\n",
           (int)nl_extension_lazy_get_status(SHOWCASE_CODEC_ID), (int)NL_MODULE_LAZY_UNLOADED);

    check("初始未加载 lazy_is_loaded == 0", nl_extension_lazy_is_loaded(SHOWCASE_CODEC_ID) == 0);
    check("初始状态 UNLOADED",
          nl_extension_lazy_get_status(SHOWCASE_CODEC_ID) == NL_MODULE_LAZY_UNLOADED);
    check("未注册扩展状态 UNLOADED",
          nl_extension_lazy_get_status("not_registered") == NL_MODULE_LAZY_UNLOADED);

    check("lazy_load(codec) 成功", nl_extension_lazy_load(SHOWCASE_CODEC_ID) == 0);
    check("加载后状态 LOADED",
          nl_extension_lazy_get_status(SHOWCASE_CODEC_ID) == NL_MODULE_LAZY_LOADED);
    check("加载后 lazy_is_loaded == 1", nl_extension_lazy_is_loaded(SHOWCASE_CODEC_ID) == 1);
    check("重复 lazy_load 幂等", nl_extension_lazy_load(SHOWCASE_CODEC_ID) == 0);

    check("lazy_unload(codec) 成功", nl_extension_lazy_unload(SHOWCASE_CODEC_ID) == 0);
    check("卸载后状态 STOPPED",
          nl_extension_lazy_get_status(SHOWCASE_CODEC_ID) == NL_MODULE_LAZY_STOPPED);
    check("卸载后 lazy_is_loaded == 0", nl_extension_lazy_is_loaded(SHOWCASE_CODEC_ID) == 0);
    check("重复 lazy_unload 返回 -1", nl_extension_lazy_unload(SHOWCASE_CODEC_ID) == -1);

    check("卸载后可再次 lazy_load", nl_extension_lazy_load(SHOWCASE_CODEC_ID) == 0);
    check("再次加载后状态 LOADED",
          nl_extension_lazy_get_status(SHOWCASE_CODEC_ID) == NL_MODULE_LAZY_LOADED);

    /* engine 未定义懒加载回调，相关接口应安全失败 */
    check("engine lazy_load 返回 -1（无回调）", nl_extension_lazy_load(SHOWCASE_ENGINE_ID) == -1);
    check("engine lazy_unload 返回 -1（无回调）", nl_extension_lazy_unload(SHOWCASE_ENGINE_ID) == -1);

    /* ==================================================
     * 11. 模块级懒加载 (nl_module_lazy_*)
     * ================================================== */
    section("11. 模块级懒加载 (nl_module_lazy_*)");
    nl_module_info_t* demo_mod = NL_MODULE_GET_INFO(showcase_module);
    check("注册演示模块", nl_module_register(demo_mod) == 0);
    printf("    模块数量 = %d (含 core + 演示模块)\n", nl_module_get_count());

    nl_module_lazy_enable(1);
    check("模块懒加载已全局启用", 1);
    nl_module_lazy_enable_module(NL_MODULE_CUSTOM);
    check("模块支持懒加载 nl_module_lazy_is_enabled",
          nl_module_lazy_is_enabled(NL_MODULE_CUSTOM) == 1);
    check("nl_module_lazy_load 成功", nl_module_lazy_load(NL_MODULE_CUSTOM) == 0);
    printf("    模块 lazy_status = %d (LOADED=%d)\n",
           (int)nl_module_lazy_get_status(NL_MODULE_CUSTOM), (int)NL_MODULE_LAZY_LOADED);
    check("nl_module_lazy_is_loaded == 1", nl_module_lazy_is_loaded(NL_MODULE_CUSTOM) == 1);
    check("nl_module_lazy_unload 成功", nl_module_lazy_unload(NL_MODULE_CUSTOM) == 0);
    check("卸载后 nl_module_lazy_is_loaded == 0", nl_module_lazy_is_loaded(NL_MODULE_CUSTOM) == 0);

    nl_module_lazy_preload_all();
    nl_module_lazy_clear_cache();
    nl_module_lazy_unload_all();
    nl_module_lazy_disable_module(NL_MODULE_CUSTOM);
    check("模块懒加载批量接口可安全调用", 1);

    /* ==================================================
     * 12. 模块查询与依赖 (nl_module_* / nl_get_module*)
     * ================================================== */
    section("12. 模块查询与依赖 (nl_module_* / nl_get_module*)");
    check("nl_module_get_info(CORE) 非空", nl_module_get_info(NL_MODULE_CORE) != NULL);
    check("nl_get_module(CUSTOM) == 演示模块", nl_get_module(NL_MODULE_CUSTOM) == demo_mod);
    check("nl_module_get_info_by_name(\"netleaf\") 非空",
          nl_module_get_info_by_name("netleaf") != NULL);
    check("nl_module_available(\"netleaf\") == 1", nl_module_available("netleaf") == 1);

    nl_module_info_t* mods[16];
    int mod_n = nl_module_get_all(mods, 16);
    printf("    nl_module_get_all => %d\n", mod_n);
    check("模块列表数量 >= 2", mod_n >= 2);

    printf("    演示模块 name=%s version=%s\n",
           nl_module_get_name(NL_MODULE_CUSTOM), nl_module_get_version(NL_MODULE_CUSTOM));
    printf("    演示模块 description=%s\n", nl_module_get_description(NL_MODULE_CUSTOM));
    check("nl_module_has_capability(CUSTOM, THREAD_SAFE)",
          nl_module_has_capability(NL_MODULE_CUSTOM, NL_CAP_THREAD_SAFE) == 1);
    printf("    演示模块 capabilities=0x%x\n", nl_module_get_capabilities(NL_MODULE_CUSTOM));
    check("nl_module_is_platform_supported(CUSTOM)",
          nl_module_is_platform_supported(NL_MODULE_CUSTOM) == 1);

    /* 模块依赖：让演示模块依赖核心模块 */
    check("添加模块依赖(CUSTOM -> CORE)",
          nl_module_add_dependency(NL_MODULE_CUSTOM, NL_MODULE_CORE) == 0);
    check("模块依赖检查通过", nl_module_check_dependencies(NL_MODULE_CUSTOM) == 0);
    check("获取模块依赖 == core",
          nl_module_get_dependencies(NL_MODULE_CUSTOM) == nl_module_get_info(NL_MODULE_CORE));
    nl_module_remove_dependency(NL_MODULE_CUSTOM, NL_MODULE_CORE);
    check("移除模块依赖不影响后续调用", 1);

    /* 启用/禁用模块 */
    check("nl_module_set_enabled(CUSTOM, 0)", nl_module_set_enabled(NL_MODULE_CUSTOM, 0) == 0);
    check("禁用后状态为 DISABLED",
          nl_module_get_status(NL_MODULE_CUSTOM) == NL_MODULE_STATUS_DISABLED);
    check("nl_module_set_enabled(CUSTOM, 1)", nl_module_set_enabled(NL_MODULE_CUSTOM, 1) == 0);

    /* 名称列表接口 */
    const char* names[16];
    int name_n = nl_get_modules(names, 16);
    printf("    nl_get_modules => %d\n", name_n);
    check("名称列表数量 >= 2", name_n >= 2);

    /* 注销演示模块，验证 nl_module_unregister */
    int before_unreg = nl_module_get_count();
    check("nl_module_unregister 成功", nl_module_unregister(NL_MODULE_CUSTOM) == 0);
    check("注销后模块数量减 1", nl_module_get_count() == before_unreg - 1);
    check("注销后 nl_get_module(CUSTOM) == NULL", nl_get_module(NL_MODULE_CUSTOM) == NULL);

    /* ==================================================
     * 13. 热重载 (reload / reload_all)
     * ================================================== */
    section("13. 热重载 (reload / reload_all)");
    check("reload(engine) 成功", nl_extension_reload(SHOWCASE_ENGINE_ID) == 0);
    check("reload 后 engine 仍可用", nl_extension_is_available(SHOWCASE_ENGINE_ID) == 1);
    int reloaded = nl_extension_reload_all();
    printf("    reload_all 重载数量 = %d\n", reloaded);
    check("reload_all 重载全部扩展", reloaded == 3);

    /* 引擎在 init 时自绑定了解析器，reload 后应仍然有效 */
    version_fn proc2 = (version_fn)nl_extension_get_func_by_id(SHOWCASE_ENGINE_ID, "version");
    check("reload 后 get_func 仍可用", proc2 != NULL);

    /* ==================================================
     * 14. 批量与单例生命周期 (init_all / shutdown_all / shutdown)
     * ================================================== */
    section("14. 批量与单例生命周期 (init_all / shutdown_all / shutdown)");
    int inited = nl_extension_init_all();
    printf("    init_all 初始化数量 = %d\n", inited);
    check("init_all 初始化 3 个扩展", inited == 3);

    check("单例 shutdown(metrics)", nl_extension_shutdown(SHOWCASE_METRICS_ID) == 0);
    check("单例 shutdown 后仍注册（is_loaded == 1）",
          nl_extension_is_loaded(SHOWCASE_METRICS_ID) == 1);

    int shut = nl_extension_shutdown_all();
    printf("    shutdown_all 关闭数量 = %d\n", shut);
    check("shutdown_all 关闭 3 个扩展", shut == 3);

    /* ==================================================
     * 15. 注销 (unregister / force_shutdown / force_shutdown_all)
     * ================================================== */
    section("15. 注销 (unregister / force_shutdown / force_shutdown_all)");
    check("注销 metrics 扩展", nl_extension_unregister(SHOWCASE_METRICS_ID) == 0);
    printf("    注销后扩展数量 = %d\n", nl_extension_get_count());
    check("注销后数量为 2", nl_extension_get_count() == 2);
    check("注销后 is_loaded(metrics) == 0", nl_extension_is_loaded(SHOWCASE_METRICS_ID) == 0);
    check("重复注销返回 -1", nl_extension_unregister(SHOWCASE_METRICS_ID) == -1);

    /* force_shutdown = 关闭并注销 */
    check("force_shutdown(engine)", nl_extension_force_shutdown(SHOWCASE_ENGINE_ID) == 0);
    printf("    force_shutdown 后扩展数量 = %d\n", nl_extension_get_count());
    check("force_shutdown 后数量为 1", nl_extension_get_count() == 1);

    int forced = nl_extension_force_shutdown_all();
    printf("    force_shutdown_all 清理数量 = %d\n", forced);
    check("force_shutdown_all 清空全部扩展", nl_extension_get_count() == 0);

    /* ==================================================
     * 16. Lang 错误码注册与多语言查询
     * ================================================== */
    section("16. Lang 错误码注册 (NL_ERROR_BEGIN/END + nl_lang_register_errors)");

    /* 注册 lang 扩展自身，演示真实扩展的 get_extension_info / version */
    nl_extension_info_t* lang_info = nl_lang_get_extension_info();
    check("nl_lang_get_extension_info 非空", lang_info != NULL);
    check("注册 lang 扩展", nl_extension_register(lang_info) == 0);
    check("lang 扩展 is_available == 1", nl_extension_is_available("lang") == 1);
    {
        char lang_ver[32];
        check("lang 扩展版本一致",
              nl_extension_get_version_by_id("lang", lang_ver, sizeof(lang_ver)) == 0 &&
              strcmp(lang_ver, nl_lang_version()) == 0);
    }

    /* 表驱动注册：一次注册 en_us + zh_cn 两份消息 */
    check("nl_lang_register_errors 成功",
          nl_lang_register_errors(SHOWCASE_LIB_ID, showcase_error_table) == 0);
    check("库已注册 nl_lang_is_registered",
          nl_lang_is_registered(SHOWCASE_LIB_ID) == 1);
    check("库包含 zh_cn 语言",
          nl_lang_has_language(SHOWCASE_LIB_ID, "zh_cn") == 1);
    check("错误码 -1 已注册", nl_lang_has_error(SHOWCASE_LIB_ID, -1) == 1);

    {
        int code_count = 0;
        int* codes = nl_lang_get_error_codes(SHOWCASE_LIB_ID, &code_count);
        printf("    nl_lang_get_error_codes => %d\n", code_count);
        check("错误码数量 == 4", codes != NULL && code_count == 4);
        if (codes) free(codes);
    }

    /* 注册库名，便于日志展示 */
    nl_lang_register_lib_name(SHOWCASE_LIB_ID, "showcase");
    check("nl_lang_get_lib_name 正确",
          strcmp(nl_lang_get_lib_name(SHOWCASE_LIB_ID), "showcase") == 0);

    /* 多语言查询：默认语言为 en_us */
    check("切换到 en_us", nl_lang_set("en_us") == 0);
    const char* en_msg = nl_lang_get_error(SHOWCASE_LIB_ID, -1);
    printf("    [en_us] %s\n", en_msg ? en_msg : "(null)");
    check("nl_lang_get_error(en_us) 正确",
          en_msg && strcmp(en_msg, "Showcase operation failed") == 0);

    check("切换到 zh_cn", nl_lang_set("zh_cn") == 0);
    const char* zh_msg = nl_lang_get_error(SHOWCASE_LIB_ID, -1);
    printf("    [zh_cn] %s\n", zh_msg ? zh_msg : "(null)");
    check("nl_lang_get_error(zh_cn) 正确",
          zh_msg && strcmp(zh_msg, "示例操作失败") == 0);

    /* 指定语言查询（不受当前语言影响） */
    const char* en2 = nl_lang_get_error_for(SHOWCASE_LIB_ID, -1, "en_us");
    check("nl_lang_get_error_for(en_us) 正确",
          en2 && strcmp(en2, "Showcase operation failed") == 0);

    /* 工具函数 */
    check("nl_error_is_success(0) == 1", nl_error_is_success(0) == 1);
    check("错误分类 -1 => Parameter",
          strcmp(nl_lang_get_error_category(-1), "Parameter") == 0);

    /* 非法语言码（缺少下划线）应被拒绝 */
    check("非法语言码被拒绝", nl_lang_set("en-US") == -1);

    /* ==================================================
     * 17. Lang 变量系统（静态 / 类型化 / 动态 provider / 外部 env）
     * ================================================== */
    section("17. Lang 变量系统 (nl_lang_var_*)");

    check("var_set 字符串", nl_lang_var_set("showcase_project", "NetLeaf") == 0);
    check("var_get 字符串", strcmp(nl_lang_var_get("showcase_project"), "NetLeaf") == 0);
    check("var_exists == 1", nl_lang_var_exists("showcase_project") == 1);

    check("var_set_int", nl_lang_var_set_int("max_conn", 1024) == 0);
    check("var_get_int", nl_lang_var_get_int("max_conn", 0) == 1024);
    check("var_set_float", nl_lang_var_set_float("ratio", 0.75) == 0);
    {
        double d = nl_lang_var_get_float("ratio", 0.0);
        check("var_get_float", d > 0.74 && d < 0.76);
    }
    check("var_set_bool", nl_lang_var_set_bool("verbose", 1) == 0);
    check("var_get_bool == 1", nl_lang_var_get_bool("verbose", 0) == 1);

    /* 缺失变量的默认值语义 */
    check("缺失变量 get_int 返回默认值",
          nl_lang_var_get_int("no_such_var", 42) == 42);
    check("缺失变量 exists == 0", nl_lang_var_exists("no_such_var") == 0);

    /* 普通变量替换 {{NAME}} */
    {
        char rbuf[128];
        const char* rr = nl_lang_var_replace("Hello {{showcase_project}}!", rbuf, sizeof(rbuf));
        printf("    replace => \"%s\"\n", rbuf);
        check("nl_lang_var_replace 正确",
              rr == rbuf && strcmp(rbuf, "Hello NetLeaf!") == 0);
    }

    /* HTML <var> 标签替换 {{<var>NAME</var>}} */
    {
        char hbuf[128];
        const char* hr = nl_lang_var_replace_html("port={{<var>max_conn</var>}}", hbuf, sizeof(hbuf));
        printf("    replace_html => \"%s\"\n", hbuf);
        check("nl_lang_var_replace_html 正确",
              hr == hbuf && strcmp(hbuf, "port=1024") == 0);
    }

    /* 条件表达式求值（基于变量值） */
    check("condition_eval(max_conn > 100) == 1",
          nl_lang_var_condition_eval("max_conn > 100") == 1);
    check("condition_eval(max_conn < 100) == 0",
          nl_lang_var_condition_eval("max_conn < 100") == 0);

    /* 动态变量：provider 每次访问都会重新计算 */
    check("var_set_provider 成功",
          nl_lang_var_set_provider("showcase_now", showcase_dynamic_provider, NULL) == 0);
    check("nl_lang_var_is_dynamic == 1",
          nl_lang_var_is_dynamic("showcase_now") == 1);
    {
        char first[64];
        char second[64];
        snprintf(first, sizeof(first), "%s", nl_lang_var_get("showcase_now"));
        snprintf(second, sizeof(second), "%s", nl_lang_var_get("showcase_now"));
        printf("    provider 两次取值: %s / %s\n", first, second);
        check("provider 每次访问重新求值",
              strncmp(first, "dynamic-", 8) == 0 && strcmp(first, second) != 0);
    }

    /* 外部变量：绑定到进程环境变量，每次访问读取最新值 */
    showcase_set_env("NL_SHOWCASE_TOKEN", "env-value-1");
    check("var_bind_env 成功",
          nl_lang_var_bind_env("showcase_token", "NL_SHOWCASE_TOKEN") == 0);
    check("绑定的变量为动态变量", nl_lang_var_is_dynamic("showcase_token") == 1);
    {
        char v1[64];
        snprintf(v1, sizeof(v1), "%s", nl_lang_var_get("showcase_token"));
        showcase_set_env("NL_SHOWCASE_TOKEN", "env-value-2");
        char v2[64];
        snprintf(v2, sizeof(v2), "%s", nl_lang_var_get("showcase_token"));
        printf("    env 绑定两次取值: %s / %s\n", v1, v2);
        check("读取环境变量初值", strcmp(v1, "env-value-1") == 0);
        check("环境变量变化后动态生效", strcmp(v2, "env-value-2") == 0);
    }

    /* 批量导入环境变量（按前缀过滤） */
    showcase_set_env("NL_SHOWCASE_DEMO_A", "alpha");
    showcase_set_env("NL_SHOWCASE_DEMO_B", "beta");
    {
        int imported = nl_lang_var_load_env("NL_SHOWCASE_DEMO_");
        printf("    load_env 导入变量数 = %d\n", imported);
        check("load_env 导入 >= 2", imported >= 2);
        check("导入的变量存在", nl_lang_var_exists("NL_SHOWCASE_DEMO_A") == 1);
        check("导入的变量值正确",
              strcmp(nl_lang_var_get("NL_SHOWCASE_DEMO_A"), "alpha") == 0);
    }

    /* 带变量的错误消息：消息模板中的 {{<var>max_conn</var>}} 会被替换 */
    {
        char evbuf[256];
        nl_lang_set("en_us");
        const char* er = nl_lang_get_error_with_vars(SHOWCASE_LIB_ID, -3, evbuf, sizeof(evbuf));
        printf("    get_error_with_vars => \"%s\"\n", evbuf);
        check("get_error_with_vars 替换变量",
              er == evbuf && strstr(evbuf, "1024") != NULL);
    }

    /* 清理变量与错误码演示状态 */
    check("var_remove 成功", nl_lang_var_remove("showcase_token") == 0);
    nl_lang_var_clear_all();
    check("clear_all 后变量不存在", nl_lang_var_exists("showcase_project") == 0);
    showcase_unset_env("NL_SHOWCASE_TOKEN");
    showcase_unset_env("NL_SHOWCASE_DEMO_A");
    showcase_unset_env("NL_SHOWCASE_DEMO_B");
    nl_lang_set("en_us");

    /* ==================================================
     * 18. 汇总与清理
     * ================================================== */
    section("18. 汇总与清理");
    nl_print_modules();
    nl_extension_force_shutdown_all();
    check("清理后扩展数量为 0", nl_extension_get_count() == 0);
    nl_modules_shutdown();
    printf("    主库已关闭，模块数量 = %d\n", nl_get_module_count());

    printf("\n==================================================\n");
    printf("  示例结束: PASS=%d FAIL=%d\n", g_pass, g_fail);
    printf("==================================================\n\n");

    return g_fail == 0 ? 0 : 1;
}
