#ifndef NETLEAF_UTIL_H
#define NETLEAF_UTIL_H

/**
 * @file nl_util.h
 * @brief NetLeaf 平台无关公共工具层 (Common Platform-Independent Utilities)
 *
 * 本模块将散落在各扩展与平台实现中、语义完全相同且与操作系统无关
 * （不依赖 socket / winsock / pthread / MSVCRT 特有 API）的工具函数
 * 统一收敛到核心库，供整个 NetLeaf 共享，避免多份重复拷贝：
 *
 *  - 字符串：nl_strdup / nl_parse_enable_value / nl_strncasecmp /
 *            nl_strncasestr / nl_tolower / nl_string_tolower
 *  - 安全：  nl_sha1（纯 C 实现）/ nl_base64_encode_into /
 *            nl_base64_encode / nl_base64_decode
 *  - 初始化：nl_once_init / nl_once_run（线程安全的一次性守卫）
 *
 * 线程安全一次性守卫的设计要点：
 *  - nl_once_init 使用"双检 + 平台锁"保证同一槽位在并发下仅被初始化一次，
 *    且已初始化的调用方不会进入临界区（热路径零开销）。
 *  - nl_once_run 用于"只运行一次"的任意任务（如缓存填充、注册表构建）。
 *  - 槽位容量为 8，如需更多请使用 nl_once_init_ex 自行扩展。
 */

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
    #ifdef NL_EXPORTS
        #define NL_API __declspec(dllexport)
    #else
        #define NL_API __declspec(dllimport)
    #endif
#else
    #define NL_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 字符串工具
 * ============================================================ */

/**
 * @brief 平台安全的 strdup（Windows 下映射到 _strdup，POSIX 直接用 strdup）。
 * @return 成功返回堆指针（调用方负责 free()），失败返回 NULL。
 */
NL_API char* nl_strdup(const char* s);

/**
 * @brief 复制字符串前 n 个字符（NUL 结尾）的平台安全版本。
 *
 * Windows CRT 无标准 strndup，glibc 的 strndup 需 _GNU_SOURCE，
 * 本实现统一覆盖，便于各扩展共用。n 会先截断到 s 的实际长度。
 * @return 成功返回堆指针（调用方负责 free()），失败返回 NULL。
 */
NL_API char* nl_strndup(const char* s, size_t n);

/**
 * @brief 解析布尔/使能字符串（大小写无关）。
 *
 * 支持：
 *  - "1" / "0"          → 1 / 0
 *  - "true" / "on" / "yes"  → 1
 *  - "false" / "off" / "no" → 0
 *  - NULL 或无法识别 → 0
 *
 * 该函数替代各扩展中重复出现的静态 parse_enable_value 拷贝。
 */
NL_API int nl_parse_enable_value(const char* value);

/**
 * @brief 限定前 n 个字符的、大小写无关的字符串比较。
 *
 * 标准语义：当 n > 0 且任一方在 n 个字符内提前遇到 '\0' 时，
 * 返回剩余字符（小写化）之差；两串同时到达结束符视为相等返回 0。
 */
NL_API int nl_strncasecmp(const char* s1, const char* s2, size_t n);

/**
 * @brief 在 haystack（长度 len，可能不含 '\0' 结尾）中查找大小写无关的 needle。
 *
 * @return 命中返回指向命中起点的指针（非 const，便于上层继续处理），
 *         未命中 / 参数非法返回 NULL。
 */
NL_API char* nl_strncasestr(const char* haystack, const char* needle, size_t len);

/**
 * @brief 单字符 ASCII 转小写（仅对 'A'..'Z' 生效，其它字符原样返回）。
 *
 * 用于需要纯 ASCII 语义（不依赖 locale）的场景，与 <ctype.h> 的 tolower
 * 在 [0, 127] 区间等价，但不受 locale 影响。
 */
NL_API int nl_tolower(int c);

/**
 * @brief 将 ASCII 字符串原地转为小写（只改 'A'..'Z'，其它字符不动）。
 *
 * 返回传入的字符串指针，便于链式使用。
 */
NL_API char* nl_string_tolower(char* s);

/* ============================================================
 * 安全：SHA-1 / Base64
 * ============================================================ */

/**
 * @brief 纯 C 实现的 SHA-1（20 字节摘要）。
 *
 * 输入为 input[0, len)，输出 20 字节写至 output（调用方保证至少 20 字节）。
 * 该函数被 WebSocket 握手（Sec-WebSocket-Accept 计算）使用。
 */
NL_API void nl_sha1(const char* input, size_t len, unsigned char* output);

/**
 * @brief 标准 Base64 编码（带 '=' 填充，结尾 '\0'），写入调用方提供的缓冲区。
 *
 * 输出缓冲至少需要 4*((len+2)/3)+1 字节。该接口不分配内存，
 * 供 WebSocket 握手等"输出长度已知"的场景使用。
 */
NL_API void nl_base64_encode_into(const char* input, size_t len, char* output);

/**
 * @brief 标准 Base64 编码（带 '=' 填充，结尾 '\0'），返回堆分配的字符串。
 *
 * 调用方负责 free() 返回值。这是 netleaf.h 对外 API 的实现，
 * 内部复用 nl_base64_encode_into。
 */
NL_API char* nl_base64_encode(const char* input, size_t len);

/**
 * @brief 标准 Base64 解码。
 *
 * 返回堆分配、'\0' 结尾的解码缓冲（out_len 输出解码后长度），
 * 调用方负责 free()。参数非法或输入含非法字符时返回 NULL。
 */
NL_API char* nl_base64_decode(const char* input, size_t* out_len);

/* ============================================================
 * 线程安全的一次性初始化守卫
 * ============================================================ */

/**
 * @brief 一次性初始化槽位。
 *
 * 同一 index 在并发下只会被初始化一次；已就绪时立即返回 1，
 * 不会进入临界区。
 *
 * @param init_fn 实际执行初始化的回调，返回值被忽略。
 * @return 始终返回 1（表示"已完成初始化"，调用方可继续业务）。
 */
NL_API int nl_once_init(int index, void (*init_fn)(void));

/**
 * @brief 通用的一次性任务执行：保证 work_fn 只被执行一次。
 *
 * 与 nl_once_init 的差异在于：
 *  - nl_once_init 语义偏向"惰性初始化某资源"（init_fn 通常设置全局指针）；
 *  - nl_once_run 语义偏向"只跑一遍的副作用任务"（如缓存填充、注册表构建）。
 * 二者底层使用同一套互斥槽位，调用方按语义选择即可。
 */
NL_API int nl_once_run(int index, void (*work_fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* NETLEAF_UTIL_H */
