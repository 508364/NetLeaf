#ifndef NETLEAF_IPC_INTERNAL_H
#define NETLEAF_IPC_INTERNAL_H

#include <stddef.h>

// Internal IPC structures
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

// IPC server structure (matches public typedef)
typedef struct nl_ipc nl_ipc_t;
struct nl_ipc {
#ifdef _WIN32
    HANDLE pipe_handle;
    char endpoint[512];
#else
    int server_fd;
    char endpoint[512];
#endif
    volatile int listening;
};

// IPC connection structure
typedef struct nl_ipc_conn nl_ipc_conn_t;
struct nl_ipc_conn {
#ifdef _WIN32
    HANDLE pipe_handle;
    OVERLAPPED overlapped;
#else
    int sock_fd;
#endif
};

#endif // NETLEAF_IPC_INTERNAL_H
