#define _DARWIN_C_SOURCE
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <limits.h>

#include "../include/netleaf.h"

#define MAX_EVENTS 1024
#define BUFFER_SIZE 8192
#define MAX_CLIENTS 65535

struct nl_server {
    int fd;
    int kqueue_fd;
    int port;
    nl_protocol_t protocol;
    nl_request_handler handler;
    nl_udp_message_handler udp_handler;
    void* user_data;
    volatile int running;
    pthread_t thread_id;
    struct kevent events[MAX_EVENTS];
};

struct nl_client {
    int fd;
    nl_protocol_t protocol;
    struct sockaddr_in addr;
    int connected;
    char buffer[BUFFER_SIZE];
    size_t buffer_len;
};

struct nl_config {
    char data[4096];
    int count;
    pthread_mutex_t mutex;
};

struct nl_buffer {
    char* data;
    size_t capacity;
    size_t length;
    pthread_mutex_t mutex;
};

struct nl_event_loop {
    int kqueue_fd;
    volatile int running;
    pthread_t thread_id;
};

static nl_log_level_t current_log_level = NL_LOG_INFO;
static nl_log_callback log_callback = NULL;
static void* log_user_data = NULL;
static int debug_mode = 0;

static void macos_log(nl_log_level_t level, const char* fmt, ...) {
    if (level < current_log_level) return;
    
    va_list args;
    va_start(args, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    
    if (log_callback) {
        log_callback(level, msg, log_user_data);
    } else {
        const char* prefix[] = {"DEBUG", "INFO", "WARN", "ERROR"};
        time_t now = time(NULL);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);
        fprintf(stderr, "[%s] [%s] %s\n", time_str, prefix[level], msg);
    }
}

void nl_debug_enable(int enable) {
    debug_mode = enable;
    if (enable) {
        current_log_level = NL_LOG_DEBUG;
        macos_log(NL_LOG_INFO, "Debug mode enabled");
    } else {
        macos_log(NL_LOG_INFO, "Debug mode disabled");
    }
}

int nl_debug_is_enabled(void) {
    return debug_mode;
}

void nl_log(nl_log_level_t level, const char* format, ...) {
    if (level < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    macos_log(level, "%s", msg);
}

void nl_log_debug(const char* format, ...) {
    if (NL_LOG_DEBUG < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    macos_log(NL_LOG_DEBUG, "%s", msg);
}

void nl_log_info(const char* format, ...) {
    if (NL_LOG_INFO < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    macos_log(NL_LOG_INFO, "%s", msg);
}

void nl_log_warn(const char* format, ...) {
    if (NL_LOG_WARN < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    macos_log(NL_LOG_WARN, "%s", msg);
}

void nl_log_error(const char* format, ...) {
    if (NL_LOG_ERROR < current_log_level) return;
    
    va_list args;
    va_start(args, format);
    char msg[1024];
    vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);
    
    macos_log(NL_LOG_ERROR, "%s", msg);
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_reuseaddr(int fd) {
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
}

static int set_reuseport(int fd) {
#ifdef SO_REUSEPORT
    int opt = 1;
    return setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#else
    (void)fd;
    return 0;
#endif
}

static int set_tcp_nodelay(int fd, int enable) {
    int opt = enable ? 1 : 0;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}

static int set_tcp_keepalive(int fd, int enable, int idle, int interval, int count) {
    int opt = enable ? 1 : 0;
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) == -1) {
        return -1;
    }
#ifdef TCP_KEEPIDLE
    if (enable && idle > 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    }
#else
    (void)idle;
#endif
#ifdef TCP_KEEPINTVL
    if (enable && interval > 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    }
#else
    (void)interval;
#endif
#ifdef TCP_KEEPCNT
    if (enable && count > 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    }
#else
    (void)count;
#endif
    return 0;
}

static int set_buffer_sizes(int fd, int sndbuf, int rcvbuf) {
    if (sndbuf > 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    }
    if (rcvbuf > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    }
    return 0;
}

static int set_broadcast(int fd, int enable) {
    int opt = enable ? 1 : 0;
    return setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
}

static void* server_thread(void* arg) {
    nl_server_t* server = (nl_server_t*)arg;
    
    while (server->running) {
        struct timespec timeout;
        timeout.tv_sec = 1;
        timeout.tv_nsec = 0;
        
        int nfds = kevent(server->kqueue_fd, NULL, 0, server->events, MAX_EVENTS, &timeout);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            macos_log(NL_LOG_ERROR, "macOS: kevent() failed: %s", strerror(errno));
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            int fd = (int)(uintptr_t)server->events[i].ident;
            
            if (fd == server->fd) {
                if (server->protocol == NL_PROTO_UDP) {
                    if (server->udp_handler) {
                        char buf[BUFFER_SIZE];
                        struct sockaddr_in addr;
                        socklen_t addr_len = sizeof(addr);
                        ssize_t len = recvfrom(fd, buf, BUFFER_SIZE, 0,
                                              (struct sockaddr*)&addr, &addr_len);
                        if (len > 0) {
                            char addr_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &addr.sin_addr, addr_str, sizeof(addr_str));
                            server->udp_handler(buf, (size_t)len, addr_str, 
                                              ntohs(addr.sin_port), server->user_data);
                        }
                    }
                } else {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
                    if (client_fd != -1) {
                        set_nonblocking(client_fd);
                        struct kevent ev;
                        EV_SET(&ev, client_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
                        kevent(server->kqueue_fd, &ev, 1, NULL, 0, NULL);
                    }
                }
            } else {
                if (server->handler && (server->events[i].filter == EVFILT_READ)) {
                    nl_buffer_t* req = nl_buffer_create(BUFFER_SIZE);
                    nl_buffer_t* resp = nl_buffer_create(BUFFER_SIZE);
                    
                    char buf[BUFFER_SIZE];
                    ssize_t n = read(fd, buf, BUFFER_SIZE);
                    if (n > 0) {
                        nl_buffer_write(req, buf, (size_t)n);
                        server->handler(req, resp, server->user_data);
                        
                        if (nl_buffer_size(resp) > 0) {
                            write(fd, resp->data, nl_buffer_size(resp));
                        }
                    } else if (n == 0) {
                        close(fd);
                    }
                    
                    nl_buffer_destroy(req);
                    nl_buffer_destroy(resp);
                }
            }
        }
    }
    
    return NULL;
}

nl_server_t* nl_server_create(nl_protocol_t protocol, int port) {
    nl_server_t* server = calloc(1, sizeof(nl_server_t));
    if (!server) return NULL;
    
    server->protocol = protocol;
    server->port = port;
    server->kqueue_fd = kqueue();
    
    if (server->kqueue_fd == -1) {
        free(server);
        return NULL;
    }
    
    if (protocol == NL_PROTO_TCP || protocol == NL_PROTO_HTTP || protocol == NL_PROTO_WEBSOCKET) {
        server->fd = socket(AF_INET, SOCK_STREAM, 0);
    } else {
        server->fd = socket(AF_INET, SOCK_DGRAM, 0);
    }
    
    if (server->fd == -1) {
        close(server->kqueue_fd);
        free(server);
        return NULL;
    }
    
    set_reuseaddr(server->fd);
    set_nonblocking(server->fd);
    
    if (protocol == NL_PROTO_TCP || protocol == NL_PROTO_HTTP || protocol == NL_PROTO_WEBSOCKET) {
        set_tcp_nodelay(server->fd, 1);
        set_tcp_keepalive(server->fd, 1, 7200, 75, 9);
    }
    
    set_buffer_sizes(server->fd, 262144, 262144);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(server->fd);
        close(server->kqueue_fd);
        free(server);
        return NULL;
    }
    
    macos_log(NL_LOG_INFO, "macOS: Server created on port %d (kqueue)", port);
    return server;
}

void nl_server_destroy(nl_server_t* server) {
    if (!server) return;
    if (server->fd != -1) close(server->fd);
    if (server->kqueue_fd != -1) close(server->kqueue_fd);
    free(server);
}

int nl_server_start(nl_server_t* server) {
    if (!server) return NL_EINVAL;
    
    if (server->protocol == NL_PROTO_TCP || server->protocol == NL_PROTO_HTTP || 
        server->protocol == NL_PROTO_WEBSOCKET) {
        if (listen(server->fd, SOMAXCONN) == -1) {
            macos_log(NL_LOG_ERROR, "macOS: listen() failed: %s", strerror(errno));
            return NL_ERROR;
        }
    }
    
    struct kevent ev;
    EV_SET(&ev, server->fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    
    if (kevent(server->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
        macos_log(NL_LOG_ERROR, "macOS: kevent() failed: %s", strerror(errno));
        return NL_ERROR;
    }
    
    server->running = 1;
    if (pthread_create(&server->thread_id, NULL, server_thread, server) != 0) {
        server->running = 0;
        return NL_ERROR;
    }
    
    macos_log(NL_LOG_INFO, "macOS: Server started (kqueue), fd=%d", server->fd);
    return NL_OK;
}

void nl_server_stop(nl_server_t* server) {
    if (!server) return;
    server->running = 0;
    if (server->fd != -1) close(server->fd);
    if (server->thread_id) {
        pthread_join(server->thread_id, NULL);
    }
    macos_log(NL_LOG_INFO, "macOS: Server stopped");
}

void nl_server_set_handler(nl_server_t* server, nl_request_handler handler, void* user_data) {
    if (!server) return;
    server->handler = handler;
    server->user_data = user_data;
}

void nl_server_set_udp_handler(nl_server_t* server, nl_udp_message_handler handler, void* user_data) {
    if (!server) return;
    server->udp_handler = handler;
    server->user_data = user_data;
}

int nl_server_set_option(nl_server_t* server, nl_socket_option_t option, int value) {
    if (!server || server->fd == -1) return NL_EINVAL;
    
    int result = 0;
    
    switch (option) {
        case NL_OPT_TCP_NODELAY:
            result = set_tcp_nodelay(server->fd, value);
            break;
        case NL_OPT_TCP_KEEPALIVE:
            result = set_tcp_keepalive(server->fd, value, 0, 0, 0);
            break;
        case NL_OPT_SO_SNDBUF:
            result = setsockopt(server->fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value));
            break;
        case NL_OPT_SO_RCVBUF:
            result = setsockopt(server->fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
            break;
        case NL_OPT_SO_REUSEADDR:
            result = set_reuseaddr(server->fd);
            break;
        case NL_OPT_SO_REUSEPORT:
            result = set_reuseport(server->fd);
            break;
        case NL_OPT_SO_BROADCAST:
            result = set_broadcast(server->fd, value);
            break;
        default:
            return NL_ENOTSUPPORTED;
    }
    
    return (result == 0) ? NL_OK : NL_ERROR;
}

int nl_server_get_option(nl_server_t* server, nl_socket_option_t option, int* value) {
    if (!server || server->fd == -1 || !value) return NL_EINVAL;
    
    socklen_t len = sizeof(*value);
    
    switch (option) {
        case NL_OPT_TCP_NODELAY:
            if (getsockopt(server->fd, IPPROTO_TCP, TCP_NODELAY, value, &len) == -1) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_SO_SNDBUF:
            if (getsockopt(server->fd, SOL_SOCKET, SO_SNDBUF, value, &len) == -1) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_SO_RCVBUF:
            if (getsockopt(server->fd, SOL_SOCKET, SO_RCVBUF, value, &len) == -1) {
                return NL_ERROR;
            }
            break;
        default:
            return NL_ENOTSUPPORTED;
    }
    
    return NL_OK;
}

int nl_server_get_fd(nl_server_t* server) {
    return (server && server->fd != -1) ? server->fd : -1;
}

nl_client_t* nl_client_create(nl_protocol_t protocol) {
    nl_client_t* client = calloc(1, sizeof(nl_client_t));
    if (!client) return NULL;
    
    client->protocol = protocol;
    
    if (protocol == NL_PROTO_TCP || protocol == NL_PROTO_HTTP || protocol == NL_PROTO_WEBSOCKET) {
        client->fd = socket(AF_INET, SOCK_STREAM, 0);
    } else {
        client->fd = socket(AF_INET, SOCK_DGRAM, 0);
    }
    
    if (client->fd == -1) {
        free(client);
        return NULL;
    }
    
    if (protocol == NL_PROTO_TCP || protocol == NL_PROTO_HTTP || protocol == NL_PROTO_WEBSOCKET) {
        set_tcp_nodelay(client->fd, 1);
    }
    
    set_buffer_sizes(client->fd, 262144, 262144);
    macos_log(NL_LOG_DEBUG, "macOS: Client created (socket=%d)", client->fd);
    return client;
}

void nl_client_destroy(nl_client_t* client) {
    if (!client) return;
    if (client->fd != -1) close(client->fd);
    free(client);
}

int nl_client_connect(nl_client_t* client, const char* host, int port) {
    if (!client || !host) return NL_EINVAL;
    
    memset(&client->addr, 0, sizeof(client->addr));
    client->addr.sin_family = AF_INET;
    client->addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &client->addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host);
        if (!he) return NL_ECONNECT;
        memcpy(&client->addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    if (connect(client->fd, (struct sockaddr*)&client->addr, sizeof(client->addr)) == -1) {
        if (errno != EINPROGRESS) {
            macos_log(NL_LOG_ERROR, "macOS: connect() failed: %s", strerror(errno));
            return NL_ECONNECT;
        }
    }
    
    client->connected = 1;
    macos_log(NL_LOG_INFO, "macOS: Client connected to %s:%d", host, port);
    return NL_OK;
}

void nl_client_disconnect(nl_client_t* client) {
    if (!client) return;
    if (client->fd != -1) close(client->fd);
    client->connected = 0;
}

int nl_client_send(nl_client_t* client, const void* data, size_t len) {
    if (!client || !data) return NL_EINVAL;
    
    ssize_t sent = send(client->fd, data, len, 0);
    if (sent == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return NL_EAGAIN;
        return NL_ERROR;
    }
    
    return (int)sent;
}

int nl_client_recv(nl_client_t* client, void* buf, size_t len) {
    if (!client || !buf) return NL_EINVAL;
    
    ssize_t received = recv(client->fd, buf, len, 0);
    if (received == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return NL_EAGAIN;
        return NL_ERROR;
    }
    if (received == 0) return NL_ECLOSED;
    
    return (int)received;
}

int nl_client_send_to(nl_client_t* client, const char* host, int port, const void* data, size_t len) {
    if (!client || !host || !data) return NL_EINVAL;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host);
        if (!he) return NL_ECONNECT;
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    ssize_t sent = sendto(client->fd, data, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    if (sent == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return NL_EAGAIN;
        return NL_ERROR;
    }
    
    return (int)sent;
}

int nl_client_recv_from(nl_client_t* client, void* buf, size_t len, char* from_addr, size_t addr_len, int* from_port) {
    if (!client || !buf) return NL_EINVAL;
    
    struct sockaddr_in addr;
    socklen_t addr_len_struct = sizeof(addr);
    
    ssize_t received = recvfrom(client->fd, buf, len, 0, (struct sockaddr*)&addr, &addr_len_struct);
    if (received == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return NL_EAGAIN;
        return NL_ERROR;
    }
    
    if (from_addr) {
        inet_ntop(AF_INET, &addr.sin_addr, from_addr, addr_len);
    }
    if (from_port) {
        *from_port = ntohs(addr.sin_port);
    }
    
    return (int)received;
}

int nl_client_set_option(nl_client_t* client, nl_socket_option_t option, int value) {
    if (!client || client->fd == -1) return NL_EINVAL;
    
    int result = 0;
    
    switch (option) {
        case NL_OPT_TCP_NODELAY:
            result = set_tcp_nodelay(client->fd, value);
            break;
        case NL_OPT_SO_SNDBUF:
            result = setsockopt(client->fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value));
            break;
        case NL_OPT_SO_RCVBUF:
            result = setsockopt(client->fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value));
            break;
        case NL_OPT_SO_REUSEADDR:
            result = set_reuseaddr(client->fd);
            break;
        case NL_OPT_SO_REUSEPORT:
            result = set_reuseport(client->fd);
            break;
        case NL_OPT_SO_BROADCAST:
            result = set_broadcast(client->fd, value);
            break;
        default:
            return NL_ENOTSUPPORTED;
    }
    
    return (result == 0) ? NL_OK : NL_ERROR;
}

