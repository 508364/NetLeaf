#ifndef NETLEAF_EXTENSION_EXAMPLE_H
#define NETLEAF_EXTENSION_EXAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

#ifdef _WIN32
    #ifdef NL_EXTENSION_EXPORTS
        #define NL_EXT_API __declspec(dllexport)
    #else
        #define NL_EXT_API __declspec(dllimport)
    #endif
#else
    #define NL_EXT_API
#endif

#define NL_EXAMPLE_VERSION "1.0.0"

// Extension API - follows NetLeaf extension pattern
NL_EXT_API nl_extension_info_t* nl_example_get_extension_info(void);
NL_EXT_API int nl_example_is_available(void);
NL_EXT_API const char* nl_example_version(void);
NL_EXT_API int nl_example_init(void);
NL_EXT_API void nl_example_shutdown(void);

// Extension functionality
NL_EXT_API int nl_example_process(const char* input, char* output, size_t output_size);
NL_EXT_API int nl_example_get_status(void);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_EXTENSION_EXAMPLE_H