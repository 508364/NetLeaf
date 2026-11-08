#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "netleaf_ipc.h"
#include "netleaf_ipc_internal.h"

NL_IPC_API nl_ipc_t* nl_ipc_create(const char* endpoint) {
    if (!endpoint) return NULL;
    
    nl_ipc_t* ipc = (nl_ipc_t*)calloc(1, sizeof(nl_ipc_t));
    if (!ipc) return NULL;
    
    strncpy(ipc->endpoint, endpoint, sizeof(ipc->endpoint) - 1);
    ipc->pipe_handle = INVALID_HANDLE_VALUE;
    ipc->listening = 0;
    
    return ipc;
}

NL_IPC_API void nl_ipc_destroy(nl_ipc_t* ipc) {
    if (!ipc) return;
    
    if (ipc->pipe_handle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(ipc->pipe_handle);
        DisconnectNamedPipe(ipc->pipe_handle);
        CloseHandle(ipc->pipe_handle);
    }
    free(ipc);
}

#define NL_IPC_PIPE_BUFFER 65536

// 创建命名管道实例：采用字节模式，保证与 Unix socket 相同的“原始字节流”
// 语义（消息模式会受 4KB/64KB 消息上限约束，无法承载大请求体）。
static HANDLE create_pipe_instance(const char* endpoint) {
    return CreateNamedPipe(
        endpoint,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        NL_IPC_PIPE_BUFFER, NL_IPC_PIPE_BUFFER, 0, NULL
    );
}

NL_IPC_API int nl_ipc_listen(nl_ipc_t* ipc) {
    if (!ipc) return -1;
    if (ipc->listening) return 0; // Already listening

    ipc->pipe_handle = create_pipe_instance(ipc->endpoint);
    if (ipc->pipe_handle == INVALID_HANDLE_VALUE) return -1;

    ipc->listening = 1;
    return 0;
}

NL_IPC_API int nl_ipc_accept(nl_ipc_t* ipc, void** conn) {
    if (!ipc || !conn) return -1;

    nl_ipc_conn_t* c = (nl_ipc_conn_t*)calloc(1, sizeof(nl_ipc_conn_t));
    if (!c) return -1;

    if (ipc->listening) {
        // 服务端：等待客户端连接当前实例
        HANDLE h = ipc->pipe_handle;
        if (h == INVALID_HANDLE_VALUE) {
            free(c);
            return -1;
        }
        if (!ConnectNamedPipe(h, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_NO_DATA) {
                // 上一个客户端已断开，重置后重试一次
                DisconnectNamedPipe(h);
                if (!ConnectNamedPipe(h, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
                    free(c);
                    return -1;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                free(c);
                return -1;
            }
        }

        // 先创建下一个实例，保证后续 accept 不会因实例耗尽而失败
        ipc->pipe_handle = create_pipe_instance(ipc->endpoint);

        c->pipe_handle = h;
        c->server_side = 1;
    } else {
        // Client side: connect to server
        if (!WaitNamedPipe(ipc->endpoint, NMPWAIT_WAIT_FOREVER)) {
            free(c);
            return -1;
        }

        HANDLE pipe_handle = CreateFile(
            ipc->endpoint,
            GENERIC_READ | GENERIC_WRITE,
            0, NULL, OPEN_EXISTING, 0, NULL
        );

        if (pipe_handle == INVALID_HANDLE_VALUE) {
            free(c);
            return -1;
        }

        c->pipe_handle = pipe_handle;
        c->server_side = 0;
    }

    *conn = c;
    return 0;
}

NL_IPC_API int nl_ipc_connect(nl_ipc_t* ipc, void** conn) {
    if (!ipc || !conn) return -1;
    
    if (!WaitNamedPipe(ipc->endpoint, NMPWAIT_WAIT_FOREVER)) return -1;
    
    HANDLE pipe_handle = CreateFile(
        ipc->endpoint,
        GENERIC_READ | GENERIC_WRITE,
        0, NULL, OPEN_EXISTING, 0, NULL
    );
    
    if (pipe_handle == INVALID_HANDLE_VALUE) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)calloc(1, sizeof(nl_ipc_conn_t));
    if (!c) {
        CloseHandle(pipe_handle);
        return -1;
    }
    
    c->pipe_handle = pipe_handle;
    c->server_side = 0;
    *conn = c;
    return 0;
}

static int write_all(HANDLE h, const void* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        DWORD written;
        if (!WriteFile(h, (const char*)data + sent, (DWORD)(len - sent), &written, NULL)) return -1;
        sent += written;
    }
    return (int)sent;
}

NL_IPC_API int nl_ipc_send(void* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    return write_all(c->pipe_handle, data, len);
}

NL_IPC_API int nl_ipc_recv(void* conn, void* buf, size_t buf_len, size_t* out_len) {
    if (!conn || !buf) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    DWORD read;
    
    if (!ReadFile(c->pipe_handle, buf, (DWORD)buf_len, &read, NULL)) return -1;
    if (read == 0) return 0;
    
    if (out_len) *out_len = (size_t)read;
    return 0;
}

NL_IPC_API int nl_ipc_close(void* conn) {
    if (!conn) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    if (c->pipe_handle != INVALID_HANDLE_VALUE) {
        if (c->server_side) {
            // 仅服务端实例需要断开；客户端句柄调用 DisconnectNamedPipe 无效
            FlushFileBuffers(c->pipe_handle);
            DisconnectNamedPipe(c->pipe_handle);
        }
        CloseHandle(c->pipe_handle);
    }
    free(c);
    return 0;
}
