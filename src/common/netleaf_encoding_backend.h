/*
 * 编码转码后端接口（平台钩子）。
 *
 * netleaf_encoding_linux.c 中原先内嵌 iconv 调用与 __ANDROID__ 分支，
 * 现把平台差异收敛到本接口，实现按平台由 CMake 选择编译：
 *   - src/linux/netleaf_encoding_iconv.c    glibc/musl：真实 iconv + nl_langinfo
 *   - src/android/netleaf_encoding_bionic.c Bionic（minSdk 21）：无 iconv 回退
 *
 * 约定：
 *   - open 失败返回 NULL；
 *   - convert 每次调用内部负责释放句柄资源（成功与失败路径皆关），
 *     成功返回 0 并输出堆缓冲（由调用方 free）；
 *   - default_codeset 可返回 NULL（表示"无系统编码信息"，调用方回退 UTF-8）。
 */
#ifndef NETLEAF_ENCODING_BACKEND_H
#define NETLEAF_ENCODING_BACKEND_H

#include <stddef.h>

void* nl_encoding_backend_open(const char* from, const char* to);

int nl_encoding_backend_convert(void* cd, const char* input, size_t input_len,
                                char** output, size_t* output_len);

const char* nl_encoding_backend_default_codeset(void);

#endif /* NETLEAF_ENCODING_BACKEND_H */
