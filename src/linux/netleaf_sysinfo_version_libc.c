/*
 * C 运行库版本串：glibc / 其他 Linux C 库实现。
 *
 * 从原 netleaf_linux.c 的 __GLIBC__ 分支抽出，供 Linux（非 Android）构建编译。
 */
#ifdef _WIN32
#error "This file should only be compiled on Linux"
#endif

#include <stdio.h>

#include "common/netleaf_sysinfo_version.h"

void nl_sysinfo_format_runtime(char* buf, size_t n) {
#if defined(__GLIBC__)
    snprintf(buf, n, "glibc %d.%d", __GLIBC__, __GLIBC_MINOR__);
#else
    snprintf(buf, n, "unknown C library");
#endif
}