int nl_client_get_option(nl_client_t* client, nl_socket_option_t option, int* value) {
    if (!client || client->fd == -1 || !value) return NL_EINVAL;
    
    socklen_t len = sizeof(*value);
    
    switch (option) {
        case NL_OPT_TCP_NODELAY:
            if (getsockopt(client->fd, IPPROTO_TCP, TCP_NODELAY, value, &len) == -1) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_SO_SNDBUF:
            if (getsockopt(client->fd, SOL_SOCKET, SO_SNDBUF, value, &len) == -1) {
                return NL_ERROR;
            }
            break;
        case NL_OPT_SO_RCVBUF:
            if (getsockopt(client->fd, SOL_SOCKET, SO_RCVBUF, value, &len) == -1) {
                return NL_ERROR;
            }
            break;
        default:
            return NL_ENOTSUPPORTED;
    }
    
    return NL_OK;
}

int nl_client_get_fd(nl_client_t* client) {
    return (client && client->fd != -1) ? client->fd : -1;
}

nl_config_t* nl_config_create(void) {
    nl_config_t* config = calloc(1, sizeof(nl_config_t));
    if (!config) return NULL;
    pthread_mutex_init(&config->mutex, NULL);
    return config;
}

void nl_config_destroy(nl_config_t* config) {
    if (!config) return;
    pthread_mutex_destroy(&config->mutex);
    free(config);
}

int nl_config_load(nl_config_t* config, const char* path) {
    if (!config || !path) return NL_EINVAL;
    
    FILE* fp = fopen(path, "r");
    if (!fp) return NL_ERROR;
    
    pthread_mutex_lock(&config->mutex);
    config->count = 0;
    
    char line[256];
    while (fgets(line, sizeof(line), fp) && config->count < 100) {
        char* eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            strncpy(config->data + config->count * 128, line, 64);
            strncpy(config->data + config->count * 128 + 64, eq + 1, 64);
            config->count++;
        }
    }
    
    pthread_mutex_unlock(&config->mutex);
    fclose(fp);
    
    macos_log(NL_LOG_INFO, "macOS: Config loaded from %s", path);
    return NL_OK;
}

int nl_config_save(nl_config_t* config, const char* path) {
    if (!config || !path) return NL_EINVAL;
    
    FILE* fp = fopen(path, "w");
    if (!fp) return NL_ERROR;
    
    pthread_mutex_lock(&config->mutex);
    
    for (int i = 0; i < config->count; i++) {
        fprintf(fp, "%s=%s\n", 
                config->data + i * 128,
                config->data + i * 128 + 64);
    }
    
    pthread_mutex_unlock(&config->mutex);
    fclose(fp);
    
    return NL_OK;
}

const char* nl_config_get(nl_config_t* config, const char* key) {
    if (!config || !key) return NULL;
    
    pthread_mutex_lock(&config->mutex);
    
    for (int i = 0; i < config->count; i++) {
        if (strcmp(config->data + i * 128, key) == 0) {
            pthread_mutex_unlock(&config->mutex);
            return config->data + i * 128 + 64;
        }
    }
    
    pthread_mutex_unlock(&config->mutex);
    return NULL;
}

void nl_config_set(nl_config_t* config, const char* key, const char* value) {
    if (!config || !key || !value) return;
    
    pthread_mutex_lock(&config->mutex);
    
    for (int i = 0; i < config->count; i++) {
        if (strcmp(config->data + i * 128, key) == 0) {
            strncpy(config->data + i * 128 + 64, value, 64);
            pthread_mutex_unlock(&config->mutex);
            return;
        }
    }
    
    if (config->count < 100) {
        strncpy(config->data + config->count * 128, key, 64);
        strncpy(config->data + config->count * 128 + 64, value, 64);
        config->count++;
    }
    
    pthread_mutex_unlock(&config->mutex);
}

nl_buffer_t* nl_buffer_create(size_t capacity) {
    nl_buffer_t* buffer = calloc(1, sizeof(nl_buffer_t));
    if (!buffer) return NULL;
    
    buffer->data = malloc(capacity);
    if (!buffer->data) {
        free(buffer);
        return NULL;
    }
    
    buffer->capacity = capacity;
    pthread_mutex_init(&buffer->mutex, NULL);
    return buffer;
}

void nl_buffer_destroy(nl_buffer_t* buffer) {
    if (!buffer) return;
    if (buffer->data) free(buffer->data);
    pthread_mutex_destroy(&buffer->mutex);
    free(buffer);
}

void nl_buffer_clear(nl_buffer_t* buffer) {
    if (!buffer) return;
    pthread_mutex_lock(&buffer->mutex);
    buffer->length = 0;
    pthread_mutex_unlock(&buffer->mutex);
}

size_t nl_buffer_write(nl_buffer_t* buffer, const void* data, size_t len) {
    if (!buffer || !data) return 0;
    
    pthread_mutex_lock(&buffer->mutex);
    
    if (buffer->length + len > buffer->capacity) {
        len = buffer->capacity - buffer->length;
    }
    
    if (len > 0) {
        memcpy(buffer->data + buffer->length, data, len);
        buffer->length += len;
    }
    
    pthread_mutex_unlock(&buffer->mutex);
    return len;
}

size_t nl_buffer_read(nl_buffer_t* buffer, void* data, size_t len) {
    if (!buffer || !data) return 0;
    
    pthread_mutex_lock(&buffer->mutex);
    
    if (len > buffer->length) {
        len = buffer->length;
    }
    
    if (len > 0) {
        memcpy(data, buffer->data, len);
        memmove(buffer->data, buffer->data + len, buffer->length - len);
        buffer->length -= len;
    }
    
    pthread_mutex_unlock(&buffer->mutex);
    return len;
}

size_t nl_buffer_size(nl_buffer_t* buffer) {
    if (!buffer) return 0;
    pthread_mutex_lock(&buffer->mutex);
    size_t size = buffer->length;
    pthread_mutex_unlock(&buffer->mutex);
    return size;
}

void nl_log_set_level(nl_log_level_t level) {
    current_log_level = level;
}

void nl_log_set_callback(nl_log_callback callback, void* user_data) {
    log_callback = callback;
    log_user_data = user_data;
}

const char* nl_version_string(void) {
    return NETLEAF_VERSION;
}

int nl_version_major(void) {
    return NETLEAF_VERSION_MAJOR;
}

int nl_version_minor(void) {
    return NETLEAF_VERSION_MINOR;
}

int nl_version_patch(void) {
    return NETLEAF_VERSION_PATCH;
}

// File Server Implementation
struct nl_file_server {
    char directory[4096];
    char index_file[256];
    int port;
    int sock;
    pthread_t thread;
    volatile int running;
    int enable_easter_egg;
};

struct nl_route {
    char path[256];
    nl_http_method_t method;
    nl_http_handler_t handler;
    void* user_data;
    struct nl_route* next;
};

struct nl_router {
    struct nl_route* routes;
    char static_dir[4096];
    pthread_mutex_t mutex;
};

static void get_file_extension(const char* filename, char* ext, size_t ext_len) {
    const char* dot = strrchr(filename, '.');
    if (dot) {
        strncpy(ext, dot + 1, ext_len - 1);
        ext[ext_len - 1] = '\0';
        for (size_t i = 0; i < strlen(ext); i++) {
            ext[i] = tolower(ext[i]);
        }
    } else {
        ext[0] = '\0';
    }
}

static const char* get_mime_type(const char* ext) {
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) return "text/html";
    if (strcmp(ext, "css") == 0) return "text/css";
    if (strcmp(ext, "js") == 0) return "application/javascript";
    if (strcmp(ext, "json") == 0) return "application/json";
    if (strcmp(ext, "png") == 0) return "image/png";
    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "gif") == 0) return "image/gif";
    if (strcmp(ext, "svg") == 0) return "image/svg+xml";
    if (strcmp(ext, "ico") == 0) return "image/x-icon";
    if (strcmp(ext, "xml") == 0) return "application/xml";
    if (strcmp(ext, "txt") == 0) return "text/plain";
    if (strcmp(ext, "pdf") == 0) return "application/pdf";
    if (strcmp(ext, "zip") == 0) return "application/zip";
    return "application/octet-stream";
}

static char* read_file(const char* filepath, size_t* out_size) {
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    if (size < 0) {
        fclose(fp);
        if (out_size) *out_size = 0;
        return NULL;
    }
    
    char* content = (char*)malloc((size_t)size + 1);
    if (!content) {
        fclose(fp);
        if (out_size) *out_size = 0;
        return NULL;
    }
    
    size_t read = fread(content, 1, (size_t)size, fp);
    fclose(fp);
    content[read] = '\0';
    
    if (out_size) *out_size = read;
    return content;
}

