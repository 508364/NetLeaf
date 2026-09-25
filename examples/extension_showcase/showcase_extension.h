#ifndef NETLEAF_SHOWCASE_EXTENSION_H
#define NETLEAF_SHOWCASE_EXTENSION_H

/**
 * @file showcase_extension.h
 * @brief NL 扩展系统示例 - 扩展库公共接口
 *
 * 本示例仅演示 include/netleaf_module.h 中 NL Extension 的系统级 API 用法，
 * 不承载任何业务/负载逻辑。这里定义三个扩展：
 *   1. showcase_codec    —— 被依赖的扩展（使用 NL_EXTENSION_DEFINE_LAZY 定义，演示懒加载）
 *   2. showcase_engine   —— 演示依赖声明、生命周期与扩展间函数解析
 *   3. showcase_metrics  —— 演示平台串 "all" 与独立卸载（使用 NL_EXTENSION_DEFINE 定义）
 *
 * 扩展库被编译为动态链接库，由宿主程序（extension_showcase_demo.c）注册并管理。
 * 所有 NL_EXT_API 函数都会被导出，既可以被宿主直接调用，
 * 也可以通过 nl_extension_get_func() / nl_extension_get_func_by_id() 间接调用。
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

#ifdef _WIN32
    #ifdef NL_EXTENSION_EXPORTS
        #define NL_EXT_API __declspec(dllexport)
    #else
        #define NL_EXT_API __declspec(dllimport)
    #endif
#else
    #define NL_EXT_API
#endif

// =========================================
// 扩展版本与标识
// =========================================

#define SHOWCASE_CODEC_VERSION   "1.2.0"
#define SHOWCASE_ENGINE_VERSION  "2.0.0"
#define SHOWCASE_METRICS_VERSION "1.0.0"

#define SHOWCASE_CODEC_ID    "showcase_codec"
#define SHOWCASE_ENGINE_ID   "showcase_engine"
#define SHOWCASE_METRICS_ID  "showcase_metrics"

// =========================================
// 扩展信息获取函数（宿主通过它拿到 nl_extension_info_t）
// =========================================

/**
 * @brief 获取编解码扩展的信息结构体指针
 * @return 指向静态 nl_extension_info_t 的指针（进程内唯一）
 */
NL_EXT_API nl_extension_info_t* nl_showcase_codec_get_extension_info(void);

/**
 * @brief 获取引擎扩展的信息结构体指针
 */
NL_EXT_API nl_extension_info_t* nl_showcase_engine_get_extension_info(void);

/**
 * @brief 获取指标扩展的信息结构体指针
 */
NL_EXT_API nl_extension_info_t* nl_showcase_metrics_get_extension_info(void);

// =========================================
// 编解码扩展（showcase_codec）
// =========================================

NL_EXT_API int nl_showcase_codec_init(void);
NL_EXT_API void nl_showcase_codec_shutdown(void);
NL_EXT_API int nl_showcase_codec_is_available(void);
NL_EXT_API const char* nl_showcase_codec_version(void);

/** 懒加载回调：返回扩展上下文句柄（NULL 表示加载失败）。 */
NL_EXT_API void* nl_showcase_codec_lazy_load(void);
/** 懒卸载回调：释放懒加载时申请的资源。 */
NL_EXT_API void nl_showcase_codec_lazy_unload(void);

// =========================================
// 引擎扩展（showcase_engine）
// =========================================

NL_EXT_API int nl_showcase_engine_init(void);
NL_EXT_API void nl_showcase_engine_shutdown(void);
NL_EXT_API int nl_showcase_engine_is_available(void);
NL_EXT_API const char* nl_showcase_engine_version(void);

/**
 * @brief 扩展间函数解析回调。
 *        该函数会被写入 ext->get_extension_function，
 *        从而支持 nl_extension_get_func() / nl_extension_get_func_by_id()。
 */
NL_EXT_API void* nl_showcase_engine_resolve(const char* library_id, const char* func_name);

// =========================================
// 指标扩展（showcase_metrics）
// =========================================

NL_EXT_API int nl_showcase_metrics_init(void);
NL_EXT_API void nl_showcase_metrics_shutdown(void);
NL_EXT_API int nl_showcase_metrics_is_available(void);
NL_EXT_API const char* nl_showcase_metrics_version(void);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_SHOWCASE_EXTENSION_H
