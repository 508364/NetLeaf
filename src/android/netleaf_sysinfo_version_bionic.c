/*
 * C 运行库版本串：Android Bionic 实现（minSdk 21）。
 *
 * 从原 netleaf_linux.c 的 __ANDROID__ 分支抽出，供 Android 构建编译。
 * Bionic 的版本宏是预处理器内部标识，不作为稳定版本号输出。
 */
#include <stdio.h>

#include "common/netleaf_sysinfo_version.h"

void nl_sysinfo_format_runtime(char* buf, size_t n) {
    snprintf(buf, n, "Bionic (NDK)");
}