static char* normalize_path(const char* base_dir, const char* req_path, char* result, size_t result_len) {
    char full_path[8192];
    
    if (req_path[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s%s", base_dir, req_path);
    } else {
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, req_path);
    }
    
    strncpy(result, full_path, result_len);
    result[result_len - 1] = '\0';
    return result;
}

static int is_path_safe(const char* base_dir, const char* filepath) {
    char normalized_base[8192];
    char normalized_file[8192];
    
    if (realpath(base_dir, normalized_base) == NULL) return 0;
    if (realpath(filepath, normalized_file) == NULL) return 0;
    
    return strncmp(normalized_file, normalized_base, strlen(normalized_base)) == 0;
}

static int send_http_response(int client, const char* content_type, const char* content, size_t content_len) {
    char header[8192];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        content_type, content_len);
    
    send(client, header, header_len, 0);
    if (content && content_len > 0) {
        send(client, content, content_len, 0);
    }
    
    return 0;
}

static int send_http_error(int client, int status_code, const char* message) {
    char response[8192];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html><body><h1>%d %s</h1></body></html>",
        status_code, message,
        strlen(message) + 32,
        status_code, message);
    
    send(client, response, len, 0);
    return 0;
}

static int send_418_response(int client) {
    const char* teapot_html = 
        "<html><head><title>418 I'm a teapot</title></head><body><h1>418 I'm a teapot</h1></body></html>";
    char response[8192];
    int len = snprintf(response, sizeof(response),
        "HTTP/1.1 418 I'm a teapot\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        strlen(teapot_html),
        teapot_html);
    send(client, response, len, 0);
    return 0;
}

static int is_april_fools_day(void) {
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    return (tm_buf.tm_mon == 3 && tm_buf.tm_mday == 1);
}

static void* file_server_thread(void* arg) {
    nl_file_server_t* server = (nl_file_server_t*)arg;
    
    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client = accept(server->sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client < 0) {
            usleep(100000);
            continue;
        }
        
        char buffer[8192];
        ssize_t received = recv(client, buffer, sizeof(buffer) - 1, 0);
        
        if (received <= 0) {
            close(client);
            continue;
        }
        
        buffer[received] = '\0';
        
        char method[16], path[4096], protocol[32];
        if (sscanf(buffer, "%15s %4095s %31s", method, path, protocol) != 3) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            continue;
        }
        
        if (server->enable_easter_egg && is_april_fools_day()) {
            if (strncmp(path, "/tea", 4) == 0) {
                send_418_response(client);
                close(client);
                continue;
            }
        }
        
        char filepath[8192];
        normalize_path(server->directory, path, filepath, sizeof(filepath));
        
        if (!is_path_safe(server->directory, filepath)) {
            send_http_error(client, 403, "Forbidden");
            close(client);
            continue;
        }
        
        struct stat path_stat;
        if (stat(filepath, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
            char index_path[8192];
            snprintf(index_path, sizeof(index_path), "%s/%s", filepath, server->index_file);
            if (stat(index_path, &path_stat) == 0) {
                strncpy(filepath, index_path, sizeof(filepath));
            } else {
                send_http_error(client, 404, "Not Found");
                close(client);
                continue;
            }
        }
        
        size_t file_size;
        char* file_content = read_file(filepath, &file_size);
        
        if (!file_content) {
            send_http_error(client, 404, "Not Found");
            close(client);
            continue;
        }
        
        char ext[32];
        get_file_extension(filepath, ext, sizeof(ext));
        send_http_response(client, get_mime_type(ext), file_content, file_size);
        
        free(file_content);
        close(client);
    }
    
    return NULL;
}

nl_file_server_t* nl_file_server_create(const char* directory, int port) {
    if (!directory) return NULL;
    
    nl_file_server_t* server = (nl_file_server_t*)calloc(1, sizeof(nl_file_server_t));
    if (!server) return NULL;
    
    char full_dir[4096];
    if (realpath(directory, full_dir)) {
        strncpy(server->directory, full_dir, sizeof(server->directory));
    } else {
        strncpy(server->directory, directory, sizeof(server->directory));
    }
    server->directory[sizeof(server->directory) - 1] = '\0';
    
    strncpy(server->index_file, "index.html", sizeof(server->index_file));
    server->port = port;
    server->running = 0;
    
    return server;
}

void nl_file_server_destroy(nl_file_server_t* server) {
    if (!server) return;
    if (server->running) nl_file_server_stop(server);
    free(server);
}

int nl_file_server_start(nl_file_server_t* server) {
    if (!server) return -1;
    if (server->running) return 0;
    
    server->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server->sock < 0) return -1;
    
    int opt = 1;
    setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)server->port);
    
    if (bind(server->sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->sock);
        return -1;
    }
    
    if (listen(server->sock, SOMAXCONN) < 0) {
        close(server->sock);
        return -1;
    }
    
    server->running = 1;
    pthread_create(&server->thread, NULL, file_server_thread, server);
    
    printf("File server started on port %d, serving %s\n", server->port, server->directory);
    return 0;
}

void nl_file_server_stop(nl_file_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    pthread_join(server->thread, NULL);
    if (server->sock >= 0) {
        close(server->sock);
        server->sock = -1;
    }
}

void nl_file_server_set_index(nl_file_server_t* server, const char* index_file) {
    if (!server || !index_file) return;
    strncpy(server->index_file, index_file, sizeof(server->index_file) - 1);
}

void nl_file_server_set_easter_egg(nl_file_server_t* server, int enable) {
    if (!server) return;
    server->enable_easter_egg = enable;
}

static nl_file_server_t* g_simple_server = NULL;

int nl_serve_files(const char* directory, int port) {
    if (g_simple_server) {
        nl_file_server_stop(g_simple_server);
        nl_file_server_destroy(g_simple_server);
    }
    g_simple_server = nl_file_server_create(directory, port);
    if (!g_simple_server) return -1;
    return nl_file_server_start(g_simple_server);
}

// Router Implementation
nl_router_t* nl_router_create(void) {
    nl_router_t* router = (nl_router_t*)calloc(1, sizeof(nl_router_t));
    if (!router) return NULL;
    pthread_mutex_init(&router->mutex, NULL);
    return router;
}

void nl_router_destroy(nl_router_t* router) {
    if (!router) return;
    pthread_mutex_lock(&router->mutex);
    struct nl_route* route = router->routes;
    while (route) {
        struct nl_route* next = route->next;
        free(route);
        route = next;
    }
    pthread_mutex_unlock(&router->mutex);
    pthread_mutex_destroy(&router->mutex);
    free(router);
}

void nl_router_add_route(nl_router_t* router, const char* path, nl_http_method_t method, nl_http_handler_t handler, void* user_data) {
    if (!router || !path || !handler) return;
    struct nl_route* route = (struct nl_route*)calloc(1, sizeof(struct nl_route));
    if (!route) return;
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->method = method;
    route->handler = handler;
    route->user_data = user_data;
    pthread_mutex_lock(&router->mutex);
    route->next = router->routes;
    router->routes = route;
    pthread_mutex_unlock(&router->mutex);
}

void nl_router_set_static_dir(nl_router_t* router, const char* directory) {
    if (!router) return;
    pthread_mutex_lock(&router->mutex);
    if (directory) {
        char full_dir[4096];
        if (realpath(directory, full_dir)) {
            strncpy(router->static_dir, full_dir, sizeof(router->static_dir));
        } else {
            strncpy(router->static_dir, directory, sizeof(router->static_dir));
        }
    }
    pthread_mutex_unlock(&router->mutex);
}

static struct nl_route* router_find_route(nl_router_t* router, const char* path, nl_http_method_t method) {
    pthread_mutex_lock(&router->mutex);
    struct nl_route* route = router->routes;
    while (route) {
        if (route->method == method && strcmp(route->path, path) == 0) {
            pthread_mutex_unlock(&router->mutex);
            return route;
        }
        route = route->next;
    }
    pthread_mutex_unlock(&router->mutex);
    return NULL;
}

typedef struct {
    nl_router_t* router;
    int port;
    int sock;
    pthread_t thread;
    volatile int running;
} router_server_t;

static void* router_server_thread(void* arg) {
    router_server_t* rs = (router_server_t*)arg;
    
    while (rs->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(rs->sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client < 0) {
            usleep(100000);
            continue;
        }
        
        char buffer[8192];
        ssize_t received = recv(client, buffer, sizeof(buffer) - 1, 0);
        
        if (received <= 0) {
            close(client);
            continue;
        }
        
        buffer[received] = '\0';
        
        char method_str[16], path[4096], protocol[32];
        if (sscanf(buffer, "%15s %4095s %31s", method_str, path, protocol) != 3) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            continue;
        }
        
        nl_http_method_t method = NL_METHOD_GET;
        if (strcmp(method_str, "POST") == 0) method = NL_METHOD_POST;
        else if (strcmp(method_str, "PUT") == 0) method = NL_METHOD_PUT;
        else if (strcmp(method_str, "DELETE") == 0) method = NL_METHOD_DELETE;
        else if (strcmp(method_str, "PATCH") == 0) method = NL_METHOD_PATCH;
        
        char* body_start = strstr(buffer, "\r\n\r\n");
        const char* body = NULL;
        size_t body_size = 0;
        if (body_start) {
            body = body_start + 4;
            body_size = (size_t)(received - (body - buffer));
        }
        
        struct nl_route* route = router_find_route(rs->router, path, method);
        if (route) {
            char* response = NULL;
            size_t response_size = 0;
            route->handler(path, method, body, body_size, &response, &response_size, route->user_data);
            if (response) {
                send_http_response(client, "application/json", response, response_size);
                free(response);
            } else {
                send_http_error(client, 500, "Internal Server Error");
            }
        } else {
            send_http_error(client, 404, "Not Found");
        }
        close(client);
    }
    return NULL;
}

static router_server_t* g_router_server = NULL;

