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
struct nl_ipc_conn {
#ifdef _WIN32
    HANDLE pipe_handle;
    int server_side;    // 1 = 服务端管道实例（close 时需 DisconnectNamedPipe）
    OVERLAPPED overlapped;
#else
    int sock_fd;
#endif
};

#endif // NETLEAF_IPC_INTERNAL_H
