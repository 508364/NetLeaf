#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include "netleaf_linkagg.h"
#include "netleaf_linkagg_internal.h"
#include "netleaf_ipc.h"

#define MAX_EVENTS 64
#define BUFFER_SIZE 8192
#define RESPONSE_SIZE 16384

// Backend type constants
#define BACKEND_HTTP 0
#define BACKEND_IPC  1

// --- Load balancing ---

static nl_lagg_backend_t* select_backend(nl_lagg_server_t* server) {
    if (!server || !server->backends) return NULL;
    
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    
    nl_lagg_backend_t* result = NULL;
    
    switch (server->policy) {
        case NL_POLICY_ROUND_ROBIN:
            if (server->backends) {
                result = server->backends;
                server->current_index = (server->current_index + 1) % server->backend_count;
            }
            break;
        case NL_POLICY_LEAST_CONNECTIONS: {
            int min_conn = INT_MAX;
            nl_lagg_backend_t* b = server->backends;
            while (b) {
                if (b->current_connections < min_conn) {
                    min_conn = b->current_connections;
                    result = b;
                }
                b = b->next;
            }
            break;
        }
        case NL_POLICY_RANDOM: {
            int idx = rand() % server->backend_count;
            nl_lagg_backend_t* b = server->backends;
            for (int i = 0; i <= idx && b; i++) {
                if (i == idx) { result = b; break; }
                b = b->next;
            }
            break;
        }
        case NL_POLICY_WEIGHTED_ROUND_ROBIN: {
            nl_lagg_backend_t* b = server->backends;
            while (b && b->weight <= 0) b = b->next;
            result = b;
            break;
        }
        default:
            result = server->backends;
            break;
    }
    
    if (result) result->current_connections++;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return result;
}

static void release_backend(nl_lagg_server_t* server, nl_lagg_backend_t* backend) {
    if (!server || !backend) return;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    backend->current_connections--;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
}

static int send_all(int sock, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += n;
    }
    return sent;
}

static int read_with_timeout(int sock, char* buf, int buf_len, int timeout_ms) {
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) return ret;
    return recv(sock, buf, buf_len, 0);
}

// --- Global mutex for ID registry (exported) ---

static nl_lagg_mutex_t g_id_mutex;
static int g_id_mutex_initialized = 0;

// Initialize global mutex (must be called before using ID registry)
void nl_lagg_init_global_mutex(void) {
    if (!g_id_mutex_initialized) {
        NL_LAGG_MUTEX_INIT(&g_id_mutex);
        g_id_mutex_initialized = 1;
    }
}

// Mutex wrappers for cross-platform compatibility
void nl_lagg_mutex_lock(nl_lagg_mutex_t* m) {
    nl_lagg_init_global_mutex();
    NL_LAGG_MUTEX_LOCK(m);
}

void nl_lagg_mutex_unlock(nl_lagg_mutex_t* m) {
    NL_LAGG_MUTEX_UNLOCK(m);
}

// --- Public API ---

nl_lagg_server_t* nl_lagg_create(int port) {
    // Initialize global ID registry mutex
    nl_lagg_init_global_mutex();

    srand((unsigned int)time(NULL)); // Seed random

    nl_lagg_server_t* server = (nl_lagg_server_t*)calloc(1, sizeof(nl_lagg_server_t));
    if (!server) return NULL;

    server->port = port;
    server->port_id[0] = '\0';  // No ID set initially
    server->policy = NL_POLICY_ROUND_ROBIN;
    server->current_index = 0;
    server->running = 0;
    server->server_fd = -1;
    server->epoll_fd = -1;
    server->backends = NULL;
    server->backend_count = 0;
    NL_LAGG_MUTEX_INIT(&server->mutex);
    server->on_connect = NULL;
    server->on_disconnect = NULL;
    return server;
}

void nl_lagg_destroy(nl_lagg_server_t* server) {
    if (!server) return;
    if (server->epoll_fd >= 0) close(server->epoll_fd);
    nl_lagg_backend_t* b = server->backends;
    while (b) {
        nl_lagg_backend_t* next = b->next;
        free(b);
        b = next;
    }
    NL_LAGG_MUTEX_DESTROY(&server->mutex);
    free(server);
}

