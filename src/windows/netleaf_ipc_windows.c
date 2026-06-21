#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "netleaf_ipc.h"
#include "netleaf_ipc_internal.h"

nl_ipc_t* nl_ipc_create(const char* endpoint) {
    if (!endpoint) return NULL;
    
    nl_ipc_t* ipc = (nl_ipc_t*)calloc(1, sizeof(nl_ipc_t));
    if (!ipc) return NULL;
    
    strncpy(ipc->endpoint, endpoint, sizeof(ipc->endpoint) - 1);
    ipc->pipe_handle = INVALID_HANDLE_VALUE;
    ipc->listening = 0;
    
    return ipc;
}

void nl_ipc_destroy(nl_ipc_t* ipc) {
    if (!ipc) return;
    
    if (ipc->pipe_handle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(ipc->pipe_handle);
        DisconnectNamedPipe(ipc->pipe_handle);
        CloseHandle(ipc->pipe_handle);
    }
    free(ipc);
}

int nl_ipc_listen(nl_ipc_t* ipc) {
    if (!ipc) return -1;
    if (ipc->listening) return 0; // Already listening
    
    ipc->pipe_handle = CreateNamedPipe(
        ipc->endpoint,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        4096, 4096, 0, NULL
    );
    
    if (ipc->pipe_handle == INVALID_HANDLE_VALUE) return -1;
    
    ipc->listening = 1;
    return 0;
}

int nl_ipc_accept(nl_ipc_t* ipc, void** conn) {
    if (!ipc || !conn) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)calloc(1, sizeof(nl_ipc_conn_t));
    if (!c) return -1;
    
    if (ipc->listening) {
        // Server side: wait for a client connection
        if (!ConnectNamedPipe(ipc->pipe_handle, NULL)) {
            DWORD err = GetLastError();
            // ERROR_PIPE_CONNECTED: another thread already connected
            // ERROR_NO_DATA: pipe closed but we should still try to use it
            if (err != ERROR_PIPE_CONNECTED && err != ERROR_NO_DATA) {
                free(c);
                return -1;
            }
        }
        
        // Re-open the pipe for the new connection
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
    }
    
    *conn = c;
    return 0;
}

int nl_ipc_connect(nl_ipc_t* ipc, void** conn) {
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

int nl_ipc_send(void* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    return write_all(c->pipe_handle, data, len);
}

int nl_ipc_recv(void* conn, void* buf, size_t buf_len, size_t* out_len) {
    if (!conn || !buf) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    DWORD read;
    
    if (!ReadFile(c->pipe_handle, buf, (DWORD)buf_len, &read, NULL)) return -1;
    if (read == 0) return 0;
    
    if (out_len) *out_len = (size_t)read;
    return 0;
}

int nl_ipc_close(void* conn) {
    if (!conn) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    if (c->pipe_handle != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(c->pipe_handle);
        DisconnectNamedPipe(c->pipe_handle);
        CloseHandle(c->pipe_handle);
    }
    free(c);
    return 0;
}
