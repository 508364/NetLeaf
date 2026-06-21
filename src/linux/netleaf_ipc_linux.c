#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "netleaf_ipc.h"
#include "netleaf_ipc_internal.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

nl_ipc_t* nl_ipc_create(const char* endpoint) {
    if (!endpoint) return NULL;
    
    nl_ipc_t* ipc = (nl_ipc_t*)calloc(1, sizeof(nl_ipc_t));
    if (!ipc) return NULL;
    
    strncpy(ipc->endpoint, endpoint, sizeof(ipc->endpoint) - 1);
    ipc->server_fd = -1;
    ipc->listening = 0;
    
    return ipc;
}

void nl_ipc_destroy(nl_ipc_t* ipc) {
    if (!ipc) return;
    
    if (ipc->server_fd >= 0) {
        close(ipc->server_fd);
    }
    free(ipc);
}

int nl_ipc_listen(nl_ipc_t* ipc) {
    if (!ipc) return -1;
    if (ipc->listening) return 0;
    
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ipc->endpoint, sizeof(addr.sun_path) - 1);
    
    // Remove existing socket file
    unlink(ipc->endpoint);
    
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock_fd);
        return -1;
    }
    
    if (listen(sock_fd, 128) < 0) {
        close(sock_fd);
        return -1;
    }
    
    ipc->server_fd = sock_fd;
    ipc->listening = 1;
    return 0;
}

int nl_ipc_accept(nl_ipc_t* ipc, void** conn) {
    if (!ipc || !conn) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)calloc(1, sizeof(nl_ipc_conn_t));
    if (!c) return -1;
    
    if (ipc->listening && ipc->server_fd >= 0) {
        // Server side: accept a new client connection
        struct sockaddr_un client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(ipc->server_fd, (struct sockaddr*)&client_addr, &addr_len);
        
        if (client_fd < 0) {
            free(c);
            return -1;
        }
        
        c->sock_fd = client_fd;
    } else {
        // Client side: connect to server
        int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            free(c);
            return -1;
        }
        
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, ipc->endpoint, sizeof(addr.sun_path) - 1);
        
        if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock_fd);
            free(c);
            return -1;
        }
        
        c->sock_fd = sock_fd;
    }
    
    *conn = c;
    return 0;
}

int nl_ipc_connect(nl_ipc_t* ipc, void** conn) {
    if (!ipc || !conn) return -1;
    
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) return -1;
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ipc->endpoint, sizeof(addr.sun_path) - 1);
    
    if (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock_fd);
        return -1;
    }
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)calloc(1, sizeof(nl_ipc_conn_t));
    if (!c) {
        close(sock_fd);
        return -1;
    }
    
    c->sock_fd = sock_fd;
    *conn = c;
    return 0;
}

static int write_all(int fd, const void* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = write(fd, (const char*)data + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }
    return (int)sent;
}

int nl_ipc_send(void* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    return write_all(c->sock_fd, data, len);
}

int nl_ipc_recv(void* conn, void* buf, size_t buf_len, size_t* out_len) {
    if (!conn || !buf) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    
    ssize_t n = recv(c->sock_fd, buf, buf_len, 0);
    if (n < 0) return -1;
    if (n == 0) return 0; // Connection closed
    
    if (out_len) *out_len = (size_t)n;
    return 0;
}

int nl_ipc_close(void* conn) {
    if (!conn) return -1;
    
    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    if (c->sock_fd >= 0) {
        shutdown(c->sock_fd, SHUT_RDWR);
        close(c->sock_fd);
    }
    free(c);
    return 0;
}