int nl_lagg_start(nl_lagg_server_t* server) {
    if (!server || server->running) return -1;
    
    server->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_fd < 0) return -1;
    
    int opt = 1;
    setsockopt(server->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Set timeout for accept so nl_lagg_stop can interrupt
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(server->server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server->port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(server->server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }
    if (listen(server->server_fd, 128) != 0) {
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }
    
    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd < 0) {
        close(server->server_fd);
        server->server_fd = -1;
        return -1;
    }
    
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server->server_fd;
    epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->server_fd, &ev);
    
    server->running = 1;
    
    struct epoll_event events[MAX_EVENTS];
    
    while (server->running) {
        int n = epoll_wait(server->epoll_fd, events, MAX_EVENTS, 1000);
        
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        
        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == server->server_fd) {
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(server->server_fd, (struct sockaddr*)&client_addr, &client_len);
                
                if (client_fd < 0) continue;
                
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
                
                ev.events = EPOLLIN | EPOLLRDHUP;
                ev.data.fd = client_fd;
                epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
                
                if (server->on_connect) server->on_connect(client_fd, server->user_data);
                
                // Read HTTP request
                char request[BUFFER_SIZE];
                int total = 0;
                while (total < BUFFER_SIZE - 1) {
                    int ret = recv(client_fd, request + total, 1, 0);
                    if (ret <= 0) break;
                    total += ret;
                    if (total >= 4 && request[total-4] == '\r' && request[total-3] == '\n' &&
                        request[total-2] == '\r' && request[total-1] == '\n') break;
                }
                
                if (total <= 0) {
                    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                    if (server->on_disconnect) server->on_disconnect(client_fd, server->user_data);
                    continue;
                }
                request[total] = '\0';
                
                // Find body and Content-Length with overflow protection
                char* body_start = strstr(request, "\r\n\r\n");
                int body_offset = 0, body_len = 0;
                if (body_start) {
                    body_offset = (int)(body_start - request) + 4;
                    char* cl = strstr(request, "Content-Length:");
                    if (cl) {
                        body_len = atoi(cl + 15);
                        // Protect against malicious Content-Length
                        if (body_len < 0 || body_len > 4 * 1024 * 1024) { // Max 4MB
                            body_len = 0;
                        }
                    }
                }
                
                // Read body with bounds check
                if (body_len > 0 && total < body_offset + body_len && body_offset + body_len <= BUFFER_SIZE) {
                    int remaining = body_offset + body_len - total;
                    int received = 0;
                    while (received < remaining) {
                        int ret = recv(client_fd, request + total + received, remaining - received, 0);
                        if (ret <= 0) break;
                        received += ret; total += ret;
                    }
                }
                
                nl_lagg_backend_t* backend = select_backend(server);
                if (!backend) {
                    send(client_fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n", 58, MSG_NOSIGNAL);
                    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                    continue;
                }
                
                char response[RESPONSE_SIZE];
                int resp_total = 0;
                
                if (backend->type == BACKEND_HTTP) {
                    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
                    if (backend_fd >= 0) {
                        struct sockaddr_in backend_addr;
                        memset(&backend_addr, 0, sizeof(backend_addr));
                        backend_addr.sin_family = AF_INET;
                        backend_addr.sin_port = htons((uint16_t)backend->port);
                        inet_pton(AF_INET, "127.0.0.1", &backend_addr.sin_addr);
                        
                        if (connect(backend_fd, (struct sockaddr*)&backend_addr, sizeof(backend_addr)) == 0) {
                            send_all(backend_fd, request, total);
                            while (resp_total < RESPONSE_SIZE - 1) {
                                int ret = read_with_timeout(backend_fd, response + resp_total,
                                                            RESPONSE_SIZE - resp_total - 1, 5000);
                                if (ret <= 0) break;
                                resp_total += ret;
                            }
                            response[resp_total] = '\0';
                            send_all(client_fd, response, resp_total);
                        }
                        close(backend_fd);
                    }
                } else { // BACKEND_IPC
                    void* ipc_conn = NULL;
                    nl_ipc_t* ipc = nl_ipc_create(backend->endpoint);
                    if (ipc && nl_ipc_connect(ipc, &ipc_conn) == 0) {
                        nl_ipc_send(ipc_conn, request, total);
                        size_t read_len;
                        while (resp_total < RESPONSE_SIZE - 1) {
                            int ret = nl_ipc_recv(ipc_conn, response + resp_total,
                                                   RESPONSE_SIZE - resp_total - 1, &read_len);
                            if (ret != 0 || read_len == 0) break;
                            resp_total += (int)read_len;
                        }
                        response[resp_total] = '\0';
                        send_all(client_fd, response, resp_total);
                        nl_ipc_close(ipc_conn);
                        nl_ipc_destroy(ipc);
                    }
                }
                
                release_backend(server, backend);
                epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                close(client_fd);
                
                if (server->on_disconnect) server->on_disconnect(client_fd, server->user_data);
            } else {
                // Client event (EPOLLRDHUP = disconnect)
                int fd = events[i].data.fd;
                epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
            }
        }
    }
    
    if (server->epoll_fd >= 0) close(server->epoll_fd);
    if (server->server_fd >= 0) close(server->server_fd);
    server->epoll_fd = -1;
    server->server_fd = -1;
    server->running = 0;
    return 0;
}