int nl_router_serve(nl_router_t* router, int port) {
    if (!router) return -1;
    
    if (g_router_server) {
        g_router_server->running = 0;
        if (g_router_server->thread) pthread_join(g_router_server->thread, NULL);
        if (g_router_server->sock >= 0) close(g_router_server->sock);
        free(g_router_server);
    }
    
    router_server_t* rs = (router_server_t*)calloc(1, sizeof(router_server_t));
    if (!rs) return -1;
    
    rs->router = router;
    rs->port = port;
    rs->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (rs->sock < 0) {
        free(rs);
        return -1;
    }
    
    int opt = 1;
    setsockopt(rs->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);
    
    if (bind(rs->sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(rs->sock);
        free(rs);
        return -1;
    }
    
    if (listen(rs->sock, SOMAXCONN) < 0) {
        close(rs->sock);
        free(rs);
        return -1;
    }
    
    rs->running = 1;
    pthread_create(&rs->thread, NULL, router_server_thread, rs);
    g_router_server = rs;
    printf("Router server started on port %d\n", port);
    return 0;
}

static nl_http_handler_t g_default_handler = NULL;
static void* g_default_handler_data = NULL;

static void default_server_handler(const char* path, nl_http_method_t method,
                                  const char* body, size_t body_size,
                                  char** response, size_t* response_size,
                                  void* user_data) {
    (void)user_data;
    if (g_default_handler) {
        g_default_handler(path, method, body, body_size, response, response_size, g_default_handler_data);
    } else {
        const char* default_resp = "{\"message\":\"NetLeaf v2.0.0\"}";
        *response_size = strlen(default_resp);
        *response = (char*)malloc(*response_size + 1);
        if (*response) strcpy(*response, default_resp);
    }
}

int nl_serve(int port, nl_http_handler_t default_handler, void* user_data) {
    g_default_handler = default_handler;
    g_default_handler_data = user_data;
    nl_router_t* router = nl_router_create();
    if (!router) return -1;
    nl_router_add_route(router, "/", NL_METHOD_GET, default_server_handler, NULL);
    return nl_router_serve(router, port);
}

// Web Server Implementation
// Route type enumeration
typedef enum {
    NL_ROUTE_TYPE_CONTENT = 0,    // Static content (html/vue/json)
    NL_ROUTE_TYPE_FILE = 1,       // File-based (hot reload)
    NL_ROUTE_TYPE_REDIRECT = 2    // HTTP 302 redirect
} nl_route_type_t;

typedef struct nl_web_route {
    char path[256];
    char* content;
    size_t content_size;
    char content_type[64];
    nl_route_type_t type;         // Route type
    char file_path[512];          // File path for hot reload
    char redirect_url[512];       // Redirect URL for 302
    struct nl_web_route* next;
} nl_web_route_t;

struct nl_web_server {
    int port;
    int sock;
    pthread_t thread;
    volatile int running;
    nl_web_route_t* routes;
    pthread_mutex_t mutex;
    char encoding[32];
    struct nl_web_server* next;
    int auto_encoding_enabled;
    char fallback_encoding[32];
    int error_suggestions_enabled;
    char error_page_templates[8][256];
};

static struct nl_web_server* g_web_servers = NULL;
static pthread_mutex_t g_web_servers_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_auto_cleanup_enabled = 0;

static const char* nl_responsive_css = 
    "<style>"
    "* { margin:0; padding:0; box-sizing:border-box; }"
    "body { font-family:system-ui,sans-serif; background:linear-gradient(135deg,#667eea,#764ba2); min-height:100vh; padding:20px; }"
    ".container { max-width:800px; margin:0 auto; background:#fff; border-radius:16px; box-shadow:0 20px 60px rgba(0,0,0,0.3); padding:40px; }"
    "h1 { color:#2d3748; margin-bottom:24px; }"
    ".btn { background:linear-gradient(135deg,#667eea,#764ba2); color:white; border:none; padding:14px 28px; border-radius:8px; cursor:pointer; margin:8px; }"
    ".counter { font-size:4rem; font-weight:800; color:#667eea; text-align:center; margin:24px 0; }"
    ".card { background:#f7fafc; border-radius:12px; padding:24px; margin:16px 0; border-left:4px solid #667eea; }"
    ".grid { display:grid; grid-template-columns:repeat(auto-fit, minmax(200px,1fr)); gap:16px; }"
    ".stat { background:white; padding:24px; border-radius:12px; text-align:center; box-shadow:0 4px 12px rgba(0,0,0,0.1); }"
    ".stat-value { font-size:2.5rem; font-weight:800; color:#667eea; }"
    ".stat-label { color:#718096; font-size:0.9rem; margin-top:8px; }"
    "</style>";

static const char* nl_vue_cdn = "<script src=\"https://unpkg.com/vue@3/dist/vue.global.js\"></script>";

static void* web_server_thread(void* arg) {
    nl_web_server_t* server = (nl_web_server_t*)arg;
    
    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(server->sock, (struct sockaddr*)&client_addr, &client_len);
        
        if (client < 0) {
            usleep(10000);
            continue;
        }
        
        char buffer[8192];
        ssize_t received = recv(client, buffer, sizeof(buffer) - 1, 0);
        
        if (received <= 0) {
            close(client);
            continue;
        }
        
        buffer[received] = '\0';
        
        char method[16], path[4096], protocol[32];
        if (sscanf(buffer, "%15s %4095s %31s", method, path, protocol) != 3) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            continue;
        }
        
        pthread_mutex_lock(&server->mutex);
        nl_web_route_t* route = server->routes;
        while (route) {
            if (strcmp(route->path, path) == 0) {
                // Handle different route types
                if (route->type == NL_ROUTE_TYPE_REDIRECT) {
                    // Send 302 redirect
                    pthread_mutex_unlock(&server->mutex);
                    char redirect_response[1024];
                    snprintf(redirect_response, sizeof(redirect_response),
                        "HTTP/1.1 302 Found\r\n"
                        "Location: %s\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n"
                        "\r\n",
                        route->redirect_url);
                    (void)write(client, redirect_response, strlen(redirect_response));
                    close(client);
                    goto next_conn;
                }
                else if (route->type == NL_ROUTE_TYPE_FILE) {
                    // Hot reload: read file content on each request
                    pthread_mutex_unlock(&server->mutex);
                    
                    FILE* fp = fopen(route->file_path, "rb");
                    if (!fp) {
                        send_http_error(client, 404, "File Not Found");
                        close(client);
                        goto next_conn;
                    }
                    
                    fseek(fp, 0, SEEK_END);
                    long file_size = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    
                    if (file_size <= 0 || file_size > 10 * 1024 * 1024) {
                        fclose(fp);
                        send_http_error(client, 500, "File Too Large");
                        close(client);
                        goto next_conn;
                    }
                    
                    char* file_content = (char*)malloc(file_size + 1);
                    if (!file_content) {
                        fclose(fp);
                        send_http_error(client, 500, "Memory Error");
                        close(client);
                        goto next_conn;
                    }
                    
                    size_t read_size = fread(file_content, 1, file_size, fp);
                    fclose(fp);
                    file_content[read_size] = '\0';
                    
                    send_http_response(client, route->content_type, file_content, read_size);
                    free(file_content);
                    close(client);
                    goto next_conn;
                }
                else {
                    pthread_mutex_unlock(&server->mutex);
                    send_http_response(client, route->content_type, route->content, route->content_size);
                    close(client);
                    goto next_conn;
                }
            }
            route = route->next;
        }
        pthread_mutex_unlock(&server->mutex);
        send_http_error(client, 404, "Not Found");
        close(client);
    next_conn:;
    }
    return NULL;
}

nl_web_server_t* nl_web_create(int port) {
    pthread_mutex_lock(&g_web_servers_mutex);
    struct nl_web_server* existing = g_web_servers;
    while (existing) {
        if (existing->port == port) {
            pthread_mutex_unlock(&g_web_servers_mutex);
            return existing;
        }
        existing = existing->next;
    }
    
    nl_web_server_t* server = (nl_web_server_t*)calloc(1, sizeof(nl_web_server_t));
    if (!server) {
        pthread_mutex_unlock(&g_web_servers_mutex);
        return NULL;
    }
    
    server->port = port;
    server->routes = NULL;
    pthread_mutex_init(&server->mutex, NULL);
    strncpy(server->encoding, "UTF-8", sizeof(server->encoding) - 1);
    server->next = g_web_servers;
    g_web_servers = server;
    pthread_mutex_unlock(&g_web_servers_mutex);
    
    int result = nl_web_start(server);
    if (result != 0) {
        nl_web_destroy(server);
        return NULL;
    }
    return server;
}

void nl_web_destroy(nl_web_server_t* server) {
    if (!server) return;
    
    pthread_mutex_lock(&g_web_servers_mutex);
    struct nl_web_server** pp = &g_web_servers;
    while (*pp) {
        if (*pp == server) {
            *pp = server->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_web_servers_mutex);
    
    if (server->running) nl_web_stop(server);
    
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = server->routes;
    while (route) {
        nl_web_route_t* next = route->next;
        if (route->content) free(route->content);
        free(route);
        route = next;
    }
    pthread_mutex_unlock(&server->mutex);
    pthread_mutex_destroy(&server->mutex);
    free(server);
}

int nl_web_start(nl_web_server_t* server) {
    if (!server || server->running) return -1;
    
    server->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server->sock < 0) return -1;
    
    int opt = 1;
    setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)server->port);
    
    if (bind(server->sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server->sock);
        return -1;
    }
    
    if (listen(server->sock, SOMAXCONN) < 0) {
        close(server->sock);
        return -1;
    }
    
    server->running = 1;
    pthread_create(&server->thread, NULL, web_server_thread, server);
    printf("Web server started on port %d\n", server->port);
    return 0;
}

void nl_web_stop(nl_web_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    if (server->thread) pthread_join(server->thread, NULL);
    if (server->sock >= 0) {
        close(server->sock);
        server->sock = -1;
    }
}

static void add_web_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type) {
    if (!server || !path || !content) return;
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (route) {
        strncpy(route->path, path, sizeof(route->path) - 1);
        route->content_size = strlen(content);
        route->content = (char*)malloc(route->content_size + 1);
        if (!route->content) {
            free(route);
            pthread_mutex_unlock(&server->mutex);
            return;
        }
        strcpy(route->content, content);
        strncpy(route->content_type, content_type, sizeof(route->content_type) - 1);
        route->type = NL_ROUTE_TYPE_CONTENT;  // Default: static content
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
    }
    pthread_mutex_unlock(&server->mutex);
}

void nl_web_add_html(nl_web_server_t* server, const char* path, const char* html) {
    add_web_route(server, path, html, "text/html");
}

void nl_web_add_vue(nl_web_server_t* server, const char* path, const char* vue_code) {
    char full_html[32768];
    snprintf(full_html, sizeof(full_html),
        "<!DOCTYPE html><html><head><title>NetLeaf Vue</title>%s%s</head><body><div id=\"app\">%s</div><script>const {createApp,ref,reactive}=Vue;createApp({setup(){return{}}}).mount('#app');</script></body></html>",
        nl_responsive_css, nl_vue_cdn, vue_code);
    add_web_route(server, path, full_html, "text/html");
}

void nl_web_set_encoding(nl_web_server_t* server, const char* encoding) {
    if (!server || !encoding) return;
    strncpy(server->encoding, encoding, sizeof(server->encoding) - 1);
}

static char* substitute_variables(const char* template, const char** vars, const char** values, int count) {
    if (!template || !vars || !values || count <= 0) {
        char* result = (char*)malloc(strlen(template) + 1);
        if (result) strcpy(result, template);
        return result;
    }
    size_t result_size = strlen(template) * 2;
    char* result = (char*)malloc(result_size);
    if (!result) return NULL;
    char* ptr = result;
    const char* src = template;
    *ptr = '\0';
    while (*src) {
        if (strncmp(src, "{{<var>", 7) == 0) {
            const char* var_start = src + 7;
            const char* var_end = strstr(var_start, "</var>}}");
            if (var_end) {
                size_t var_len = var_end - var_start;
                char var_name[256];
                strncpy(var_name, var_start, var_len);
                var_name[var_len] = '\0';
                const char* replacement = NULL;
                for (int i = 0; i < count; i++) {
                    if (strcmp(vars[i], var_name) == 0) {
                        replacement = values[i];
                        break;
                    }
                }
                if (replacement) {
                    size_t rep_len = strlen(replacement);
                    size_t current_len = strlen(result);
                    if (current_len + rep_len + 1 > result_size) {
                        result_size *= 2;
                        char* new_result = (char*)realloc(result, result_size);
                        if (!new_result) { free(result); return NULL; }
                        result = new_result;
                        ptr = result + current_len;
                    }
                    strcpy(ptr, replacement);
                    ptr += rep_len;
                }
                src = var_end + 8;
                continue;
            }
        }
        *ptr++ = *src++;
        *ptr = '\0';
    }
    return result;
}

void nl_web_add_html_with_vars(nl_web_server_t* server, const char* path, const char* html, const char** vars, const char** values, int count) {
    if (!server || !path || !html) return;
    char* substituted = substitute_variables(html, vars, values, count);
    if (substituted) {
        add_web_route(server, path, substituted, "text/html");
        free(substituted);
    }
}

void nl_web_add_vue_with_vars(nl_web_server_t* server, const char* path, const char* vue_code, const char** vars, const char** values, int count) {
    if (!server || !path || !vue_code) return;
    char* substituted = substitute_variables(vue_code, vars, values, count);
    if (substituted) {
        char full_html[32768];
        snprintf(full_html, sizeof(full_html),
            "<!DOCTYPE html><html><head><title>NetLeaf Vue</title>%s%s</head><body><div id=\"app\">%s</div><script>const {createApp,ref,reactive}=Vue;createApp({setup(){return{}}}).mount('#app');</script></body></html>",
            nl_responsive_css, nl_vue_cdn, substituted);
        add_web_route(server, path, full_html, "text/html");
        free(substituted);
    }
}

void nl_web_add_json(nl_web_server_t* server, const char* path, const char* json) {
    add_web_route(server, path, json, "application/json");
}

// Add HTML from external file (hot reload support)
int nl_web_add_html_file(nl_web_server_t* server, const char* path, const char* file_path) {
    if (!server || !path || !file_path) return NL_EINVAL;
    
    char abs_path[512];
    if (realpath(file_path, abs_path) == NULL) {
        strncpy(abs_path, file_path, sizeof(abs_path) - 1);
    }
    abs_path[sizeof(abs_path) - 1] = '\0';
    
    FILE* fp = fopen(abs_path, "rb");
    if (!fp) {
        printf("Error: File not found: %s\n", abs_path);
        return NL_EFILE;
    }
    fclose(fp);
    
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (!route) {
        pthread_mutex_unlock(&server->mutex);
        return NL_ENOMEM;
    }
    
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->path[sizeof(route->path) - 1] = '\0';
    strncpy(route->file_path, abs_path, sizeof(route->file_path) - 1);
    route->file_path[sizeof(route->file_path) - 1] = '\0';
    strncpy(route->content_type, "text/html", sizeof(route->content_type) - 1);
    route->type = NL_ROUTE_TYPE_FILE;
    route->content = NULL;
    route->content_size = 0;
    route->redirect_url[0] = '\0';
    route->next = server->routes;
    server->routes = route;
    pthread_mutex_unlock(&server->mutex);
    
    printf("Added HTML file route: %s -> %s (hot reload)\n", path, abs_path);
    return NL_OK;
}

