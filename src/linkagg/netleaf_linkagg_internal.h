#ifndef NETLEAF_LINKAGG_INTERNAL_H
#define NETLEAF_LINKAGG_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Platform-specific lock
#ifdef _WIN32
    #include <windows.h>
    typedef CRITICAL_SECTION nl_lagg_mutex_t;
    #define NL_LAGG_MUTEX_INIT(m)   InitializeCriticalSection(m)
    #define NL_LAGG_MUTEX_LOCK(m)   EnterCriticalSection(m)
    #define NL_LAGG_MUTEX_UNLOCK(m) LeaveCriticalSection(m)
    #define NL_LAGG_MUTEX_DESTROY(m) DeleteCriticalSection(m)
#else
    #include <pthread.h>
    typedef pthread_mutex_t nl_lagg_mutex_t;
    #define NL_LAGG_MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
    #define NL_LAGG_MUTEX_LOCK(m)   pthread_mutex_lock(m)
    #define NL_LAGG_MUTEX_UNLOCK(m) pthread_mutex_unlock(m)
    #define NL_LAGG_MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#endif

// Backend server info (internal)
typedef struct nl_lagg_backend {
    int type;  // NL_BACKEND_HTTP or NL_BACKEND_IPC
    char endpoint[512];
    int port;
    int weight;
    int current_connections;
    struct nl_lagg_backend* next;
} nl_lagg_backend_t;

// Link aggregation server
typedef struct nl_lagg_server {
    int port;
    char port_id[256];       // Port ID (format: xxx.xxx)
    int policy;              // nl_lagg_policy_t
    nl_lagg_backend_t* backends;
    int backend_count;       // Must not exceed NL_LINKAGG_MAX_BACKENDS (512)
    int current_index;
    int running;
    nl_lagg_mutex_t mutex;
    int server_fd;           // -1 if not initialized
#ifdef _WIN32
    // Windows-specific fields if needed
#else
    int epoll_fd;
#endif
    nl_lagg_on_connect_cb on_connect;
    nl_lagg_on_disconnect_cb on_disconnect;
    void* user_data;
} nl_lagg_server_t;

// Global ID registry for conflict detection (process-wide)
typedef struct nl_lagg_id_entry {
    char id[256];
    struct nl_lagg_id_entry* next;
} nl_lagg_id_entry_t;

// Global functions for ID management
bool nl_lagg_id_exists(const char* id);
bool nl_lagg_id_register(const char* id);
void nl_lagg_id_unregister(const char* id);

// Global mutex for ID registry (implemented in platform files)
void nl_lagg_init_global_mutex(void);
void nl_lagg_mutex_lock(nl_lagg_mutex_t* m);
void nl_lagg_mutex_unlock(nl_lagg_mutex_t* m);

#endif // NETLEAF_LINKAGG_INTERNAL_H
