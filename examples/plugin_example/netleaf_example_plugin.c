#define _GNU_SOURCE
#include "netleaf_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#define EXAMPLE_PLUGIN_VERSION "1.0.0"

static int g_plugin_initialized = 0;

static int example_plugin_init(void) {
    if (g_plugin_initialized) return 0;
    g_plugin_initialized = 1;
    printf("[Example Plugin] Initialized successfully\n");
    return 0;
}

static void example_plugin_shutdown(void) {
    if (!g_plugin_initialized) return;
    g_plugin_initialized = 0;
    printf("[Example Plugin] Shutdown completed\n");
}

static int example_plugin_is_available(void) {
    return 1;
}

static const char* example_plugin_version(void) {
    return EXAMPLE_PLUGIN_VERSION;
}

NL_MODULE_DEFINE_LAZY(
    NL_MODULE_CUSTOM,
    example_plugin,
    EXAMPLE_PLUGIN_VERSION,
    NL_CAP_THREAD_SAFE | NL_CAP_PLATFORM_ALL,
    1, 1, 1,
    example_plugin_init,
    example_plugin_shutdown,
    example_plugin_is_available,
    example_plugin_version,
    "Example plugin demonstrating NetLeaf plugin system",
    "508364",
    NULL,
    NULL
);

static nl_plugin_descriptor_t g_plugin_descriptor = {
    .name = "example_plugin",
    .version = EXAMPLE_PLUGIN_VERSION,
    .description = "Example plugin demonstrating NetLeaf plugin system",
    .author = "508364",
    .init = example_plugin_init,
    .shutdown = example_plugin_shutdown,
    .register_module = NULL
};

EXPORT void nl_plugin_register(void) {
    nl_module_register(NL_MODULE_GET_INFO(example_plugin));
    printf("[Example Plugin] Registered with NetLeaf\n");
}

EXPORT nl_plugin_descriptor_t* nl_plugin_get_descriptor(void) {
    return &g_plugin_descriptor;
}

EXPORT void example_plugin_do_something(const char* message) {
    printf("[Example Plugin] Doing something: %s\n", message ? message : "(null)");
}

EXPORT int example_plugin_get_status(void) {
    return g_plugin_initialized;
}
