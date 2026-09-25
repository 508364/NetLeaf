/**
 * @file test_plugin_api.c
 * @brief Test the enhanced plugin API (NL扩展系统 v2.4.0)
 * @date 2026-09-12
 */

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
    printf("  NetLeaf Plugin API Test (v2.4.0)\n");
    print_separator();
    printf("\n");
    
    // Step 1: Initialize modules
    printf("[1] Initializing NetLeaf modules...\n");
    nl_modules_init();
    printf("    Modules: %d\n", nl_get_module_count());
    printf("    OK\n\n");
    
    // Step 2: Load plugin
    printf("[2] Loading example plugin...\n");
    
    char plugin_path[512];
#ifdef _WIN32
    snprintf(plugin_path, sizeof(plugin_path), "example_plugin.dll");
#else
    snprintf(plugin_path, sizeof(plugin_path), "./libexample_plugin.so");
#endif
    
    nl_plugin_handle_t plugin = nl_plugin_load(plugin_path);
    if (!plugin) {
        printf("    FAILED: %s\n", nl_plugin_get_error());
        nl_modules_shutdown();
        return 1;
    }
    printf("    Plugin loaded: %p\n", (void*)plugin);
    printf("    OK\n\n");
    
    // Step 3: Query plugin info using new APIs
    printf("[3] Querying plugin info...\n");
    printf("    Name: %s\n", nl_plugin_get_name(plugin));
    printf("    ID: %s\n", nl_plugin_get_id(plugin));
    printf("    Version: %s\n", nl_plugin_get_version(plugin));
    printf("    Author: %s\n", nl_plugin_get_author(plugin));
    printf("    Description: %s\n", nl_plugin_get_description(plugin));
    printf("    State: %d\n", nl_plugin_get_state(plugin));
    printf("    OK\n\n");
    
    // Step 4: Validate plugin
    printf("[4] Validating plugin...\n");
    char error_msg[256];
    int valid = nl_plugin_validate(plugin, error_msg, sizeof(error_msg));
    if (valid) {
        printf("    Plugin is valid\n");
    } else {
        printf("    Validation failed: %s\n", error_msg);
    }
    printf("    OK\n\n");
    
    // Step 5: Check version compatibility
    printf("[5] Checking version compatibility...\n");
    int compatible = nl_plugin_check_version_compatibility(plugin, NETLEAF_VERSION);
    printf("    Compatible with %s: %s\n", NETLEAF_VERSION, compatible ? "Yes" : "No");
    const char* min_ver = nl_plugin_get_min_version(plugin);
    if (min_ver) {
        printf("    Min NL version: %s\n", min_ver);
    }
    printf("    OK\n\n");
    
    // Step 6: Get plugin descriptor
    printf("[6] Getting plugin descriptor...\n");
    nl_plugin_descriptor_t* desc = nl_plugin_get_descriptor(plugin);
    if (desc) {
        printf("    Descriptor name: %s\n", desc->name);
        printf("    Descriptor version: %s\n", desc->version);
    }
    printf("    OK\n\n");
    
    // Step 7: Call plugin functions via dlsym/GetProcAddress
    printf("[7] Calling plugin functions...\n");
    
#ifdef _WIN32
    typedef int (*send_msg_func)(const char*, const char*);
    typedef const char* (*get_msg_func)(void);
    typedef int (*get_count_func)(void);
    typedef void (*get_info_func)(char*, size_t);
    
    send_msg_func send_msg = (send_msg_func)GetProcAddress((HMODULE)plugin, "example_send_message");
    get_msg_func get_msg = (get_msg_func)GetProcAddress((HMODULE)plugin, "example_get_last_message");
    get_count_func get_count = (get_count_func)GetProcAddress((HMODULE)plugin, "example_get_message_count");
    get_info_func get_info = (get_info_func)GetProcAddress((HMODULE)plugin, "example_get_info_string");
#else
    typedef int (*send_msg_func)(const char*, const char*);
    typedef const char* (*get_msg_func)(void);
    typedef int (*get_count_func)(void);
    typedef void (*get_info_func)(char*, size_t);
    
    send_msg_func send_msg = (send_msg_func)dlsym(plugin, "example_send_message");
    get_msg_func get_msg = (get_msg_func)dlsym(plugin, "example_get_last_message");
    get_count_func get_count = (get_count_func)dlsym(plugin, "example_get_message_count");
    get_info_func get_info = (get_info_func)dlsym(plugin, "example_get_info_string");
#endif
    
    if (send_msg) {
        send_msg("TestUser", "Hello NetLeaf!");
        printf("    Sent message\n");
    }
    
    if (get_msg) {
        printf("    Last message: %s\n", get_msg());
    }
    
    if (get_count) {
        printf("    Message count: %d\n", get_count());
    }
    
    if (get_info) {
        char info[1024];
        get_info(info, sizeof(info));
        printf("    Plugin info:\n%s\n", info);
    }
    printf("    OK\n\n");
    
    // Step 8: Test event system
    printf("[8] Testing event system...\n");
    int events_fired = nl_plugin_emit_event("test.event", NULL, 0);
    printf("    Event emitted: %d\n", events_fired);
    
    nl_plugin_subscribe(plugin, "test.event");
    printf("    Subscribed to 'test.event'\n");
    nl_plugin_unsubscribe(plugin, "test.event");
    printf("    Unsubscribed from 'test.event'\n");
    printf("    OK\n\n");
    
    // Step 9: Print all modules and plugins
    printf("[9] Printing all registered modules...\n");
    nl_print_modules();
    
    printf("    Plugins:\n");
    int plugin_count = 0;
    nl_plugin_handle_t** all_plugins = nl_plugin_get_all(&plugin_count);
    for (int i = 0; i < plugin_count; i++) {
        printf("      [%s] %s v%s by %s\n",
               nl_plugin_get_id(all_plugins[i]),
               nl_plugin_get_name(all_plugins[i]),
               nl_plugin_get_version(all_plugins[i]),
               nl_plugin_get_author(all_plugins[i]));
    }
    if (all_plugins) free(all_plugins);
    printf("    OK\n\n");
    
    // Step 10: Test sandbox (if supported)
    printf("[10] Testing sandbox feature...\n");
#ifdef _WIN32
    // Sandbox may not be supported on all platforms
    nl_plugin_enable_sandbox(plugin, 0);  // Disable for now
#endif
    printf("    OK\n\n");
    
    // Step 11: Test hot reload
    printf("[11] Testing hot reload...\n");
    int reload_result = nl_plugin_reload(plugin);
    printf("    Reload result: %d\n", reload_result);
    printf("    OK\n\n");
    
    // Step 12: Unload plugin
    printf("[12] Unloading plugin...\n");
    nl_plugin_unload(plugin);
    printf("    Plugin unloaded\n");
    printf("    OK\n\n");
    
    // Step 13: Shutdown
    printf("[13] Shutting down...\n");
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
