#ifdef _WIN32
    #include <windows.h>
    #include <tchar.h>
    #include <stdio.h>
    #include <string.h>
    #include <stdlib.h>

    #include "netleaf.h"

    static int sys_info_loaded = 0;
    static nl_ram_unit_t ram_unit = NL_RAM_UNIT_BINARY;

    static char os_name[256] = "";
    static char architecture[64] = "";
    static char cpu_model[256] = "";
    static int64_t total_ram = 0;
    static char runtime_version[64] = "";

    static void load_sys_info(void) {
        if (sys_info_loaded) return;

        OSVERSIONINFOEX osvi;
        ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
        osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);

        if (GetVersionEx((LPOSVERSIONINFO)&osvi)) {
            if (osvi.dwMajorVersion == 10 && osvi.dwMinorVersion == 0) {
                snprintf(os_name, sizeof(os_name), "Windows 10/11 %d.%d", 
                         osvi.dwMajorVersion, osvi.dwMinorVersion);
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 3) {
                snprintf(os_name, sizeof(os_name), "Windows 8.1");
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 2) {
                snprintf(os_name, sizeof(os_name), "Windows 8");
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1) {
                snprintf(os_name, sizeof(os_name), "Windows 7");
            } else {
                snprintf(os_name, sizeof(os_name), "Windows %d.%d", 
                         osvi.dwMajorVersion, osvi.dwMinorVersion);
            }
        } else {
            snprintf(os_name, sizeof(os_name), "Windows");
        }

        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        switch (si.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64:
                snprintf(architecture, sizeof(architecture), "x64");
                break;
            case PROCESSOR_ARCHITECTURE_INTEL:
                snprintf(architecture, sizeof(architecture), "x86");
                break;
            case PROCESSOR_ARCHITECTURE_ARM:
                snprintf(architecture, sizeof(architecture), "ARM");
                break;
            case PROCESSOR_ARCHITECTURE_ARM64:
                snprintf(architecture, sizeof(architecture), "ARM64");
                break;
            default:
                snprintf(architecture, sizeof(architecture), "unknown");
        }

        HKEY hKey;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, 
                         "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 
                         0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            char buffer[256];
            DWORD size = sizeof(buffer);
            if (RegQueryValueEx(hKey, "ProcessorNameString", NULL, NULL, 
                               (LPBYTE)buffer, &size) == ERROR_SUCCESS) {
                strncpy(cpu_model, buffer, sizeof(cpu_model) - 1);
            } else {
                snprintf(cpu_model, sizeof(cpu_model), "unknown");
            }
            RegCloseKey(hKey);
        } else {
            snprintf(cpu_model, sizeof(cpu_model), "unknown");
        }

        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(MEMORYSTATUSEX);
        if (GlobalMemoryStatusEx(&memInfo)) {
            total_ram = (int64_t)memInfo.ullTotalPhys;
        } else {
            total_ram = 0;
        }

        snprintf(runtime_version, sizeof(runtime_version), "MSVCRT");

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
#endif