int nl_web_add_vue_file(nl_web_server_t* server, const char* path, const char* file_path) {
    if (!server || !path || !file_path) return NL_EINVAL;
    
    char abs_path[512];
    if (realpath(file_path, abs_path) == NULL) {
        strncpy(abs_path, file_path, sizeof(abs_path) - 1);
    }
    abs_path[sizeof(abs_path) - 1] = '\0';
    
    FILE* fp = fopen(abs_path, "rb");
    if (!fp) {
        printf("Error: File not found: %s\n", abs_path);
        return NL_EFILE;
    }
    fclose(fp);
    
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (!route) {
        pthread_mutex_unlock(&server->mutex);
        return NL_ENOMEM;
    }
    
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->path[sizeof(route->path) - 1] = '\0';
    strncpy(route->file_path, abs_path, sizeof(route->file_path) - 1);
    route->file_path[sizeof(route->file_path) - 1] = '\0';
    strncpy(route->content_type, "text/html", sizeof(route->content_type) - 1);
    route->type = NL_ROUTE_TYPE_FILE;
    route->content = NULL;
    route->content_size = 0;
    route->redirect_url[0] = '\0';
    route->next = server->routes;
    server->routes = route;
    pthread_mutex_unlock(&server->mutex);
    
    printf("Added Vue file route: %s -> %s (hot reload)\n", path, abs_path);
    return NL_OK;
}

int nl_web_add_json_file(nl_web_server_t* server, const char* path, const char* file_path) {
    if (!server || !path || !file_path) return NL_EINVAL;
    
    char abs_path[512];
    if (realpath(file_path, abs_path) == NULL) {
        strncpy(abs_path, file_path, sizeof(abs_path) - 1);
    }
    abs_path[sizeof(abs_path) - 1] = '\0';
    
    FILE* fp = fopen(abs_path, "rb");
    if (!fp) {
        printf("Error: File not found: %s\n", abs_path);
        return NL_EFILE;
    }
    fclose(fp);
    
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (!route) {
        pthread_mutex_unlock(&server->mutex);
        return NL_ENOMEM;
    }
    
    strncpy(route->path, path, sizeof(route->path) - 1);
    route->path[sizeof(route->path) - 1] = '\0';
    strncpy(route->file_path, abs_path, sizeof(route->file_path) - 1);
    route->file_path[sizeof(route->file_path) - 1] = '\0';
    strncpy(route->content_type, "application/json", sizeof(route->content_type) - 1);
    route->type = NL_ROUTE_TYPE_FILE;
    route->content = NULL;
    route->content_size = 0;
    route->redirect_url[0] = '\0';
    route->next = server->routes;
    server->routes = route;
    pthread_mutex_unlock(&server->mutex);
    
    printf("Added JSON file route: %s -> %s (hot reload)\n", path, abs_path);
    return NL_OK;
}

void nl_web_add_redirect(nl_web_server_t* server, const char* path, const char* target_url) {
    nl_web_add_redirect_302(server, path, target_url);
}

void nl_web_add_redirect_302(nl_web_server_t* server, const char* path, const char* target_url) {
    if (!server || !path || !target_url) return;
    
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (route) {
        strncpy(route->path, path, sizeof(route->path) - 1);
        route->path[sizeof(route->path) - 1] = '\0';
        strncpy(route->redirect_url, target_url, sizeof(route->redirect_url) - 1);
        route->redirect_url[sizeof(route->redirect_url) - 1] = '\0';
        strncpy(route->content_type, "text/html", sizeof(route->content_type) - 1);
        route->type = NL_ROUTE_TYPE_REDIRECT;
        route->content = NULL;
        route->content_size = 0;
        route->file_path[0] = '\0';
        route->next = server->routes;
        server->routes = route;
    }
    pthread_mutex_unlock(&server->mutex);
    
    printf("Added redirect: %s -> %s (302)\n", path, target_url);
}

void nl_web_stop_by_port(int port) {
    pthread_mutex_lock(&g_web_servers_mutex);
    struct nl_web_server* server = g_web_servers;
    while (server) {
        if (server->port == port) {
            pthread_mutex_unlock(&g_web_servers_mutex);
            nl_web_destroy(server);
            return;
        }
        server = server->next;
    }
    pthread_mutex_unlock(&g_web_servers_mutex);
}

static void cleanup_all_web_servers(void) {
    pthread_mutex_lock(&g_web_servers_mutex);
    while (g_web_servers) {
        struct nl_web_server* server = g_web_servers;
        g_web_servers = server->next;
        pthread_mutex_unlock(&g_web_servers_mutex);
        if (server->running) nl_web_stop(server);
        pthread_mutex_lock(&server->mutex);
        nl_web_route_t* route = server->routes;
        while (route) {
            nl_web_route_t* next = route->next;
            if (route->content) free(route->content);
            free(route);
            route = next;
        }
        pthread_mutex_unlock(&server->mutex);
        pthread_mutex_destroy(&server->mutex);
        free(server);
        pthread_mutex_lock(&g_web_servers_mutex);
    }
    pthread_mutex_unlock(&g_web_servers_mutex);
}

void nl_web_set_auto_cleanup(int enable) {
    if (enable && !g_auto_cleanup_enabled) {
        g_auto_cleanup_enabled = 1;
        atexit(cleanup_all_web_servers);
    }
    g_auto_cleanup_enabled = enable;
}

void nl_web_add_counter(nl_web_server_t* server, const char* path, const char* title) {
    char html[32768];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>%s</title>%s%s</head><body><div class=\"container\" id=\"app\"><h1>{{title}}</h1><div class=\"counter\">{{count}}</div><button class=\"btn\" @click=\"count++\">+</button><button class=\"btn\" @click=\"count--\">-</button></div><script>const {createApp,ref}=Vue;createApp({setup(){return{count:ref(0),title:ref('%s')}}}).mount('#app');</script></body></html>",
        title, nl_responsive_css, nl_vue_cdn, title);
    add_web_route(server, path, html, "text/html");
}

void nl_web_add_dashboard(nl_web_server_t* server, const char* path, const char* title) {
    char html[65536];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>%s</title>%s%s</head><body><div class=\"container\" id=\"app\"><h1>{{title}}</h1><div class=\"grid\"><div class=\"stat\"><div class=\"stat-value\">{{stats.users}}</div><div class=\"stat-label\">Users</div></div><div class=\"stat\"><div class=\"stat-value\">{{stats.orders}}</div><div class=\"stat-label\">Orders</div></div></div></div><script>const {createApp,ref,reactive}=Vue;createApp({setup(){return{title:ref('%s'),stats:reactive({users:0,orders:0})}}}).mount('#app');</script></body></html>",
        title, nl_responsive_css, nl_vue_cdn, title);
    add_web_route(server, path, html, "text/html");
}

void nl_web_add_form(nl_web_server_t* server, const char* path, const char* title, const char** fields, int field_count) {
    char html[32768] = "";
    for (int i = 0; i < field_count && i < 10; i++) {
        char field[512];
        snprintf(field, sizeof(field), "<div><label>%s</label><input v-model=\"form.%s\" /></div>", fields[i], fields[i]);
        strncat(html, field, sizeof(html) - strlen(html) - 1);
    }
    char full_html[32768];
    snprintf(full_html, sizeof(full_html),
        "<!DOCTYPE html><html><head><title>%s</title>%s%s</head><body><div class=\"container\" id=\"app\"><h1>{{title}}</h1>%s<button class=\"btn\" @click=\"submit\">Submit</button></div><script>const {createApp,ref,reactive}=Vue;createApp({setup(){return{title:ref('%s'),form:reactive({})}}}).mount('#app');</script></body></html>",
        title, nl_responsive_css, nl_vue_cdn, html, title);
    add_web_route(server, path, full_html, "text/html");
}

int nl_serve_html(int port, const char* html) {
    nl_web_server_t* server = nl_web_create(port);
    if (!server) return -1;
    nl_web_add_html(server, "/", html);
    return nl_web_start(server);
}

int nl_serve_vue(int port, const char* vue_code) {
    nl_web_server_t* server = nl_web_create(port);
    if (!server) return -1;
    nl_web_add_vue(server, "/", vue_code);
    return nl_web_start(server);
}

int nl_serve_dashboard(int port, const char* title) {
    nl_web_server_t* server = nl_web_create(port);
    if (!server) return -1;
    nl_web_add_dashboard(server, "/", title);
    return nl_web_start(server);
}

void nl_web_add_todo(nl_web_server_t* server, const char* path, const char* title) {
    char html[65536];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>%s</title>%s%s</head><body><div class=\"container\" id=\"app\"><h1>{{title}}</h1><input v-model=\"newTodo\" @keyup.enter=\"addTodo\" placeholder=\"Add task...\" /><button class=\"btn\" @click=\"addTodo\">Add</button><div v-for=\"(todo,i) in todos\" :key=\"i\" class=\"card\"><span>{{todo.text}}</span><button @click=\"remove(i)\">X</button></div></div><script>const {createApp,ref}=Vue;createApp({setup(){return{title:ref('%s'),newTodo:ref(''),todos:ref([{text:'Task 1',done:false}])}}}).mount('#app');</script></body></html>",
        title, nl_responsive_css, nl_vue_cdn, title);
    add_web_route(server, path, html, "text/html");
}

void nl_web_add_chat(nl_web_server_t* server, const char* path, const char* title) {
    char html[65536];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>%s</title>%s%s<style>.msg{padding:8px;margin:4px;border-radius:8px;}.user{background:#667eea;color:white;}.bot{background:#f0f0f0;}</style></head><body><div class=\"container\" id=\"app\"><h1>{{title}}</h1><div><div v-for=\"(m,i) in msgs\" :key=\"i\" class=\"msg\" :class=\"m.sender\">{{m.text}}</div></div><input v-model=\"input\" @keyup.enter=\"send\" /><button class=\"btn\" @click=\"send\">Send</button></div><script>const {createApp,ref}=Vue;createApp({setup(){return{title:ref('%s'),input:ref(''),msgs:ref([{sender:'bot',text:'Hi!'}]),send(){if(this.input){this.msgs.push({sender:'user',text:this.input});this.input=''}}}).mount('#app');</script></body></html>",
        title, nl_responsive_css, nl_vue_cdn, title);
    add_web_route(server, path, html, "text/html");
}

void nl_web_add_gallery(nl_web_server_t* server, const char* path, const char* title, const char** image_urls, int count) {
    char images_str[32768] = "";
    for (int i = 0; i < count && i < 10; i++) {
        char img[512];
        snprintf(img, sizeof(img), "'%s',", image_urls[i]);
        strncat(images_str, img, sizeof(images_str) - strlen(images_str) - 1);
    }
    char html[65536];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><title>%s</title>%s%s<style>.gallery{display:grid;grid-template-columns:repeat(auto-fill,200px);gap:16px;}.gallery-item{border-radius:8px;overflow:hidden;}.gallery-item img{width:100%%;height:200px;object-fit:cover;}</style></head><body><div class=\"container\" id=\"app\"><h1>{{title}}</h1><div class=\"gallery\"><div v-for=\"(img,i) in images\" :key=\"i\" class=\"gallery-item\"><img :src=\"img\" /></div></div></div><script>const {createApp,ref}=Vue;createApp({setup(){return{title:ref('%s'),images:ref([%s])}}}).mount('#app');</script></body></html>",
        title, nl_responsive_css, nl_vue_cdn, title, images_str);
    add_web_route(server, path, html, "text/html");
}

int nl_serve_todo(int port, const char* title) {
    nl_web_server_t* server = nl_web_create(port);
    if (!server) return -1;
    nl_web_add_todo(server, "/", title);
    return nl_web_start(server);
}

int nl_serve_chat(int port, const char* title) {
    nl_web_server_t* server = nl_web_create(port);
    if (!server) return -1;
    nl_web_add_chat(server, "/", title);
    return nl_web_start(server);
}

