#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "netleaf_ipc.h"
#include "netleaf_ipc_internal.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

// macOS IPC uses TCP loopback (127.0.0.1) instead of Unix domain sockets
// endpoint format: "127.0.0.1:PORT" or just "PORT"

static int parse_endpoint(const char* endpoint, char* host, int* port) {
    // Try to parse as "host:port" or just "port"
    const char* colon = strchr(endpoint, ':');
    if (colon) {
        size_t host_len = (size_t)(colon - endpoint);
        if (host_len >= 512) host_len = 511;
        strncpy(host, endpoint, host_len);
        host[host_len] = '\0';
        *port = atoi(colon + 1);
    } else {
        strcpy(host, "127.0.0.1");
        *port = atoi(endpoint);
    }
    if (*port <= 0 || *port > 65535) return -1;
    return 0;
}

NL_IPC_API nl_ipc_t* nl_ipc_create(const char* endpoint) {
    if (!endpoint) return NULL;

    nl_ipc_t* ipc = (nl_ipc_t*)calloc(1, sizeof(nl_ipc_t));
    if (!ipc) return NULL;

    strncpy(ipc->endpoint, endpoint, sizeof(ipc->endpoint) - 1);
    ipc->endpoint[sizeof(ipc->endpoint) - 1] = '\0';
    ipc->server_fd = -1;
    ipc->listening = 0;

    return ipc;
}

NL_IPC_API void nl_ipc_destroy(nl_ipc_t* ipc) {
    if (!ipc) return;

    if (ipc->server_fd >= 0) {
        close(ipc->server_fd);
        ipc->server_fd = -1;
    }
    free(ipc);
}

NL_IPC_API int nl_ipc_listen(nl_ipc_t* ipc) {
    if (!ipc) return -1;
    if (ipc->listening) return 0;

    char host[256];
    int port = 0;
    if (parse_endpoint(ipc->endpoint, host, &port) < 0) return -1;

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) return -1;

    int opt = 1;
    setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

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

NL_IPC_API int nl_ipc_accept(nl_ipc_t* ipc, void** conn) {
    if (!ipc || !conn) return -1;

    nl_ipc_conn_t* c = (nl_ipc_conn_t*)calloc(1, sizeof(nl_ipc_conn_t));
    if (!c) return -1;

    if (ipc->listening && ipc->server_fd >= 0) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(ipc->server_fd, (struct sockaddr*)&client_addr, &addr_len);

        if (client_fd < 0) {
            free(c);
            return -1;
        }

        c->sock_fd = client_fd;
    } else {
        char host[256];
        int port = 0;
        if (parse_endpoint(ipc->endpoint, host, &port) < 0) {
            free(c);
            return -1;
        }

        int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            free(c);
            return -1;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, host, &addr.sin_addr);

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

NL_IPC_API int nl_ipc_connect(nl_ipc_t* ipc, void** conn) {
    if (!ipc || !conn) return -1;

    char host[256];
    int port = 0;
    if (parse_endpoint(ipc->endpoint, host, &port) < 0) return -1;

    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, host, &addr.sin_addr);

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
        int n = (int)write(fd, (const char*)data + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return (int)sent;
}

NL_IPC_API int nl_ipc_send(void* conn, const void* data, size_t len) {
    if (!conn || !data || len == 0) return -1;

    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    return write_all(c->sock_fd, data, len);
}

NL_IPC_API int nl_ipc_recv(void* conn, void* buf, size_t buf_len, size_t* out_len) {
    if (!conn || !buf) return -1;

    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;

    ssize_t n = recv(c->sock_fd, buf, buf_len, 0);
    if (n < 0) return -1;
    if (n == 0) return 0;

    if (out_len) *out_len = (size_t)n;
    return 0;
}

NL_IPC_API int nl_ipc_close(void* conn) {
    if (!conn) return -1;

    nl_ipc_conn_t* c = (nl_ipc_conn_t*)conn;
    if (c->sock_fd >= 0) {
        shutdown(c->sock_fd, SHUT_RDWR);
        close(c->sock_fd);
        c->sock_fd = -1;
    }
    free(c);
    return 0;
}
