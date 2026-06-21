#ifndef NETLEAF_IPC_H
#define NETLEAF_IPC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "netleaf_module.h"

// DLL export/import macros
#ifdef _WIN32
    #ifdef NL_IPC_EXPORTS
        #define NL_IPC_API __declspec(dllexport)
    #elif defined(NL_IPC_STATIC)
        #define NL_IPC_API
    #else
        #define NL_IPC_API __declspec(dllimport)
    #endif
#else
    #define NL_IPC_API
#endif

#define NL_IPC_VERSION "2.2.1"
#define NL_IPC_VERSION_MAJOR 2
#define NL_IPC_VERSION_MINOR 2
#define NL_IPC_VERSION_PATCH 1

// =========================================
// Module Information
// =========================================

// IPC module capabilities
#define NL_IPC_CAPABILITIES \
    (NL_CAP_SERVER | NL_CAP_CLIENT | NL_CAP_THREAD_SAFE | \
     NL_CAP_PLATFORM_WIN | NL_CAP_PLATFORM_LINUX)

// IPC module info structure (defined in implementation)
NL_IPC_API nl_module_info_t* nl_ipc_get_module_info(void);

// Check if IPC is available on current platform
NL_IPC_API int nl_ipc_is_available(void);

// Get IPC version string
NL_IPC_API const char* nl_ipc_version(void);

// =========================================
// IPC Server/Client API
// =========================================

// IPC opaque types
typedef struct nl_ipc nl_ipc_t;
typedef struct nl_ipc_conn nl_ipc_conn_t;

NL_IPC_API nl_ipc_t* nl_ipc_create(const char* endpoint);
NL_IPC_API void nl_ipc_destroy(nl_ipc_t* ipc);
NL_IPC_API int nl_ipc_listen(nl_ipc_t* ipc);

NL_IPC_API int nl_ipc_accept(nl_ipc_t* ipc, void** conn);
NL_IPC_API int nl_ipc_connect(nl_ipc_t* ipc, void** conn);

NL_IPC_API int nl_ipc_send(void* conn, const void* data, size_t len);
NL_IPC_API int nl_ipc_recv(void* conn, void* buf, size_t buf_len, size_t* out_len);
NL_IPC_API int nl_ipc_close(void* conn);

#ifdef __cplusplus
}
#endif

#endif // NETLEAF_IPC_H