// JSON Parser Implementation
typedef struct nl_json_node {
    nl_json_type_t type;
    union {
        int bool_val;
        int64_t int_val;
        double double_val;
        char* string_val;
        struct nl_json_node** array_val;
        struct {
            char** keys;
            struct nl_json_node** values;
            size_t count;
        } object_val;
    } data;
    size_t array_size;
} nl_json_node;

typedef struct nl_json {
    nl_json_node* root;
    nl_status_t error_code;
    int error_line;
    int error_col;
} nl_json_t;

static void skip_ws(const char** s, int* line, int* col) {
    while (**s && (unsigned char)**s <= 32) {
        if (**s == '\n') { (*line)++; *col = 0; }
        else (*col)++;
        (*s)++;
    }
}

static nl_json_node* parse_value(const char** s, int* line, int* col, nl_status_t* err) {
    skip_ws(s, line, col);
    if (!**s || **s == '\0') { *err = NL_EPARSE; return NULL; }
    nl_json_node* node = (nl_json_node*)calloc(1, sizeof(nl_json_node));
    if (!node) { *err = NL_ENOMEM; return NULL; }
    
    switch (**s) {
        case 'n': 
            if (strncmp(*s, "null", 4) == 0) { node->type = NL_JSON_NULL; (*s) += 4; *col += 4; }
            else { free(node); *err = NL_ESYNTAX; return NULL; }
            break;
        case 't':
            if (strncmp(*s, "true", 4) == 0) { node->type = NL_JSON_BOOL; node->data.bool_val = 1; (*s) += 4; *col += 4; }
            else { free(node); *err = NL_ESYNTAX; return NULL; }
            break;
        case 'f':
            if (strncmp(*s, "false", 5) == 0) { node->type = NL_JSON_BOOL; node->data.bool_val = 0; (*s) += 5; *col += 5; }
            else { free(node); *err = NL_ESYNTAX; return NULL; }
            break;
        case '"': {
            node->type = NL_JSON_STRING;
            (*s)++; (*col)++;
            size_t cap = 32, len = 0;
            node->data.string_val = (char*)malloc(cap);
            if (!node->data.string_val) { free(node); *err = NL_ENOMEM; return NULL; }
            while (**s && **s != '"') {
                if (**s == '\\') {
                    (*s)++; (*col)++;
                    if (!**s) { free(node->data.string_val); free(node); *err = NL_ESYNTAX; return NULL; }
                    char esc = 0;
                    switch (**s) {
                        case '"': case '\\': case '/': esc = **s; break;
                        case 'b': esc = '\b'; break;
                        case 'f': esc = '\f'; break;
                        case 'n': esc = '\n'; break;
                        case 'r': esc = '\r'; break;
                        case 't': esc = '\t'; break;
                        default: free(node->data.string_val); free(node); *err = NL_ESYNTAX; return NULL;
                    }
                    if (len + 1 >= cap) { cap *= 2; node->data.string_val = (char*)realloc(node->data.string_val, cap); }
                    node->data.string_val[len++] = esc;
                } else {
                    if (len + 1 >= cap) { cap *= 2; node->data.string_val = (char*)realloc(node->data.string_val, cap); }
                    node->data.string_val[len++] = **s;
                }
                (*s)++; (*col)++;
            }
            if (**s != '"') { free(node->data.string_val); free(node); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            node->data.string_val[len] = '\0';
            break;
        }
        case '[': {
            node->type = NL_JSON_ARRAY;
            (*s)++; (*col)++;
            size_t cap = 4; node->data.array_val = (nl_json_node**)malloc(sizeof(nl_json_node*) * cap);
            node->array_size = 0;
            if (!node->data.array_val) { free(node); *err = NL_ENOMEM; return NULL; }
            skip_ws(s, line, col);
            if (**s == ']') { (*s)++; break; }
            while (1) {
                if (node->array_size >= cap) { cap *= 2; node->data.array_val = (nl_json_node**)realloc(node->data.array_val, sizeof(nl_json_node*) * cap); }
                nl_json_node* item = parse_value(s, line, col, err);
                if (!item) { for (size_t i = 0; i < node->array_size; i++) free(node->data.array_val[i]); free(node->data.array_val); free(node); return NULL; }
                node->data.array_val[node->array_size++] = item;
                skip_ws(s, line, col);
                if (**s == ']') { (*s)++; break; }
                if (**s != ',') { for (size_t i = 0; i < node->array_size; i++) free(node->data.array_val[i]); free(node->data.array_val); free(node); *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
            }
            break;
        }
        case '{': {
            node->type = NL_JSON_OBJECT;
            node->data.object_val.keys = (char**)malloc(sizeof(char*) * 4);
            node->data.object_val.values = (nl_json_node**)malloc(sizeof(nl_json_node*) * 4);
            node->data.object_val.count = 0;
            size_t cap = 4;
            if (!node->data.object_val.keys || !node->data.object_val.values) { free(node); *err = NL_ENOMEM; return NULL; }
            (*s)++; (*col)++;
            skip_ws(s, line, col);
            if (**s == '}') { (*s)++; break; }
            while (1) {
                skip_ws(s, line, col);
                if (**s != '"') { *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
                size_t kcap = 32, klen = 0;
                char* key = (char*)malloc(kcap);
                if (!key) { free(node); *err = NL_ENOMEM; return NULL; }
                while (**s && **s != '"') {
                    if (klen + 1 >= kcap) { kcap *= 2; key = (char*)realloc(key, kcap); }
                    if (**s == '\\') { (*s)++; (*col)++; if (**s == '"') key[klen++] = '"'; else if (**s == '\\') key[klen++] = '\\'; else if (**s == 'n') key[klen++] = '\n'; else { free(key); *err = NL_ESYNTAX; return NULL; } }
                    else key[klen++] = **s;
                    (*s)++; (*col)++;
                }
                key[klen] = '\0';
                if (**s != '"') { free(key); *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
                skip_ws(s, line, col);
                if (**s != ':') { free(key); *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
                nl_json_node* val = parse_value(s, line, col, err);
                if (!val) { free(key); *err = NL_ESYNTAX; return NULL; }
                if (node->data.object_val.count >= cap) { cap *= 2; node->data.object_val.keys = (char**)realloc(node->data.object_val.keys, sizeof(char*) * cap); node->data.object_val.values = (nl_json_node**)realloc(node->data.object_val.values, sizeof(nl_json_node*) * cap); }
                node->data.object_val.keys[node->data.object_val.count] = key;
                node->data.object_val.values[node->data.object_val.count++] = val;
                skip_ws(s, line, col);
                if (**s == '}') { (*s)++; break; }
                if (**s != ',') { *err = NL_ESYNTAX; return NULL; }
                (*s)++; (*col)++;
            }
            break;
        }
        default: {
            if (**s == '-' || (**s >= '0' && **s <= '9')) {
                char* end; double d = strtod(*s, &end);
                if (end == *s) { free(node); *err = NL_ESYNTAX; return NULL; }
                if (d == (int64_t)d) { node->type = NL_JSON_INT; node->data.int_val = (int64_t)d; }
                else { node->type = NL_JSON_DOUBLE; node->data.double_val = d; }
                *s = end; *col += (int)(end - *s);
            } else { free(node); *err = NL_ESYNTAX; return NULL; }
        }
    }
    return node;
}

static void free_node(nl_json_node* node) {
    if (!node) return;
    switch (node->type) {
        case NL_JSON_STRING: if (node->data.string_val) free(node->data.string_val); break;
        case NL_JSON_ARRAY: for (size_t i = 0; i < node->array_size; i++) free_node(node->data.array_val[i]); free(node->data.array_val); break;
        case NL_JSON_OBJECT: for (size_t i = 0; i < node->data.object_val.count; i++) { free(node->data.object_val.keys[i]); free_node(node->data.object_val.values[i]); } free(node->data.object_val.keys); free(node->data.object_val.values); break;
        default: break;
    }
    free(node);
}

static void stringify_node(nl_json_node* node, char** out, size_t* cap, size_t* len) {
    char buf[128];
    switch (node->type) {
        case NL_JSON_NULL:
            while (*cap - *len < 4) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, "null", 4); *len += 4;
            break;
        case NL_JSON_BOOL:
            if (node->data.bool_val) {
                while (*cap - *len < 4) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
                memcpy(*out + *len, "true", 4); *len += 4;
            } else {
                while (*cap - *len < 5) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
                memcpy(*out + *len, "false", 5); *len += 5;
            }
            break;
        case NL_JSON_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)node->data.int_val);
            size_t ilen = strlen(buf);
            while (*cap - *len < ilen) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, buf, ilen); *len += ilen;
            break;
        case NL_JSON_DOUBLE:
            snprintf(buf, sizeof(buf), "%.17g", node->data.double_val);
            ilen = strlen(buf);
            while (*cap - *len < ilen) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, buf, ilen); *len += ilen;
            break;
        case NL_JSON_STRING: {
            while (*cap - *len < 2) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            (*out)[(*len)++] = '"';
            size_t slen = strlen(node->data.string_val);
            while (*cap - *len < slen + 2) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, node->data.string_val, slen); *len += slen;
            (*out)[(*len)++] = '"';
            break;
        }
        case NL_JSON_ARRAY:
            (*out)[(*len)++] = '[';
            for (size_t i = 0; i < node->array_size; i++) {
                if (i > 0) { (*out)[(*len)++] = ','; }
                stringify_node(node->data.array_val[i], out, cap, len);
            }
            (*out)[(*len)++] = ']';
            break;
        case NL_JSON_OBJECT:
            (*out)[(*len)++] = '{';
            for (size_t i = 0; i < node->data.object_val.count; i++) {
                if (i > 0) { (*out)[(*len)++] = ','; }
                size_t klen = strlen(node->data.object_val.keys[i]);
                while (*cap - *len < klen + 4) { *cap *= 2; *out = (char*)realloc(*out, *cap); }
                (*out)[(*len)++] = '"';
                memcpy(*out + *len, node->data.object_val.keys[i], klen); *len += klen;
                (*out)[(*len)++] = '"';
                (*out)[(*len)++] = ':';
                stringify_node(node->data.object_val.values[i], out, cap, len);
            }
            (*out)[(*len)++] = '}';
            break;
    }
}

void* nl_json_parse(const char* json_str, nl_status_t* error_code, int* error_line, int* error_col) {
    if (!json_str) { if (error_code) *error_code = NL_EINVAL; return NULL; }
    nl_json_t* json = (nl_json_t*)calloc(1, sizeof(nl_json_t));
    if (!json) { if (error_code) *error_code = NL_ENOMEM; return NULL; }
    int line = 1, col = 0;
    nl_status_t err = NL_OK;
    json->root = parse_value(&json_str, &line, &col, &err);
    if (!json->root) { json->error_code = err; json->error_line = line; json->error_col = col; if (error_code) *error_code = err; if (error_line) *error_line = line; if (error_col) *error_col = col; free(json); return NULL; }
    if (error_code) *error_code = NL_OK;
    if (error_line) *error_line = line;
    if (error_col) *error_col = col;
    return json;
}

void* nl_json_parse_file(const char* file_path, nl_status_t* error_code) {
    if (!file_path) { if (error_code) *error_code = NL_EINVAL; return NULL; }
    FILE* fp = fopen(file_path, "r");
    if (!fp) { if (error_code) *error_code = NL_EFILE; return NULL; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(fp); if (error_code) *error_code = NL_ENOMEM; return NULL; }
    fread(buf, 1, sz, fp); buf[sz] = '\0'; fclose(fp);
    int el = 0, ec = 0;
    void* json = nl_json_parse(buf, error_code, &el, &ec);
    free(buf);
    return json;
}