void nl_lagg_stop(nl_lagg_server_t* server) {
    if (!server) return;
    server->running = 0;
    if (server->server_fd >= 0) {
        // Force close to interrupt blocking accept
        close(server->server_fd);
        server->server_fd = -1;
    }
}

int nl_lagg_add_http_backend(nl_lagg_server_t* server, const char* host, int port, int weight) {
    if (!server || !host || port <= 0) return NL_LINKAGG_ERROR_INVALID_PARAM;

    NL_LAGG_MUTEX_LOCK(&server->mutex);

    // Check backend limit (512 max)
    if (server->backend_count >= NL_LINKAGG_MAX_BACKENDS) {
        NL_LAGG_MUTEX_UNLOCK(&server->mutex);
        return NL_LINKAGG_ERROR_TOO_MANY_BACKENDS;  // -8: Exceeded 512 backends
    }

    nl_lagg_backend_t* b = (nl_lagg_backend_t*)calloc(1, sizeof(nl_lagg_backend_t));
    if (!b) {
        NL_LAGG_MUTEX_UNLOCK(&server->mutex);
        return NL_LINKAGG_ERROR_MEMORY;
    }
    b->type = BACKEND_HTTP; // NL_BACKEND_HTTP
    snprintf(b->endpoint, sizeof(b->endpoint), "%s:%d", host, port);
    b->port = port;
    b->weight = weight;
    b->current_connections = 0;
    b->next = server->backends;
    server->backends = b;
    server->backend_count++;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return NL_LINKAGG_OK;
}

int nl_lagg_add_ipc_backend(nl_lagg_server_t* server, const char* endpoint, int weight) {
    if (!server || !endpoint) return NL_LINKAGG_ERROR_INVALID_PARAM;

    NL_LAGG_MUTEX_LOCK(&server->mutex);

    // Check backend limit (512 max)
    if (server->backend_count >= NL_LINKAGG_MAX_BACKENDS) {
        NL_LAGG_MUTEX_UNLOCK(&server->mutex);
        return NL_LINKAGG_ERROR_TOO_MANY_BACKENDS;  // -8: Exceeded 512 backends
    }

    nl_lagg_backend_t* b = (nl_lagg_backend_t*)calloc(1, sizeof(nl_lagg_backend_t));
    if (!b) {
        NL_LAGG_MUTEX_UNLOCK(&server->mutex);
        return NL_LINKAGG_ERROR_MEMORY;
    }
    b->type = BACKEND_IPC; // NL_BACKEND_IPC
    strncpy(b->endpoint, endpoint, sizeof(b->endpoint) - 1);
    b->port = 0;
    b->weight = weight;
    b->current_connections = 0;
    b->next = server->backends;
    server->backends = b;
    server->backend_count++;
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return NL_LINKAGG_OK;
}

int nl_lagg_remove_backend(nl_lagg_server_t* server, const char* endpoint) {
    if (!server || !endpoint) return -1;
    NL_LAGG_MUTEX_LOCK(&server->mutex);
    nl_lagg_backend_t* prev = NULL;
    nl_lagg_backend_t* b = server->backends;
    while (b) {
        if (strcmp(b->endpoint, endpoint) == 0) {
            if (prev) prev->next = b->next;
            else server->backends = b->next;
            server->backend_count--;
            free(b);
            NL_LAGG_MUTEX_UNLOCK(&server->mutex);
            return 0;
        }
        prev = b; b = b->next;
    }
    NL_LAGG_MUTEX_UNLOCK(&server->mutex);
    return -1;
}

void nl_lagg_set_policy(nl_lagg_server_t* server, nl_lagg_policy_t policy) {
    if (!server) return;
    server->policy = policy;
}

void nl_lagg_set_on_connect(nl_lagg_server_t* server, nl_lagg_on_connect_cb cb, void* user_data) {
    if (!server) return;
    server->on_connect = cb;
    server->user_data = user_data;
}

void nl_lagg_set_on_disconnect(nl_lagg_server_t* server, nl_lagg_on_disconnect_cb cb, void* user_data) {
    if (!server) return;
    server->on_disconnect = cb;
    server->user_data = user_data;
}
