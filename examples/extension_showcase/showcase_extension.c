/**
 * @file showcase_extension.c
 * @brief NL 扩展系统示例 - 扩展库实现
 *
 * 本文件仅演示 NL Extension API 本身：扩展定义、生命周期回调、
 * 懒加载回调与扩展间函数解析。所有回调只维护最小状态，不承载任何业务/负载逻辑。
 *
 * 编译时需定义宏 NL_EXTENSION_EXPORTS，使 NL_EXT_API 展开为 dllexport。
 */

#include "showcase_extension.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// =========================================
// 扩展定义
//
// 说明：showcase_extension.h 中已声明了全部生命周期回调（NL_EXT_API），
//       因此这里的宏展开可以直接取它们的地址。
//
// NL_EXTENSION_DEFINE(ext_id, 显示名, 版本, 作者, 描述, 平台串, 能力标志,
//                     init, shutdown, is_available, get_version)
//
// NL_EXTENSION_DEFINE_LAZY(...) 额外提供 lazy_load / lazy_unload，
// 并在能力中自动补上 NL_CAP_LAZY_LOAD。
// =========================================

// —— 编解码扩展：支持懒加载，作为引擎扩展的被依赖项 ——
NL_EXTENSION_DEFINE_LAZY(
    showcase_codec,
    "Showcase Codec Extension",
    SHOWCASE_CODEC_VERSION,
    "NetLeaf Team",
    "示例扩展：演示懒加载与依赖被依赖关系",
    "Windows,Linux,MacOS",
    NL_CAP_THREAD_SAFE | NL_CAP_ASYNC,
    nl_showcase_codec_init,
    nl_showcase_codec_shutdown,
    nl_showcase_codec_is_available,
    nl_showcase_codec_version,
    nl_showcase_codec_lazy_load,
    nl_showcase_codec_lazy_unload
);

// —— 引擎扩展：演示依赖声明、生命周期与扩展间函数解析 ——
NL_EXTENSION_DEFINE(
    showcase_engine,
    "Showcase Engine Extension",
    SHOWCASE_ENGINE_VERSION,
    "NetLeaf Team",
    "示例扩展：演示依赖声明、生命周期与扩展间函数解析",
    "Windows,Linux,MacOS",
    NL_CAP_SERVER | NL_CAP_THREAD_SAFE,
    nl_showcase_engine_init,
    nl_showcase_engine_shutdown,
    nl_showcase_engine_is_available,
    nl_showcase_engine_version
);

// —— 指标扩展：平台串 "all"，可独立卸载 ——
NL_EXTENSION_DEFINE(
    showcase_metrics,
    "Showcase Metrics Extension",
    SHOWCASE_METRICS_VERSION,
    "NetLeaf Team",
    "示例扩展：演示平台串 all 与扩展状态/元数据查询",
    "all",
    NL_CAP_THREAD_SAFE,
    nl_showcase_metrics_init,
    nl_showcase_metrics_shutdown,
    nl_showcase_metrics_is_available,
    nl_showcase_metrics_version
);

// =========================================
// 扩展内部状态（仅用于演示生命周期/懒加载状态位）
// =========================================

static int g_codec_initialized = 0;
static int g_codec_loaded = 0;
static int g_codec_ctx = 0;

static int g_engine_initialized = 0;
static int g_metrics_initialized = 0;

// =========================================
// 编解码扩展实现
// =========================================

int nl_showcase_codec_init(void) {
    if (g_codec_initialized) return 0;
    g_codec_initialized = 1;
    printf("    [showcase_codec] init: 编解码扩展已初始化\n");
    return 0;
}

void nl_showcase_codec_shutdown(void) {
    if (!g_codec_initialized) return;
    g_codec_initialized = 0;
    printf("    [showcase_codec] shutdown: 编解码扩展已关闭\n");
}

int nl_showcase_codec_is_available(void) {
    return 1;
}

const char* nl_showcase_codec_version(void) {
    return SHOWCASE_CODEC_VERSION;
}

void* nl_showcase_codec_lazy_load(void) {
    g_codec_loaded = 1;
    printf("    [showcase_codec] lazy_load: 已按需加载编解码上下文\n");
    return &g_codec_ctx;
}

void nl_showcase_codec_lazy_unload(void) {
    g_codec_loaded = 0;
    printf("    [showcase_codec] lazy_unload: 已释放编解码上下文\n");
}

// =========================================
// 引擎扩展实现
// =========================================

int nl_showcase_engine_init(void) {
    if (g_engine_initialized) return 0;
    g_engine_initialized = 1;

    // 自绑定扩展间函数解析器，使 nl_extension_get_func() 可用。
    nl_extension_info_t* self = NL_EXTENSION_GET_INFO(showcase_engine);
    self->get_extension_function = nl_showcase_engine_resolve;

    printf("    [showcase_engine] init: 引擎扩展已初始化（已绑定函数解析器）\n");
    return 0;
}

void nl_showcase_engine_shutdown(void) {
    if (!g_engine_initialized) return;
    g_engine_initialized = 0;
    printf("    [showcase_engine] shutdown: 引擎扩展已关闭\n");
}

int nl_showcase_engine_is_available(void) {
    return 1;
}

const char* nl_showcase_engine_version(void) {
    return SHOWCASE_ENGINE_VERSION;
}

void* nl_showcase_engine_resolve(const char* library_id, const char* func_name) {
    (void)library_id;
    if (!func_name) return NULL;

    // 暴露扩展自身的元数据查询函数，演示 get_func 解析流程（无业务语义）。
    if (strcmp(func_name, "version") == 0) {
        return (void*)(size_t)&nl_showcase_engine_version;
    }
    if (strcmp(func_name, "is_available") == 0) {
        return (void*)(size_t)&nl_showcase_engine_is_available;
    }
    return NULL;
}

// =========================================
// 指标扩展实现
// =========================================

int nl_showcase_metrics_init(void) {
    if (g_metrics_initialized) return 0;
    g_metrics_initialized = 1;
    printf("    [showcase_metrics] init: 指标扩展已初始化\n");
    return 0;
}

void nl_showcase_metrics_shutdown(void) {
    if (!g_metrics_initialized) return;
    g_metrics_initialized = 0;
    printf("    [showcase_metrics] shutdown: 指标扩展已关闭\n");
}

int nl_showcase_metrics_is_available(void) {
    return 1;
}

const char* nl_showcase_metrics_version(void) {
    return SHOWCASE_METRICS_VERSION;
}

// =========================================
// 信息获取函数（宿主注册入口）
// =========================================

NL_EXT_API nl_extension_info_t* nl_showcase_codec_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(showcase_codec);
}

NL_EXT_API nl_extension_info_t* nl_showcase_engine_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(showcase_engine);
}

NL_EXT_API nl_extension_info_t* nl_showcase_metrics_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(showcase_metrics);
}
