/*
 * nl_uri.h - RFC 3986 URI 端点处理 + 物理文件隔离（平台无关，一份实现）
 *
 * 背景（第192轮）：三平台此前各自把请求路径直接拼到根目录上再做 realpath
 * 校验（先拼后查），且不剥 query、不 percent-decode、不做 `.`/`..` 段规范化：
 *   - `/a?x=1` 的 query 被拼进文件名（永远 404）；
 *   - `%2e%2e%2f` 等编码变体无统一语义；
 *   - 路由查找用原始 path 精确匹配，带 query 的请求必然 404。
 *
 * 本模块提供：
 *   1) nl_url_encode / nl_url_decode —— 补全 include/netleaf.h 中
 *      声明但从未实现的公共 API（严格 RFC 3986，非法编码返回 NULL）；
 *   2) nl_uri_normalize —— 请求端点规范化（剥 authority/query/fragment →
 *      percent-decode → remove_dot_segments，先解码后归一，使
 *      `%2e%2e%2f` 与 `..` 同义）；
 *   3) nl_uri_resolve_file —— 物理文件隔离主入口：任何"请求路径 → 磁盘
 *      文件"的拼接都必须经过它，结果被约束在 root 之内（含符号链接 /
 *      junction 解析）；
 *   4) nl_uri_is_under —— 拼接后二次校验（如目录 index.html 追加）。
 *
 * 返回码（负值为错误）：
 *   NL_URI_EINVAL  非法端点（坏 % 编码 / %00 / 控制字符 / '\' / 盘符 ':'）
 *   NL_URI_ESCAPES 越出根目录（越根 `..` 或 canonical 结果不在 root 下）
 *   NL_URI_ENOSPC  输出缓冲 / 段数超限
 *   NL_URI_EROOT   根目录不可解析（不存在 / 无权限 / 符号链接环）
 * 调用方建议映射：EINVAL/ENOSPC → 400，ESCAPES/EROOT → 403。
 */
#ifndef NL_URI_H
#define NL_URI_H

#include <stddef.h>

#ifndef NL_API
    #ifdef _WIN32
        #ifdef NL_EXPORTS
            #define NL_API __declspec(dllexport)
        #else
            #define NL_API __declspec(dllimport)
        #endif
    #else
        #define NL_API
    #endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NL_URI_OK       0
#define NL_URI_EINVAL  (-1)
#define NL_URI_ESCAPES (-2)
#define NL_URI_ENOSPC  (-3)
#define NL_URI_EROOT   (-4)

/* 规范化路径的工作上限（含 NUL） */
#define NL_URI_MAX 8192

/**
 * @brief URL percent-decode（严格 RFC 3986）。
 * '%' 后必须跟两个十六进制位；解码结果含 NUL / 控制字符视为非法。
 * '+' 保持字面量（不作 form-urlencoded 空格）。
 * @return 成功返回 NUL 结尾堆指针（调用方 free()），失败返回 NULL。
 */
NL_API char* nl_url_decode(const char* input);

/**
 * @brief URL percent-encode（仅保留 unreserved：A-Za-z0-9-._~）。
 * @return 成功返回 NUL 结尾堆指针（调用方 free()），失败返回 NULL。
 */
NL_API char* nl_url_encode(const char* input);

/**
 * @brief 请求端点规范化（RFC 3986 §5.2.4）。
 * 处理顺序：
 *   1) 绝对形式 `scheme://authority/path` 取 path（RFC 7230 §5.3.2）；
 *   2) 剥离 `?query` 与 `#fragment`；
 *   3) percent-decode（先解码后归一：`%2e%2e%2f` ≡ `..`）；
 *   4) remove_dot_segments + 折叠重复 '/'；越根 `..` → NL_URI_ESCAPES；
 *   5) 拒绝 '\'（RFC 3986 不含该字节）与控制字符；
 *      Windows 额外拒绝 ':'（盘符 / NTFS ADS）。
 * 输入必须以 '/' 开头（`OPTIONS *` 等特例由调用方先行处理）。
 * @return NL_URI_OK / NL_URI_*。out 不以 out 为入参别名（可与 uri 重叠）。
 */
NL_API int nl_uri_normalize(const char* uri, char* out, size_t cap);

/**
 * @brief 物理文件隔离：把 URL 路径安全解析到 root 目录之下。
 * 保证：返回 0 时 *out 要么是 root 下真实存在的文件（已解析符号链接 /
 * junction 并校验前缀边界），要么是 root 下词法安全的不存在路径
 * （交由调用方回 404）。
 * @return NL_URI_OK / NL_URI_EINVAL（→400）/ NL_URI_ESCAPES、
 *         NL_URI_EROOT（→403）/ NL_URI_ENOSPC（→400）。
 */
NL_API int nl_uri_resolve_file(const char* root, const char* url_path,
                               char* out, size_t out_cap);

/**
 * @brief 判断绝对路径是否位于 root 之下（canonical + 前缀边界比较，
 * 下一字符必须是分隔符或结尾，避免 /base 误匹配 /baseXXX）。
 * @return 1 在内，0 在外或无法解析。
 */
NL_API int nl_uri_is_under(const char* root, const char* abs_path);

#ifdef __cplusplus
}
#endif

#endif /* NL_URI_H */
