#ifdef _WIN32
    #define NL_EXPORTS
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>

#include "netleaf.h"

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
        snprintf(os_name, sizeof(os_name), "Linux");
        snprintf(architecture, sizeof(architecture), "unknown");
    }
    
    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo) {
        char line[256];
        while (fgets(line, sizeof(line), cpuinfo)) {
            if (strncmp(line, "model name", 10) == 0) {
                char* colon = strchr(line, ':');
                if (colon) {
                    strncpy(cpu_model, colon + 2, sizeof(cpu_model) - 1);
                    cpu_model[strcspn(cpu_model, "\n")] = 0;
                    break;
                }
            }
        }
        fclose(cpuinfo);
    } else {
        snprintf(cpu_model, sizeof(cpu_model), "unknown");
    }
    
    struct sysinfo info;
    if (sysinfo(&info) == 0) {
        total_ram = (int64_t)info.totalram * info.mem_unit;
    } else {
        total_ram = 0;
    }
    
    snprintf(runtime_version, sizeof(runtime_version), "glibc %d.%d", 
             __GLIBC__, __GLIBC_MINOR__);
    
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