void nl_json_destroy(void* json) { if (!json) return; nl_json_t* j = (nl_json_t*)json; if (j->root) free_node(j->root); free(j); }
int nl_json_get_type(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root ? j->root->type : 0; }
int nl_json_get_bool(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == NL_JSON_BOOL ? j->root->data.bool_val : 0; }
int64_t nl_json_get_int(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == NL_JSON_INT ? j->root->data.int_val : 0; }
double nl_json_get_double(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == NL_JSON_DOUBLE ? j->root->data.double_val : 0.0; }
const char* nl_json_get_string(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == NL_JSON_STRING ? j->root->data.string_val : ""; }
size_t nl_json_array_size(void* json) { nl_json_t* j = (nl_json_t*)json; return j && j->root && j->root->type == NL_JSON_ARRAY ? j->root->array_size : 0; }
void* nl_json_array_get(void* json, size_t index) { nl_json_t* j = (nl_json_t*)json; if (!j || !j->root || j->root->type != NL_JSON_ARRAY || index >= j->root->array_size) return NULL; nl_json_t* r = (nl_json_t*)calloc(1, sizeof(nl_json_t)); if (r) r->root = j->root->data.array_val[index]; return r; }
void* nl_json_object_get(void* json, const char* key) { nl_json_t* j = (nl_json_t*)json; if (!j || !j->root || j->root->type != NL_JSON_OBJECT || !key) return NULL; for (size_t i = 0; i < j->root->data.object_val.count; i++) if (strcmp(j->root->data.object_val.keys[i], key) == 0) { nl_json_t* r = (nl_json_t*)calloc(1, sizeof(nl_json_t)); if (r) r->root = j->root->data.object_val.values[i]; return r; } return NULL; }
int nl_json_has_key(void* json, const char* key) { nl_json_t* j = (nl_json_t*)json; if (!j || !j->root || j->root->type != NL_JSON_OBJECT || !key) return 0; for (size_t i = 0; i < j->root->data.object_val.count; i++) if (strcmp(j->root->data.object_val.keys[i], key) == 0) return 1; return 0; }
char* nl_json_stringify(void* json, int pretty) { (void)pretty; nl_json_t* j = (nl_json_t*)json; if (!j || !j->root) return (char*)""; size_t cap = 256, len = 0; char* out = (char*)malloc(cap); if (!out) return NULL; stringify_node(j->root, &out, &cap, &len); out = (char*)realloc(out, len + 1); out[len] = '\0'; return out; }
int nl_json_save_file(void* json, const char* file_path, int pretty) { if (!json || !file_path) return NL_EINVAL; char* s = nl_json_stringify(json, pretty); if (!s) return NL_ENOMEM; FILE* fp = fopen(file_path, "w"); if (!fp) { free(s); return NL_EFILE; } fwrite(s, 1, strlen(s), fp); fclose(fp); free(s); return NL_OK; }
const char* nl_json_error_message(nl_status_t error_code) { switch (error_code) { case NL_OK: return "No error"; case NL_EINVAL: return "Invalid parameter"; case NL_ENOMEM: return "Out of memory"; case NL_EPARSE: return "Parse error"; case NL_ESYNTAX: return "Syntax error"; case NL_EFILE: return "File error"; default: return "Unknown error"; } }

// TOML Parser Implementation
typedef struct nl_toml_node {
    nl_toml_type_t type;
    union {
        int bool_val;
        int64_t int_val;
        double float_val;
        char* string_val;
        struct nl_toml_node** array_val;
        struct { char** keys; struct nl_toml_node** values; size_t count; } table_val;
    } data;
    size_t array_size;
} nl_toml_node;

typedef struct nl_toml {
    nl_toml_node* root;
    nl_status_t error_code;
    int error_line;
    int error_col;
} nl_toml_t;

static void toml_skip_ws(const char** s, int* line, int* col) {
    while (**s && (unsigned char)**s <= 32) { if (**s == '\n') { (*line)++; *col = 0; } else (*col)++; (*s)++; }
    while (**s == '#') { while (**s && **s != '\n' && **s != '\r') { (*s)++; (*col)++; } toml_skip_ws(s, line, col); }
}

static int toml_parse_string(const char** s, int* col, char** out, nl_status_t* err) {
    if (**s != '"') { *err = NL_ESYNTAX; return -1; }
    (*s)++; (*col)++;
    size_t cap = 32, len = 0;
    char* val = (char*)malloc(cap);
    if (!val) { *err = NL_ENOMEM; return -1; }
    while (**s && **s != '"') {
        if (**s == '\\') { (*s)++; (*col)++; if (!**s) { free(val); *err = NL_ESYNTAX; return -1; } char esc = 0; switch (**s) { case '"': esc = '"'; break; case '\\': esc = '\\'; break; case 'n': esc = '\n'; break; case 'r': esc = '\r'; break; case 't': esc = '\t'; break; default: free(val); *err = NL_ESYNTAX; return -1; } if (len + 1 >= cap) { cap *= 2; val = (char*)realloc(val, cap); } val[len++] = esc; }
        else { if (len + 1 >= cap) { cap *= 2; val = (char*)realloc(val, cap); } val[len++] = **s; }
        (*s)++; (*col)++;
    }
    if (**s != '"') { free(val); *err = NL_ESYNTAX; return -1; }
    (*s)++; (*col)++;
    if (len + 1 >= cap) val = (char*)realloc(val, len + 1);
    val[len] = '\0';
    *out = val;
    return 0;
}

static double toml_parse_float(const char** s, int* col) {
    int sign = 1;
    if (**s == '-') { sign = -1; (*s)++; (*col)++; }
    else if (**s == '+') { (*s)++; (*col)++; }
    double val = 0.0;
    while (**s >= '0' && **s <= '9') { val = val * 10 + (**s - '0'); (*s)++; (*col)++; }
    if (**s == '.') { (*s)++; (*col)++; double frac = 0.1; while (**s >= '0' && **s <= '9') { val += (**s - '0') * frac; frac *= 0.1; (*s)++; (*col)++; } }
    return val * sign;
}

