#if defined(_WIN32)
#define NL_EXPORTS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>

#include "../../include/netleaf.h"

static int sys_info_loaded = 0;
static nl_ram_unit_t ram_unit = NL_RAM_UNIT_DECIMAL;

static char os_name[256] = "";
static char architecture[64] = "";
static char cpu_model[256] = "";
static int64_t total_ram = 0;
static char runtime_version[64] = "";

static void load_sys_info(void) {
    if (sys_info_loaded) return;
    
    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf(os_name, sizeof(os_name), "%s %s", uts.sysname, uts.release);
        snprintf(architecture, sizeof(architecture), "%s", uts.machine);
    } else {
        snprintf(os_name, sizeof(os_name), "Darwin");
        snprintf(architecture, sizeof(architecture), "unknown");
    }
    
    // macOS 使用 sysctl 获取 CPU 信息
    char cpu_brand[256];
    size_t cpu_brand_len = sizeof(cpu_brand);
    if (sysctlbyname("machdep.cpu.brand_string", &cpu_brand, &cpu_brand_len, NULL, 0) == 0) {
        strncpy(cpu_model, cpu_brand, sizeof(cpu_model) - 1);
        cpu_model[sizeof(cpu_model) - 1] = '\0';
    } else {
        // 备用方案：获取 CPU 类型
        int cpu_type;
        size_t cpu_type_len = sizeof(cpu_type);
        if (sysctlbyname("hw.cputype", &cpu_type, &cpu_type_len, NULL, 0) == 0) {
            if (sysctlbyname("hw.cpu_subtype", &cpu_type, &cpu_type_len, NULL, 0) == 0) {
                snprintf(cpu_model, sizeof(cpu_model), "Apple Silicon");
            } else {
                snprintf(cpu_model, sizeof(cpu_model), "Unknown CPU");
            }
        } else {
            snprintf(cpu_model, sizeof(cpu_model), "unknown");
        }
    }
    
    // macOS 使用 sysctl 获取总内存
    uint64_t mem_size;
    size_t mem_size_len = sizeof(mem_size);
    if (sysctlbyname("hw.memsize", &mem_size, &mem_size_len, NULL, 0) == 0) {
        total_ram = (int64_t)mem_size;
    } else {
        total_ram = 0;
    }
    
    // 获取 macOS 版本信息
    char os_version[64];
    size_t os_version_len = sizeof(os_version);
    if (sysctlbyname("kern.osproductversion", &os_version, &os_version_len, NULL, 0) == 0) {
        snprintf(runtime_version, sizeof(runtime_version), "macOS %s", os_version);
    } else {
        snprintf(runtime_version, sizeof(runtime_version), "macOS");
    }
    
    sys_info_loaded = 1;
}

NL_API void nl_sys_info_set_ram_unit(nl_ram_unit_t unit) {
    ram_unit = unit;
}

NL_API nl_ram_unit_t nl_sys_info_get_ram_unit(void) {
    return ram_unit;
}

NL_API const char* nl_sys_info_get_os_name(void) {
    load_sys_info();
    return os_name;
}

NL_API const char* nl_sys_info_get_architecture(void) {
    load_sys_info();
    return architecture;
}

NL_API const char* nl_sys_info_get_cpu_model(void) {
    load_sys_info();
    return cpu_model;
}

NL_API int64_t nl_sys_info_get_total_ram(void) {
    load_sys_info();
    return total_ram;
}

NL_API const char* nl_sys_info_get_runtime_version(void) {
    load_sys_info();
    return runtime_version;
}

NL_API void nl_sys_info_clear_cache(void) {
    sys_info_loaded = 0;
    os_name[0] = '\0';
    architecture[0] = '\0';
    cpu_model[0] = '\0';
    total_ram = 0;
    runtime_version[0] = '\0';
}

NL_API int nl_sys_info_is_loaded(void) {
    return sys_info_loaded;
}
