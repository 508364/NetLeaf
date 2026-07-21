#include "netleaf.h"
#include "netleaf_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void print_separator(void) {
    printf("==================================================\n");
}

int main() {
    int result = 0;
    
    printf("\n");
    print_separator();
    printf("  NetLeaf Extension Library Test (v2.2.2)\n");
    print_separator();
    printf("\n");
    
    // Step 1: Initialize modules
    printf("[1] Initializing modules...\n");
    nl_modules_init();
    printf("    Module count: %d\n", nl_get_module_count());
    printf("    Extension count: %d\n", nl_extension_get_count());
    printf("    OK\n\n");
    
    // Step 2: Load extension library dynamically
    printf("[2] Loading extension library...\n");
    
    char ext_path[512];
#ifdef _WIN32
    snprintf(ext_path, sizeof(ext_path), "netleaf_extension.dll");
#else
    snprintf(ext_path, sizeof(ext_path), "./netleaf_extension.so");
#endif
    
    nl_plugin_handle_t ext = nl_plugin_load(ext_path);
    if (!ext) {
        printf("    FAILED: %s\n", nl_plugin_get_error());
        
        // Try alternate path
        snprintf(ext_path, sizeof(ext_path), "bin/Release/netleaf_extension.dll");
        printf("    Trying: %s\n", ext_path);
        ext = nl_plugin_load(ext_path);
        
        if (!ext) {
            printf("    FAILED again: %s\n", nl_plugin_get_error());
            nl_modules_shutdown();
            return 1;
        }
    }
    printf("    Extension loaded: %p\n", (void*)ext);
    printf("    OK\n\n");
    
    // Step 3: Get extension info using new API
    printf("[3] Getting extension info...\n");
    
#ifdef _WIN32
    typedef nl_extension_info_t* (*get_ext_info_func)(void);
    get_ext_info_func get_info = (get_ext_info_func)GetProcAddress((HMODULE)ext, "nl_example_get_extension_info");
#else
    typedef nl_extension_info_t* (*get_ext_info_func)(void);
    get_ext_info_func get_info = (get_ext_info_func)dlsym(ext, "nl_example_get_extension_info");
#endif
    
    nl_extension_info_t* ext_info = NULL;
    if (get_info) {
        ext_info = get_info();
        if (ext_info) {
            printf("    Library Name: %s\n", ext_info->library_name);
            printf("    Library ID: %s\n", ext_info->library_id);
            printf("    Version: %s\n", ext_info->version);
            printf("    Author: %s\n", ext_info->author);
            printf("    Description: %s\n", ext_info->description);
            printf("    Platforms: Win=%d, Linux=%d, macOS=%d\n", 
                   ext_info->platform_windows, ext_info->platform_linux, ext_info->platform_macos);
            printf("    OK\n\n");
        }
    } else {
        printf("    WARNING: Could not get extension info function\n\n");
    }
    
    // Step 4: Register extension using new API
    printf("[4] Registering extension...\n");
    if (ext_info) {
        int reg_result = nl_extension_register(ext_info);
        printf("    Register result: %d\n", reg_result);
        printf("    Extension count: %d\n", nl_extension_get_count());
        printf("    OK\n\n");
    }
    
    // Step 5: Find extension by library_id
    printf("[5] Finding extension by library_id...\n");
    nl_extension_info_t* found = nl_extension_get_info("example_ext");
    if (found) {
        printf("    Found: %s (id=%s)\n", found->library_name, found->library_id);
        printf("    OK\n\n");
    } else {
        printf("    FAILED: Not found\n");
        result = 1;
    }
    
    // Step 6: Call extension functions
    printf("[6] Calling extension functions...\n");
    
#ifdef _WIN32
    typedef int (*process_func)(const char*, char*, size_t);
    process_func process = (process_func)GetProcAddress((HMODULE)ext, "nl_example_process");
    
    typedef int (*status_func)(void);
    status_func get_status = (status_func)GetProcAddress((HMODULE)ext, "nl_example_get_status");
#else
    typedef int (*process_func)(const char*, char*, size_t);
    process_func process = (process_func)dlsym(ext, "nl_example_process");
    
    typedef int (*status_func)(void);
    status_func get_status = (status_func)dlsym(ext, "nl_example_get_status");
#endif
    
    if (process) {
        char output[256];
        int ret = process("Hello Extension", output, sizeof(output));
        printf("    Process result: %d\n", ret);
        printf("    Output: %s\n", output);
    }
    
    if (get_status) {
        printf("    Status: %d\n", get_status());
    }
    printf("    OK\n\n");
    
    // Step 7: Print all modules and extensions
    printf("[7] Printing all modules...\n");
    nl_print_modules();
    
    printf("    Extensions:\n");
    int ext_count = 0;
    nl_extension_info_t** extensions = nl_extension_get_all(&ext_count);
    for (int i = 0; i < ext_count; i++) {
        printf("      [%s] %s v%s by %s\n", 
               extensions[i]->library_id, 
               extensions[i]->library_name, 
               extensions[i]->version, 
               extensions[i]->author);
    }
    if (extensions) free(extensions);
    printf("    OK\n\n");
    
    // Step 8: Unload extension
    printf("[8] Unloading extension...\n");
    nl_plugin_unload(ext);
    printf("    OK\n\n");
    
    // Step 9: Shutdown
    printf("[9] Shutting down...\n");
    nl_modules_shutdown();
    printf("    OK\n\n");
    
    print_separator();
    if (result == 0) {
        printf("  ALL TESTS PASSED\n");
    } else {
        printf("  SOME TESTS FAILED\n");
    }
    print_separator();
    printf("\n");
    
    return result;
}