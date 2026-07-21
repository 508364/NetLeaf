#include "netleaf_extension_example.h"
#include "netleaf.h"
#include "netleaf_module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_extension_initialized = 0;

// Define extension using simplified NL_EXTENSION_DEFINE macro
// platforms: "Windows,Linux,MacOS" or "all" (case insensitive, any order)
NL_EXTENSION_DEFINE(
    example_ext,                    // library_id
    "Example Extension",            // library_name
    NL_EXAMPLE_VERSION,             // version
    "508364",                       // author
    "示例扩展库演示NetLeaf扩展模式", // description (optional, max 50 Chinese chars)
    "Windows,Linux,MacOS",          // platforms (or "all")
    NL_CAP_THREAD_SAFE,             // capabilities
    nl_example_init,                // init
    nl_example_shutdown,            // shutdown
    nl_example_is_available,        // is_available
    nl_example_version              // get_version
)

NL_EXT_API nl_extension_info_t* nl_example_get_extension_info(void) {
    return NL_EXTENSION_GET_INFO(example_ext);
}

NL_EXT_API int nl_example_is_available(void) {
    return 1;
}

NL_EXT_API const char* nl_example_version(void) {
    return NL_EXAMPLE_VERSION;
}

NL_EXT_API int nl_example_init(void) {
    if (g_extension_initialized) return 0;
    g_extension_initialized = 1;
    printf("[Example Extension] Initialized successfully\n");
    return 0;
}

NL_EXT_API void nl_example_shutdown(void) {
    if (!g_extension_initialized) return;
    g_extension_initialized = 0;
    printf("[Example Extension] Shutdown completed\n");
}

NL_EXT_API int nl_example_process(const char* input, char* output, size_t output_size) {
    if (!input || !output || output_size == 0) {
        return -1;
    }
    
    snprintf(output, output_size, "[Extension] Processed: %s", input);
    return 0;
}

NL_EXT_API int nl_example_get_status(void) {
    return g_extension_initialized;
}