static nl_toml_node* toml_parse_value(const char** s, int* line, int* col, nl_status_t* err) {
    toml_skip_ws(s, line, col);
    if (!**s || **s == '\0') { *err = NL_EPARSE; return NULL; }
    nl_toml_node* node = (nl_toml_node*)calloc(1, sizeof(nl_toml_node));
    if (!node) { *err = NL_ENOMEM; return NULL; }
    
    if (**s == '"') {
        node->type = NL_TOML_STRING;
        if (toml_parse_string(s, col, &node->data.string_val, err) != 0) { free(node); return NULL; }
    } else if (**s == '[') {
        node->type = NL_TOML_ARRAY;
        (*s)++; (*col)++;
        size_t cap = 4; node->data.array_val = (nl_toml_node**)malloc(sizeof(nl_toml_node*) * cap);
        node->array_size = 0;
        if (!node->data.array_val) { free(node); *err = NL_ENOMEM; return NULL; }
        toml_skip_ws(s, line, col);
        if (**s == ']') { (*s)++; (*col)++; return node; }
        while (1) {
            if (node->array_size >= cap) { cap *= 2; node->data.array_val = (nl_toml_node**)realloc(node->data.array_val, sizeof(nl_toml_node*) * cap); }
            nl_toml_node* item = toml_parse_value(s, line, col, err);
            if (!item) { for (size_t i = 0; i < node->array_size; i++) free(node->data.array_val[i]); free(node->data.array_val); free(node); return NULL; }
            node->data.array_val[node->array_size++] = item;
            toml_skip_ws(s, line, col);
            if (**s == ']') { (*s)++; (*col)++; break; }
            if (**s != ',') { for (size_t i = 0; i < node->array_size; i++) free(node->data.array_val[i]); free(node->data.array_val); free(node); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            toml_skip_ws(s, line, col);
        }
    } else if (**s == '{') {
        node->type = NL_TOML_TABLE;
        node->data.table_val.keys = (char**)malloc(sizeof(char*) * 4);
        node->data.table_val.values = (nl_toml_node**)malloc(sizeof(nl_toml_node*) * 4);
        node->data.table_val.count = 0;
        size_t cap = 4;
        if (!node->data.table_val.keys || !node->data.table_val.values) { free(node); *err = NL_ENOMEM; return NULL; }
        (*s)++; (*col)++;
        toml_skip_ws(s, line, col);
        if (**s == '}') { (*s)++; (*col)++; return node; }
        while (1) {
            toml_skip_ws(s, line, col);
            if (**s != '"') { *err = NL_ESYNTAX; return NULL; }
            char* key;
            if (toml_parse_string(s, col, &key, err) != 0) { *err = NL_ESYNTAX; return NULL; }
            toml_skip_ws(s, line, col);
            if (**s != '=') { free(key); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            nl_toml_node* val = toml_parse_value(s, line, col, err);
            if (!val) { free(key); *err = NL_ESYNTAX; return NULL; }
            if (node->data.table_val.count >= cap) { cap *= 2; node->data.table_val.keys = (char**)realloc(node->data.table_val.keys, sizeof(char*) * cap); node->data.table_val.values = (nl_toml_node**)realloc(node->data.table_val.values, sizeof(nl_toml_node*) * cap); }
            node->data.table_val.keys[node->data.table_val.count] = key;
            node->data.table_val.values[node->data.table_val.count++] = val;
            toml_skip_ws(s, line, col);
            if (**s == '}') { (*s)++; (*col)++; break; }
            if (**s != ',') { *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            toml_skip_ws(s, line, col);
        }
    } else if (**s == 't' && strncmp(*s, "true", 4) == 0) {
        node->type = NL_TOML_BOOL; node->data.bool_val = 1; (*s) += 4; (*col) += 4;
    } else if (**s == 'f' && strncmp(*s, "false", 5) == 0) {
        node->type = NL_TOML_BOOL; node->data.bool_val = 0; (*s) += 5; (*col) += 5;
    } else if (**s == '-' || **s == '+' || (**s >= '0' && **s <= '9')) {
        const char* start = *s;
        double d = toml_parse_float(s, col);
        if (start == *s) { free(node); *err = NL_ESYNTAX; return NULL; }
        if (d == (int64_t)d) { node->type = NL_TOML_INT; node->data.int_val = (int64_t)d; }
        else { node->type = NL_TOML_FLOAT; node->data.float_val = d; }
    } else { free(node); *err = NL_ESYNTAX; return NULL; }
    return node;
}

static void toml_free_node(nl_toml_node* node) {
    if (!node) return;
    switch (node->type) {
        case NL_TOML_STRING: free(node->data.string_val); break;
        case NL_TOML_ARRAY: for (size_t i = 0; i < node->array_size; i++) toml_free_node(node->data.array_val[i]); free(node->data.array_val); break;
        case NL_TOML_TABLE: for (size_t i = 0; i < node->data.table_val.count; i++) { free(node->data.table_val.keys[i]); toml_free_node(node->data.table_val.values[i]); } free(node->data.table_val.keys); free(node->data.table_val.values); break;
        default: break;
    }
    free(node);
}

static nl_toml_node* toml_parse_main(const char** s, int* line, int* col, nl_status_t* err) {
    nl_toml_node* root = (nl_toml_node*)calloc(1, sizeof(nl_toml_node));
    if (!root) { *err = NL_ENOMEM; return NULL; }
    root->type = NL_TOML_TABLE;
    root->data.table_val.keys = (char**)malloc(sizeof(char*) * 4);
    root->data.table_val.values = (nl_toml_node**)malloc(sizeof(nl_toml_node*) * 4);
    root->data.table_val.count = 0;
    size_t cap = 4;
    if (!root->data.table_val.keys || !root->data.table_val.values) { free(root); *err = NL_ENOMEM; return NULL; }
    
    while (**s) {
        toml_skip_ws(s, line, col);
        if (!**s || **s == '\0') break;
        if (**s == '[') {
            (*s)++; (*col)++;
            toml_skip_ws(s, line, col);
            size_t kcap = 32, klen = 0;
            char* key = (char*)malloc(kcap);
            if (!key) { toml_free_node(root); *err = NL_ENOMEM; return NULL; }
            while (**s && **s != ']') { if (klen + 1 >= kcap) { kcap *= 2; key = (char*)realloc(key, kcap); } key[klen++] = **s; (*s)++; (*col)++; }
            if (**s != ']') { free(key); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
            key[klen] = '\0';
            (*s)++; (*col)++;
            toml_skip_ws(s, line, col);
            nl_toml_node* tbl = (nl_toml_node*)calloc(1, sizeof(nl_toml_node));
            if (!tbl) { free(key); toml_free_node(root); *err = NL_ENOMEM; return NULL; }
            tbl->type = NL_TOML_TABLE;
            tbl->data.table_val.keys = (char**)malloc(sizeof(char*) * 4);
            tbl->data.table_val.values = (nl_toml_node**)malloc(sizeof(nl_toml_node*) * 4);
            tbl->data.table_val.count = 0;
            if (!tbl->data.table_val.keys || !tbl->data.table_val.values) { free(key); free(tbl); toml_free_node(root); *err = NL_ENOMEM; return NULL; }
            size_t tcap = 4;
            while (**s) {
                toml_skip_ws(s, line, col);
                if (!**s || **s == '[') break;
                if (**s == '"') {
                    (*s)++; (*col)++;
                    size_t kvcap = 32, kvlen = 0;
                    char* kval = (char*)malloc(kvcap);
                    if (!kval) { free(key); toml_free_node(tbl); toml_free_node(root); *err = NL_ENOMEM; return NULL; }
                    while (**s && **s != '"') { if (kvlen + 1 >= kvcap) { kvcap *= 2; kval = (char*)realloc(kval, kvcap); } if (**s == '\\') { (*s)++; (*col)++; if (**s == '"') kval[kvlen++] = '"'; else if (**s == '\\') kval[kvlen++] = '\\'; else if (**s == 'n') kval[kvlen++] = '\n'; else { free(kval); free(key); toml_free_node(tbl); toml_free_node(root); *err = NL_ESYNTAX; return NULL; } } else kval[kvlen++] = **s; (*s)++; (*col)++; }
                    if (**s != '"') { free(kval); free(key); toml_free_node(tbl); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
                    (*s)++; (*col)++;
                    kval[kvlen] = '\0';
                    toml_skip_ws(s, line, col);
                    if (**s != '=') { free(kval); free(key); toml_free_node(tbl); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
                    (*s)++; (*col)++;
                    nl_toml_node* val = toml_parse_value(s, line, col, err);
                    if (!val) { free(kval); free(key); toml_free_node(tbl); toml_free_node(root); return NULL; }
                    if (tbl->data.table_val.count >= tcap) { tcap *= 2; tbl->data.table_val.keys = (char**)realloc(tbl->data.table_val.keys, sizeof(char*) * tcap); tbl->data.table_val.values = (nl_toml_node**)realloc(tbl->data.table_val.values, sizeof(nl_toml_node*) * tcap); }
                    tbl->data.table_val.keys[tbl->data.table_val.count] = kval;
                    tbl->data.table_val.values[tbl->data.table_val.count++] = val;
                    toml_skip_ws(s, line, col);
                    if (**s == '\n' || **s == '\r') { (*s)++; if (**s == '\n') { (*line)++; (*col) = 0; } }
                    else if (**s == ',') { (*s)++; (*col)++; toml_skip_ws(s, line, col); }
                } else { (*s)++; (*col)++; }
            }
            if (root->data.table_val.count >= cap) { cap *= 2; root->data.table_val.keys = (char**)realloc(root->data.table_val.keys, sizeof(char*) * cap); root->data.table_val.values = (nl_toml_node**)realloc(root->data.table_val.values, sizeof(nl_toml_node*) * cap); }
            root->data.table_val.keys[root->data.table_val.count] = key;
            root->data.table_val.values[root->data.table_val.count++] = tbl;
        } else if (**s == '"') {
            (*s)++; (*col)++;
            size_t kvcap = 32, kvlen = 0;
            char* kval = (char*)malloc(kvcap);
            if (!kval) { toml_free_node(root); *err = NL_ENOMEM; return NULL; }
            while (**s && **s != '"') { if (kvlen + 1 >= kvcap) { kvcap *= 2; kval = (char*)realloc(kval, kvcap); } if (**s == '\\') { (*s)++; (*col)++; if (**s == '"') kval[kvlen++] = '"'; else if (**s == '\\') kval[kvlen++] = '\\'; else if (**s == 'n') kval[kvlen++] = '\n'; else { free(kval); toml_free_node(root); *err = NL_ESYNTAX; return NULL; } } else kval[kvlen++] = **s; (*s)++; (*col)++; }
            if (**s != '"') { free(kval); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            kval[kvlen] = '\0';
            toml_skip_ws(s, line, col);
            if (**s != '=') { free(kval); toml_free_node(root); *err = NL_ESYNTAX; return NULL; }
            (*s)++; (*col)++;
            nl_toml_node* val = toml_parse_value(s, line, col, err);
            if (!val) { free(kval); toml_free_node(root); return NULL; }
            if (root->data.table_val.count >= cap) { cap *= 2; root->data.table_val.keys = (char**)realloc(root->data.table_val.keys, sizeof(char*) * cap); root->data.table_val.values = (nl_toml_node**)realloc(root->data.table_val.values, sizeof(nl_toml_node*) * cap); }
            root->data.table_val.keys[root->data.table_val.count] = kval;
            root->data.table_val.values[root->data.table_val.count++] = val;
            toml_skip_ws(s, line, col);
            if (**s == '\n' || **s == '\r') { (*s)++; if (**s == '\n') { (*line)++; (*col) = 0; } }
            else if (**s == ',') { (*s)++; (*col)++; }
        } else { (*s)++; (*col)++; }
    }
    return root;
}

void* nl_toml_parse(const char* toml_str, nl_status_t* error_code, int* error_line, int* error_col) {
    if (!toml_str) { if (error_code) *error_code = NL_EINVAL; return NULL; }
    nl_toml_t* toml = (nl_toml_t*)calloc(1, sizeof(nl_toml_t));
    if (!toml) { if (error_code) *error_code = NL_ENOMEM; return NULL; }
    int line = 1, col = 0;
    nl_status_t err = NL_OK;
    toml->root = toml_parse_main(&toml_str, &line, &col, &err);
    if (!toml->root) { toml->error_code = err; toml->error_line = line; toml->error_col = col; if (error_code) *error_code = err; if (error_line) *error_line = line; if (error_col) *error_col = col; free(toml); return NULL; }
    if (error_code) *error_code = NL_OK;
    if (error_line) *error_line = line;
    if (error_col) *error_col = col;
    return toml;
}

void* nl_toml_parse_file(const char* file_path, nl_status_t* error_code) {
    if (!file_path) { if (error_code) *error_code = NL_EINVAL; return NULL; }
    FILE* fp = fopen(file_path, "r");
    if (!fp) { if (error_code) *error_code = NL_EFILE; return NULL; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    char* buf = (char*)malloc(sz + 1);
    if (!buf) { fclose(fp); if (error_code) *error_code = NL_ENOMEM; return NULL; }
    fread(buf, 1, sz, fp); buf[sz] = '\0'; fclose(fp);
    int el = 0, ec = 0;
    void* toml = nl_toml_parse(buf, error_code, &el, &ec);
    free(buf);
    return toml;
}

void nl_toml_destroy(void* toml) { if (!toml) return; nl_toml_t* t = (nl_toml_t*)toml; if (t->root) toml_free_node(t->root); free(t); }
int nl_toml_get_type(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return t && t->root ? t->root->type : 0; }
const char* nl_toml_get_string(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_STRING) ? t->root->data.string_val : ""; }
int64_t nl_toml_get_int(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_INT) ? t->root->data.int_val : 0; }
double nl_toml_get_float(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_FLOAT) ? t->root->data.float_val : 0.0; }
int nl_toml_get_bool(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_BOOL) ? t->root->data.bool_val : 0; }
size_t nl_toml_array_size(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; return (t && t->root && t->root->type == NL_TOML_ARRAY) ? t->root->array_size : 0; }
void* nl_toml_array_get(void* toml, size_t index) { nl_toml_t* t = (nl_toml_t*)toml; if (!t || !t->root || t->root->type != NL_TOML_ARRAY || index >= t->root->array_size) return NULL; nl_toml_t* r = (nl_toml_t*)calloc(1, sizeof(nl_toml_t)); if (r) r->root = t->root->data.array_val[index]; return r; }
void* nl_toml_table_get(void* toml, const char* key) { nl_toml_t* t = (nl_toml_t*)toml; if (!t || !t->root || t->root->type != NL_TOML_TABLE || !key) return NULL; for (size_t i = 0; i < t->root->data.table_val.count; i++) if (strcmp(t->root->data.table_val.keys[i], key) == 0) { nl_toml_t* r = (nl_toml_t*)calloc(1, sizeof(nl_toml_t)); if (r) r->root = t->root->data.table_val.values[i]; return r; } return NULL; }
int nl_toml_has_key(void* toml, const char* key) { nl_toml_t* t = (nl_toml_t*)toml; if (!t || !t->root || t->root->type != NL_TOML_TABLE || !key) return 0; for (size_t i = 0; i < t->root->data.table_val.count; i++) if (strcmp(t->root->data.table_val.keys[i], key) == 0) return 1; return 0; }
char* nl_toml_stringify(void* toml) { nl_toml_t* t = (nl_toml_t*)toml; if (!t || !t->root) return (char*)""; return (char*)"# TOML stringification not implemented"; }
int nl_toml_save_file(void* toml, const char* file_path) { (void)toml; (void)file_path; return NL_ENOTSUPPORTED; }
const char* nl_toml_error_message(nl_status_t error_code) { return nl_json_error_message(error_code); }

void nl_web_enable_auto_encoding(nl_web_server_t* server, int enable) { if (!server) return; server->auto_encoding_enabled = enable; }
int nl_web_is_auto_encoding_enabled(nl_web_server_t* server) { if (!server) return 0; return server->auto_encoding_enabled; }
void nl_web_set_fallback_encoding(nl_web_server_t* server, const char* encoding) { if (!server || !encoding) return; strncpy(server->fallback_encoding, encoding, sizeof(server->fallback_encoding) - 1); }
const char* nl_web_get_negotiated_encoding(nl_web_server_t* server) { if (!server) return NULL; if (server->auto_encoding_enabled && strlen(server->fallback_encoding) > 0) return server->fallback_encoding; return server->encoding; }

// =========================================
// Error Page API (v2.2.0)
// =========================================

int nl_web_server_set_error_page(nl_web_server_t* server, int status_code, const char* template_path) {
    if (!server || !template_path) return NL_ERROR_PAGE_NOT_FOUND;
    if (status_code < 100 || status_code > 999) return NL_ERROR_PAGE_NOT_FOUND;
    
    int idx = 0;
    if (status_code == 400) idx = 0;
    else if (status_code == 401) idx = 1;
    else if (status_code == 403) idx = 2;
    else if (status_code == 404) idx = 3;
    else if (status_code == 500) idx = 4;
    else if (status_code == 502) idx = 5;
    else if (status_code == 503) idx = 6;
    else idx = 7;
    
    strncpy(server->error_page_templates[idx], template_path, sizeof(server->error_page_templates[idx]) - 1);
    server->error_page_templates[idx][sizeof(server->error_page_templates[idx]) - 1] = '\0';
    
    return NL_ERROR_PAGE_OK;
}

int nl_web_server_enable_error_suggestions(nl_web_server_t* server, int enable) {
    if (!server) return 0;
    server->error_suggestions_enabled = enable ? 1 : 0;
    return 1;
}

int nl_web_server_is_error_suggestions_enabled(nl_web_server_t* server) {
    if (!server) return 0;
    return server->error_suggestions_enabled;
}

static const char* get_error_code_string(int status_code) {
    switch (status_code) {
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown Error";
    }
}

char* nl_render_error_page(const char* template_content, nl_error_page_vars_t* vars) {
    if (!vars) return NULL;
    (void)template_content;
    
    char* result = malloc(4096);
    if (!result) return NULL;
    
    // Simple template substitution
    snprintf(result, 4096, 
        "<html><head><title>%d - %s</title></head>"
        "<body><h1>%d %s</h1>"
        "<p>Requested path: %s</p>"
        "%s"
        "<p>Server version: %s</p></body></html>",
        vars->status_code, vars->error_message ? vars->error_message : get_error_code_string(vars->status_code),
        vars->status_code, vars->error_message ? vars->error_message : get_error_code_string(vars->status_code),
        vars->requested_path ? vars->requested_path : "/",
        vars->suggestion ? vars->suggestion : "",
        vars->server_version ? vars->server_version : "NetLeaf v2.2.0");
    
    return result;
}

char* nl_make_error_response(int status_code, const char* error_message, const char* requested_path, const char* suggestion) {
    nl_error_page_vars_t vars;
    memset(&vars, 0, sizeof(vars));
    vars.status_code = status_code;
    vars.error_message = error_message ? error_message : get_error_code_string(status_code);
    vars.requested_path = requested_path;
    vars.suggestion = suggestion;
    vars.server_version = "NetLeaf v2.2.0";
    
    time_t now = time(NULL);
    struct tm tm_buf;
    char time_str[64];
    localtime_r(&now, &tm_buf);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);
    vars.timestamp = time_str;
    
    char* body = nl_render_error_page(NULL, &vars);
    if (!body) return NULL;
    
    char* response = malloc(strlen(body) + 256);
    if (!response) { free(body); return NULL; }
    
    snprintf(response, strlen(body) + 256,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n%s",
        status_code, get_error_code_string(status_code),
        strlen(body), body);
    
    free(body);
    return response;
}
