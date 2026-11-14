/*
 * C 运行库版本串（平台钩子）。
 *
 * netleaf_sysinfo_linux.c 中原先内嵌 __ANDROID__/__GLIBC__ 分支，
 * 现拆到独立实现文件，由 CMake 按平台选择编译：
 *   - src/linux/netleaf_sysinfo_version_libc.c   glibc / 其他 Linux C 库
 *   - src/android/netleaf_sysinfo_version_bionic.c Android Bionic（minSdk 21）
 *
 * 实现方将 C 运行库版本写入 buf（保证 NUL 结尾），用于 nl_get_runtime_version()。
 */
#ifndef NETLEAF_SYSINFO_VERSION_H
#define NETLEAF_SYSINFO_VERSION_H

#include <stddef.h>

void nl_sysinfo_format_runtime(char* buf, size_t n);

#endif /* NETLEAF_SYSINFO_VERSION_H */
