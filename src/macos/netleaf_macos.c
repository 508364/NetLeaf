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
#include <poll.h>
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
#define DEFAULT_CONCURRENCY 4
/* 高压场景：Web 服务（含反向代理）worker 池默认值，受 MAX_CONCURRENCY 约束。 */
#define WEB_WORKER_DEFAULT 16
#define MAX_CONCURRENCY 64
#define MAX_WORKER_QUEUE 1024

struct nl_server {
    int fd;
    int kqueue_fd;
    int port;
    nl_protocol_t protocol;
    nl_request_handler handler;
    nl_udp_message_handler udp_handler;
    nl_udp_message_handler_v2 udp_handler_v2;
    void* user_data;
    volatile int running;
    pthread_t thread_id;
    struct kevent events[MAX_EVENTS];
    int concurrency;
    int num_workers;
    pthread_t* workers;
    int* queue;
    int queue_head;
    int queue_tail;
    int queue_count;
    int queue_max;
    pthread_mutex_t queue_lock;
    pthread_cond_t queue_not_empty;
    pthread_cond_t queue_not_full;
    int queue_init;
    int pool_used;
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

static int set_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
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

static void parse_http_request(const char* data, size_t len,
                               char* path, size_t path_size,
                               nl_http_method_t* method,
                               const char** body, size_t* body_size) {
    if (!data || len == 0) return;

    // Find end of headers (double CRLF)
    const char* header_end = memmem(data, len, "\r\n\r\n", 4);
    if (!header_end) header_end = memmem(data, len, "\n\n", 2);
    if (!header_end) header_end = data + len;

    // Parse first line: METHOD PATH VERSION
    size_t header_len = (size_t)(header_end - data);
    char* line = (char*)malloc(header_len + 1);
    if (!line) return;
    memcpy(line, data, header_len);
    line[header_len] = '\0';

    char* space1 = strchr(line, ' ');
    char* space2 = space1 ? strchr(space1 + 1, ' ') : NULL;

    if (space1 && space2) {
        *space1 = '\0';
        *space2 = '\0';

        // Parse method
        if (strncmp(line, "GET", 3) == 0) *method = NL_METHOD_GET;
        else if (strncmp(line, "POST", 4) == 0) *method = NL_METHOD_POST;
        else if (strncmp(line, "PUT", 3) == 0) *method = NL_METHOD_PUT;
        else if (strncmp(line, "DELETE", 6) == 0) *method = NL_METHOD_DELETE;
        else *method = NL_METHOD_GET;

        // Parse path
        strncpy(path, space1 + 1, path_size - 1);
        path[path_size - 1] = '\0';

        // Body starts after headers
        size_t body_start = (size_t)(header_end - data) + 4;
        if (body_start < len) {
            *body_size = len - body_start;
            *body = data + body_start;
        } else {
            *body_size = 0;
            *body = NULL;
        }
    } else {
        *method = NL_METHOD_GET;
        *body_size = 0;
        *body = NULL;
    }

    free(line);
}

static void* listener_thread(void* arg) {
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
                    // 没设 handler 的 UDP server 直接跳过本轮事件；kevent 会自然睡眠。
                    if (!server->udp_handler && !server->udp_handler_v2) continue;
                    char buf[BUFFER_SIZE];
                    struct sockaddr_in addr;
                    socklen_t addr_len = sizeof(addr);
                    // BUG-ARCH-003: 非阻塞 UDP socket 可能积压多个 datagram，
                    // 单次 recvfrom 只收一个会导致后续包被静默丢弃。
                    // 循环 recvfrom 直到 EAGAIN/EWOULDBLOCK（无更多数据）。
                    for (;;) {
                        addr_len = sizeof(addr);
                        ssize_t len = recvfrom(fd, buf, BUFFER_SIZE, 0,
                                              (struct sockaddr*)&addr, &addr_len);
                        if (len < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            if (errno == EINTR) continue;
                            macos_log(NL_LOG_ERROR, "macOS: UDP recvfrom failed: %s", strerror(errno));
                            break;
                        }
                        if (len == 0) break;
                        if (server->udp_handler_v2) {
                                // BUG-201: expose peer address + port to v2 handler
                                char addr_str[INET6_ADDRSTRLEN];
                                if (inet_ntop(AF_INET, &addr.sin_addr, addr_str, sizeof(addr_str)) != NULL) {
                                    server->udp_handler_v2(buf, (size_t)len,
                                                           addr_str, (int)ntohs(addr.sin_port),
                                                           server->user_data);
                                } else {
                                    server->udp_handler_v2(buf, (size_t)len,
                                                           "0.0.0.0", (int)ntohs(addr.sin_port),
                                                           server->user_data);
                                }
                            } else {
                                server->udp_handler(buf, (size_t)len, server->user_data);
                            }
                        }
                } else if (server->pool_used) {
                    // Dispatch to worker pool
                    while (server->running) {
                        struct sockaddr_in client_addr;
                        socklen_t client_len = sizeof(client_addr);
                        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
                        if (client_fd == -1) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) {
                                break;
                            }
                            if (errno != EINTR) {
                                macos_log(NL_LOG_ERROR, "macOS: accept() failed: %s", strerror(errno));
                            }
                            break;
                        }
                        set_blocking(client_fd);
                        pthread_mutex_lock(&server->queue_lock);
                        while (server->queue_count == server->queue_max && server->running) {
                            pthread_cond_wait(&server->queue_not_full, &server->queue_lock);
                        }
                        if (!server->running) {
                            pthread_mutex_unlock(&server->queue_lock);
                            close(client_fd);
                            break;
                        }
                        server->queue[server->queue_tail] = client_fd;
                        server->queue_tail = (server->queue_tail + 1) % server->queue_max;
                        server->queue_count++;
                        pthread_cond_signal(&server->queue_not_empty);
                        pthread_mutex_unlock(&server->queue_lock);
                    }
                } else {
                    // Legacy single-thread path
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

                    char buf[BUFFER_SIZE];
                    ssize_t n = read(fd, buf, BUFFER_SIZE);
                    if (n > 0) {
                        nl_buffer_write(req, buf, (size_t)n);

                        char path[1024] = {0};
                        nl_http_method_t method = NL_METHOD_GET;
                        const char* body = NULL;
                        size_t body_size = 0;

                        parse_http_request(req->data, nl_buffer_size(req),
                                          path, sizeof(path), &method, &body, &body_size);

                        char* response = NULL;
                        size_t response_len = 0;

                        server->handler(path, method, body, body_size, &response, &response_len, server->user_data);

                        // BUG-002: 契约要求 *response 必须堆分配（malloc/calloc/realloc）。
                        // 服务端在此处 free(response)：若 handler 传字符串字面量/静态数组
                        // 会触发 free(): invalid pointer。无响应时 handler 须置 *response = NULL。
                        if (response && response_len > 0) {
                            if (write(fd, response, response_len) < 0) { /* best-effort */ }
                            free(response);
                        }

                        // BUG-001: HTTP 请求-响应模型，写完 response 即关 fd。
                        // kqueue 在 close 时会自动 detach EVFILT_READ，无需 EV_DELETE。
                        close(fd);
                    } else if (n == 0) {
                        // 对端 FIN（kqueue EVFILT_READ 在 read 返回 0 时自动 detach）
                        close(fd);
                    } else if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // 非阻塞，下次再来
                        } else {
                            close(fd);
                        }
                    }
                    
                    nl_buffer_destroy(req);
                }
            }
        }
    }
    
    return NULL;
}

static void worker_handle_connection(nl_server_t* server, int fd) {
    nl_buffer_t* req = nl_buffer_create(BUFFER_SIZE);
    if (!req) {
        close(fd);
        return;
    }

    char buf[BUFFER_SIZE];
    int idle_ms = 30000;
    while (server->running) {
        int have_data = 0;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, idle_ms);
        if (pr == -1) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            have_data = (nl_buffer_size(req) > 0);
        } else if (pfd.revents & (POLLIN | POLLRDNORM)) {
            have_data = 1;
        }

        if (have_data) {
            ssize_t n = read(fd, buf, BUFFER_SIZE);
            if (n > 0) {
                nl_buffer_write(req, buf, (size_t)n);
            } else if (n == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
        }

        if (nl_buffer_size(req) > 0 && server->handler) {
            char path[1024] = {0};
            nl_http_method_t method = NL_METHOD_GET;
            const char* body = NULL;
            size_t body_size = 0;

            parse_http_request(req->data, nl_buffer_size(req),
                              path, sizeof(path), &method, &body, &body_size);

            char* response = NULL;
            size_t response_len = 0;
            server->handler(path, method, body, body_size, &response, &response_len, server->user_data);

            // BUG-002: *response 必须堆分配；服务端 free(response) 前不做字面量/静态缓冲校验。
            if (response && response_len > 0) {
                ssize_t total = 0;
                while (total < (ssize_t)response_len) {
                    ssize_t w = write(fd, response + total, response_len - total);
                    if (w <= 0) break;
                    total += w;
                }
                free(response);
            }
            nl_buffer_clear(req);
        }

        if (!have_data || pr == 0) break;
    }

    close(fd);
    nl_buffer_destroy(req);
}

static void* worker_thread(void* arg) {
    nl_server_t* server = (nl_server_t*)arg;

    while (server->running) {
        int fd = -1;
        pthread_mutex_lock(&server->queue_lock);
        while (server->queue_count == 0 && server->running) {
            pthread_cond_wait(&server->queue_not_empty, &server->queue_lock);
        }
        if (!server->running) {
            pthread_mutex_unlock(&server->queue_lock);
            break;
        }
        fd = server->queue[server->queue_head];
        server->queue_head = (server->queue_head + 1) % server->queue_max;
        server->queue_count--;
        pthread_cond_signal(&server->queue_not_full);
        pthread_mutex_unlock(&server->queue_lock);
        
        worker_handle_connection(server, fd);
    }
    
    return NULL;
}

static void stop_worker_pool(nl_server_t* server) {
    if (!server->pool_used) return;
    pthread_mutex_lock(&server->queue_lock);
    server->running = 0;
    for (int i = 0; i < server->queue_count; i++) {
        int fd = server->queue[server->queue_head];
        server->queue_head = (server->queue_head + 1) % server->queue_max;
        if (fd > 0) close(fd);
    }
    server->queue_count = 0;
    pthread_cond_broadcast(&server->queue_not_empty);
    pthread_cond_broadcast(&server->queue_not_full);
    pthread_mutex_unlock(&server->queue_lock);
    for (int i = 0; i < server->num_workers; i++) {
        pthread_join(server->workers[i], NULL);
    }
    pthread_cond_destroy(&server->queue_not_empty);
    pthread_cond_destroy(&server->queue_not_full);
    pthread_mutex_destroy(&server->queue_lock);
    free(server->queue);
    free(server->workers);
    server->queue = NULL;
    server->workers = NULL;
    server->num_workers = 0;
    server->pool_used = 0;
}

static void cleanup_start_failure(nl_server_t* server) {
    stop_worker_pool(server);
}

nl_server_t* nl_server_create(nl_protocol_t protocol, int port) {
    nl_server_t* server = calloc(1, sizeof(nl_server_t));
    if (!server) return NULL;
    
    server->protocol = protocol;
    server->port = port;
    server->concurrency = 0;
    server->num_workers = 0;
    server->pool_used = 0;
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
    /* Signal the listener thread to stop BEFORE touching its fds, otherwise
     * it blocks on kevent() and we leak the thread + UAF on free(server). */
    server->running = 0;
    if (server->pool_used) stop_worker_pool(server);
    if (server->fd != -1) close(server->fd);
    if (server->kqueue_fd != -1) close(server->kqueue_fd);
    /* Join the listener thread so it cannot run after free(server) below. */
    pthread_join(server->thread_id, NULL);
    free(server);
}

int nl_server_start(nl_server_t* server) {
    if (!server) return NL_EINVAL;
    
    int pool_created = 0;
    
    if (server->protocol == NL_PROTO_TCP || server->protocol == NL_PROTO_HTTP || 
        server->protocol == NL_PROTO_WEBSOCKET) {
        if (listen(server->fd, SOMAXCONN) == -1) {
            macos_log(NL_LOG_ERROR, "macOS: listen() failed: %s", strerror(errno));
            return NL_ERROR;
        }
        
        int use_pool = (server->protocol != NL_PROTO_UDP) ? 1 : 0;
        int target = (server->concurrency >= 1 && server->concurrency <= MAX_CONCURRENCY)
                     ? server->concurrency : DEFAULT_CONCURRENCY;
        if (use_pool && !server->pool_used) {
            server->num_workers = target;
            server->queue_max = MAX_WORKER_QUEUE;
            server->queue_head = 0;
            server->queue_tail = 0;
            server->queue_count = 0;
            server->queue = calloc((size_t)server->queue_max, sizeof(int));
            server->workers = calloc((size_t)server->num_workers, sizeof(pthread_t));
            if (!server->queue || !server->workers) {
                free(server->queue);
                free(server->workers);
                server->queue = NULL;
                server->workers = NULL;
                server->num_workers = 0;
                macos_log(NL_LOG_ERROR, "macOS: failed to allocate worker pool");
                return NL_ERROR;
            }
            pthread_mutex_init(&server->queue_lock, NULL);
            pthread_cond_init(&server->queue_not_empty, NULL);
            pthread_cond_init(&server->queue_not_full, NULL);
            server->pool_used = 1;
            pool_created = 1;
            server->running = 1;
            for (int i = 0; i < server->num_workers; i++) {
                if (pthread_create(&server->workers[i], NULL, worker_thread, server) != 0) {
                    cleanup_start_failure(server);
                    macos_log(NL_LOG_ERROR, "macOS: failed to create worker thread %d", i);
                    return NL_ERROR;
                }
            }
        }
    }
    
    struct kevent ev;
    EV_SET(&ev, server->fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    
    if (kevent(server->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
        macos_log(NL_LOG_ERROR, "macOS: kevent() failed: %s", strerror(errno));
        if (pool_created) stop_worker_pool(server);
        return NL_ERROR;
    }
    
    if (pool_created) {
        server->running = 1;
    }
    if (pthread_create(&server->thread_id, NULL, listener_thread, server) != 0) {
        server->running = 0;
        if (pool_created) stop_worker_pool(server);
        return NL_ERROR;
    }
    
    macos_log(NL_LOG_INFO, "macOS: Server started (kqueue), fd=%d, workers=%d",
              server->fd, server->pool_used ? server->num_workers : 0);
    return NL_OK;
}

void nl_server_stop(nl_server_t* server) {
    if (!server) return;
    server->running = 0;
    stop_worker_pool(server);
    
    if (server->fd != -1) {
        close(server->fd);
        server->fd = -1;
    }
    if (server->thread_id) {
        pthread_join(server->thread_id, NULL);
    }
    macos_log(NL_LOG_INFO, "macOS: Server stopped");
}

int nl_server_set_concurrency(nl_server_t* server, int threads) {
    if (!server) return NL_EINVAL;
    if (threads <= 0 || threads > MAX_CONCURRENCY) return NL_EINVAL;
    server->concurrency = threads;
    return NL_OK;
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

void nl_server_set_udp_handler_v2(nl_server_t* server, nl_udp_message_handler_v2 handler, void* user_data) {
    if (!server) return;
    server->udp_handler_v2 = handler;
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
        case NL_OPT_CONCURRENCY:
            result = (nl_server_set_concurrency(server, value) == NL_OK) ? 0 : -1;
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
        case NL_OPT_CONCURRENCY:
            *value = (server->concurrency >= 1 && server->concurrency <= MAX_CONCURRENCY)
                     ? server->concurrency : DEFAULT_CONCURRENCY;
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
            // 用 snprintf 保证 NUL 终止并清空槽位残留（旧 token/密码不会被带出）
            snprintf(config->data + i * 128 + 64, 64, "%s", value);
            pthread_mutex_unlock(&config->mutex);
            return;
        }
    }
    
    if (config->count < 100) {
        snprintf(config->data + config->count * 128, 64, "%s", key);
        snprintf(config->data + config->count * 128 + 64, 64, "%s", value);
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
    
    // 目录边界校验：前缀相同还需下一字符为路径结束或 '/'，避免 /base 误匹配 /baseXXX
    size_t base_len = strlen(normalized_base);
    if (strncmp(normalized_file, normalized_base, base_len) != 0) return 0;
    if (base_len > 0 && normalized_base[base_len - 1] == '/') return 1;
    return normalized_file[base_len] == '\0' || normalized_file[base_len] == '/';
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
            // BUG-002: *response 必须堆分配（malloc/calloc/realloc）；
            // 字面量/静态缓冲传到这里 free() 会崩。无响应须置 *response = NULL。
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
        const char* default_resp = "{\"message\":\"NetLeaf v2.4.0\"}";
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
    NL_ROUTE_TYPE_REDIRECT = 2,   // HTTP 302 redirect
    NL_ROUTE_TYPE_PROXY = 3       // Reverse proxy (upstream http/https/tcp)
} nl_route_type_t;

// 上游协议类型（反代使用）
typedef enum {
    NL_PROXY_HTTP = 0,           // 明文 HTTP 上游
    NL_PROXY_HTTPS = 1,          // TLS 上游
    NL_PROXY_TCP = 2             // 原始 TCP 字节透传
} nl_proxy_protocol_t;

typedef struct nl_web_route {
    char path[256];
    char* content;
    size_t content_size;
    char content_type[64];
    nl_route_type_t type;         // Route type
    char file_path[512];          // File path for hot reload
    char redirect_url[512];       // Redirect URL for 302
    char upstream[512];           // 反代上游地址 host:port（不带 scheme）
    nl_proxy_protocol_t protocol; // 反代上游协议
    struct nl_web_route* next;
} nl_web_route_t;

// ============ 上游 keep-alive 连接池（单线程引擎持有，无需加锁）============
// 空闲上游连接按 "host:port|proto" 分桶，取用时探测存活，空闲超时回收。
typedef struct nl_up_pool_entry {
    int fd;                    // 空闲上游连接 fd
    int64_t last_used_ms;      // 上次归还时间（用于空闲回收）
    struct nl_up_pool_entry* next;
} nl_up_pool_entry_t;
typedef struct {
    char  key[576];            // "host:port|proto"
    nl_up_pool_entry_t* head;
    int   count;
} nl_up_pool_bucket_t;
typedef struct {
    nl_up_pool_bucket_t* buckets;
    int bucket_cap;
    int total_idle;
    int max_total;             // 全局空闲上限，默认 256
    int max_per_key;           // 每 key 空闲上限，默认 8
    int idle_timeout_ms;       // 空闲超时，默认 30000
} nl_web_upstream_pool_t;

// 当前毫秒（单调时钟，用于空闲回收）
static int64_t pr_pool_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
// 关闭池中 fd（平台差异集中于此）
static void pr_pool_close_fd(int fd) { if (fd >= 0) close(fd); }
// 探测空闲上游连接是否仍存活：poll 无事件=健康；有可读/挂起事件=对端已关或有残留数据，丢弃
static int pr_pool_probe_alive(int fd) {
    if (fd < 0) return 0;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r = poll(&pfd, 1, 0);
    if (r < 0) return 0;
    return (r == 0);   // 0=无事件（健康）
}
// key 构造："host:port|proto"
static void pr_pool_make_key(char* out, size_t cap, const char* host, int port, int proto) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s:%d|%d", host ? host : "", port, proto);
}
// FNV-1a 哈希
static unsigned pr_pool_hash(const char* key) {
    unsigned h = 2166136261u;
    for (; *key; key++) { h ^= (unsigned char)*key; h *= 16777619u; }
    return h;
}
// 定位桶（create=1 时创建新键桶；线性探测解决碰撞）
static nl_up_pool_bucket_t* pr_pool_bucket(nl_web_upstream_pool_t* p, const char* key, int create) {
    if (!p || !p->buckets || p->bucket_cap <= 0) return NULL;
    int start = (int)(pr_pool_hash(key) % (unsigned)p->bucket_cap);
    for (int i = 0; i < p->bucket_cap; i++) {
        nl_up_pool_bucket_t* b = &p->buckets[(start + i) % p->bucket_cap];
        if (b->key[0] == '\0') {
            if (!create) return NULL;
            snprintf(b->key, sizeof(b->key), "%s", key);
            return b;
        }
        if (strcmp(b->key, key) == 0) return b;
    }
    return NULL;
}
// 初始化池
static void pool_init(nl_web_upstream_pool_t* p, int max_total, int max_per_key, int idle_timeout_ms) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->bucket_cap = 64;
    p->buckets = (nl_up_pool_bucket_t*)calloc((size_t)p->bucket_cap, sizeof(nl_up_pool_bucket_t));
    if (!p->buckets) { p->bucket_cap = 0; return; }
    p->max_total = max_total > 0 ? max_total : 256;
    p->max_per_key = max_per_key > 0 ? max_per_key : 8;
    p->idle_timeout_ms = idle_timeout_ms > 0 ? idle_timeout_ms : 30000;
}
// 取一条可用上游连接（探测存活，死连接丢弃）；无可用返回 -1
static int pool_get(nl_web_upstream_pool_t* p, const char* host, int port, int proto) {
    char key[576];
    pr_pool_make_key(key, sizeof(key), host, port, proto);
    nl_up_pool_bucket_t* b = pr_pool_bucket(p, key, 0);
    if (!b) return -1;
    nl_up_pool_entry_t** pp = &b->head;
    while (*pp) {
        nl_up_pool_entry_t* e = *pp;
        if (pr_pool_probe_alive(e->fd)) {
            *pp = e->next;
            int fd = e->fd;
            free(e);
            b->count--; p->total_idle--;
            return fd;
        }
        pr_pool_close_fd(e->fd);       // 死连接：丢弃
        *pp = e->next;
        free(e);
        b->count--; p->total_idle--;
    }
    return -1;
}
// 归还一条上游连接（超全局/单 key 上限则直接关闭）
static void pool_put(nl_web_upstream_pool_t* p, const char* key, int fd) {
    if (fd < 0) return;
    if (!p || !p->buckets || !key || key[0] == '\0' ||
        p->total_idle >= p->max_total) { pr_pool_close_fd(fd); return; }
    nl_up_pool_bucket_t* b = pr_pool_bucket(p, key, 1);
    if (!b || b->count >= p->max_per_key) { pr_pool_close_fd(fd); return; }
    nl_up_pool_entry_t* e = (nl_up_pool_entry_t*)malloc(sizeof(*e));
    if (!e) { pr_pool_close_fd(fd); return; }
    e->fd = fd;
    e->last_used_ms = pr_pool_now_ms();
    e->next = b->head;
    b->head = e;
    b->count++; p->total_idle++;
}
// 回收空闲超时连接
static void pool_reap(nl_web_upstream_pool_t* p, int64_t now_ms) {
    if (!p || !p->buckets) return;
    for (int i = 0; i < p->bucket_cap; i++) {
        nl_up_pool_bucket_t* b = &p->buckets[i];
        nl_up_pool_entry_t** pp = &b->head;
        while (*pp) {
            nl_up_pool_entry_t* e = *pp;
            if (now_ms - e->last_used_ms >= p->idle_timeout_ms) {
                pr_pool_close_fd(e->fd);
                *pp = e->next;
                free(e);
                b->count--; p->total_idle--;
            } else {
                pp = &e->next;
            }
        }
    }
}
// 销毁池（关闭全部空闲连接）
static void pool_destroy(nl_web_upstream_pool_t* p) {
    if (!p) return;
    if (p->buckets) {
        for (int i = 0; i < p->bucket_cap; i++) {
            nl_up_pool_entry_t* e = p->buckets[i].head;
            while (e) { nl_up_pool_entry_t* n = e->next; pr_pool_close_fd(e->fd); free(e); e = n; }
            p->buckets[i].head = NULL;
            p->buckets[i].key[0] = '\0';
            p->buckets[i].count = 0;
        }
        free(p->buckets);
        p->buckets = NULL;
    }
    p->bucket_cap = 0;
    p->total_idle = 0;
}

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
    // 工作池：accept 线程 + 动态 worker 线程池（堆分配，容量可运行时调整）
    pthread_t* workers;       // 动态分配，容量由 worker_capacity 决定
    int worker_capacity;      // workers 数组当前分配容量
    int worker_count;         // 实际已启动/在跑的 worker 数
    int worker_active;
    // 路由 hash 表（加速 O(n) -> O(1) 查找）
    nl_web_route_t** route_hash;
    int route_hash_size;
    int route_hash_count;
    // 数据驱动反向代理引擎（kqueue 多路复用 + ring buffer 状态表）：
    // 不为每个连接建对象，状态编码为 8 位无状态字节 + fd 映射整数数组。
    // 单线程 kevent 驱动 N 条 proxy 连接，彻底摆脱"并发≈worker 数"。
    int  kqfd;             // kqueue fd（-1=未启用）
    int *pr_fd_client;    // [idx] -> 客户端 fd
    int *pr_fd_upstream;  // [idx] -> 上游 fd（0=未连接）
    int *pr_route_idx;    // [idx] -> 该连接对应 route 在 server->routes 链表的下标
    uint8_t *pr_state;    // [idx] -> 无状态状态位（bit0 方向 / bit1 EOF / bit7 活跃）
    char** pr_hbuf;      // [idx] -> 每连接 8KB 首包缓冲（活跃时 malloc，归还即 free）
    char** pr_hbuf_out;  // [idx] -> 每连接 8KB 上游请求构造缓冲
    char** pr_up_buf;    // [idx] -> 每连接 64KB 上游响应→客户端方向缓冲（替共享 pump_buf）
    int *pr_hdr_len;     // [idx] -> 当前连接已收到的请求头累计字节数
    int *pr_remain;     // [idx] -> 该方向已收待发的剩余字节数（0=全部发完）
    int  pr_cap;        // ring 容量（按核数动态定，上限 4096）
    int  pr_count;      // 当前活动 proxy 连接数
    int  pr_free_top;   // 回收栈顶（O(1) 取 free slot）
    int *pr_free;       // 回收栈（int 数组，存空闲 idx）
    // ---- 上游 keep-alive 连接池（单线程引擎持有，无需锁）----
    nl_web_upstream_pool_t* up_pool;  // 空闲上游连接池（NULL=未启用）
    // 响应 framing 解析（每上游连接一份状态，旁路判定响应是否完整可复用）
    uint8_t *pr_up_parse;    // [idx] -> 解析状态（见 PR_UP_*）
    int64_t *pr_up_cl;       // [idx] -> 剩余 Content-Length
    int64_t *pr_chunk_rem;   // [idx] -> chunk 当前段剩余字节
    uint8_t *pr_chunk_state; // [idx] -> chunk 子状态（见 PR_CHUNK_*）
    int     *pr_up_hlen;     // [idx] -> 上游响应头/临时行缓冲已用长度
    uint8_t *pr_req_is_head; // [idx] -> 客户端请求是否 HEAD（无 body）
    char   **pr_up_key;      // [idx] -> 池 key（归还时使用，slot 归还即释放）
};

static struct nl_web_server* g_web_servers = NULL;
static pthread_mutex_t g_web_servers_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_auto_cleanup_enabled = 0;

// 路由 hash 表辅助函数（FNV-1a + 线性探测，桶存 route* 指针，不修改 route->next）
static const int NL_ROUTE_HASH_SIZE = 128;

static unsigned nl_route_hash(const char* path) {
    unsigned h = 2166136261u;
    for (; *path; path++) {
        h ^= (unsigned char)*path;
        h *= 16777619u;
    }
    return h % NL_ROUTE_HASH_SIZE;
}

static void nl_route_hash_init(nl_web_server_t* server) {
    if (server->route_hash) return;
    server->route_hash_size = NL_ROUTE_HASH_SIZE;
    server->route_hash_count = 0;
    server->route_hash = (nl_web_route_t**)calloc(NL_ROUTE_HASH_SIZE, sizeof(nl_web_route_t*));
}

static void nl_route_hash_insert_simple(nl_web_server_t* server, nl_web_route_t* route) {
    if (!server->route_hash) return;
    int start = (int)nl_route_hash(route->path);
    for (int i = 0; i < server->route_hash_size; i++) {
        int idx = (start + i) % server->route_hash_size;
        if (server->route_hash[idx] == NULL) {
            server->route_hash[idx] = route;
            server->route_hash_count++;
            return;
        }
        if (strcmp(server->route_hash[idx]->path, route->path) == 0) {
            server->route_hash[idx] = route;
            return;
        }
    }
}

static nl_web_route_t* nl_route_hash_lookup_simple(nl_web_server_t* server, const char* path) {
    if (!server->route_hash) return NULL;
    int start = (int)nl_route_hash(path);
    for (int i = 0; i < server->route_hash_size; i++) {
        int idx = (start + i) % server->route_hash_size;
        nl_web_route_t* r = server->route_hash[idx];
        if (r == NULL) return NULL;
        if (strcmp(r->path, path) == 0) return r;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// 数据驱动反向代理引擎（kqueue 版）
// ---------------------------------------------------------------------------
#define PR_ST_DIR_W   0x01   // 方向：等上游可读（写下游）
#define PR_ST_UP_EOF  0x02   // 上游已 EOF，写尽后关闭
#define PR_ST_ACTIVE  0x80   // 该槽活跃
#define PR_PROXY_BUF_SZ 65536
// 上游响应 framing 解析状态（pr_up_parse）
#define PR_UP_HDR     0  // 响应头未收全
#define PR_UP_CL      1  // 按 Content-Length 计 body
#define PR_UP_CHUNK   2  // Transfer-Encoding: chunked
#define PR_UP_DONE    3  // 完成，可复用
#define PR_UP_EOF     4  // 不可复用（body 以 EOF 结束 / Connection: close）
// chunked 子状态（pr_chunk_state）
#define PR_CHUNK_SIZE    0  // 读 chunk 大小行
#define PR_CHUNK_DATA    1  // 读 chunk 数据
#define PR_CHUNK_CRLF    2  // 读 chunk 数据后的 CRLF
#define PR_CHUNK_TRAILER 3  // 读 trailer / 终止空行
/* kqueue udata 编码：低 31 位=槽 idx，最高位=上游方向（1），客户端=0 */
#define PR_UDATA_UP_BIT 0x80000000ull
#define PR_UDATA(idx, up) ((up) ? ((uintptr_t)(idx) | PR_UDATA_UP_BIT) : (uintptr_t)(idx))

// 前置声明：引擎使用，定义见后文
static int nl_proxy_connect_upstream(const nl_web_route_t* route);

static int nl_web_proxy_ring_capacity(void) {
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    int cap = (int)ncpu * 64;
    if (cap > 4096) cap = 4096;
    if (cap < 64)  cap = 64;
    return cap;
}

static int pr_alloc_slot(nl_web_server_t* s) {
    if (s->pr_count >= s->pr_cap || s->pr_free_top < 0) return -1;
    int idx = s->pr_free[s->pr_free_top--];
    s->pr_count++;
    return idx;
}

static void pr_free_slot(nl_web_server_t* s, int idx) {
    if (idx < 0 || idx >= s->pr_cap) return;
    if (s->pr_hbuf[idx])     { free(s->pr_hbuf[idx]);     s->pr_hbuf[idx] = NULL; }
    if (s->pr_hbuf_out[idx]) { free(s->pr_hbuf_out[idx]); s->pr_hbuf_out[idx] = NULL; }
    if (s->pr_up_buf[idx])   { free(s->pr_up_buf[idx]);   s->pr_up_buf[idx] = NULL; }
    if (s->pr_up_key && s->pr_up_key[idx]) { free(s->pr_up_key[idx]); s->pr_up_key[idx] = NULL; }
    s->pr_state[idx] = 0;
    s->pr_fd_client[idx] = -1;
    s->pr_fd_upstream[idx] = 0;
    s->pr_route_idx[idx] = -1;
    if (s->pr_hdr_len) s->pr_hdr_len[idx] = 0;
    if (s->pr_remain) s->pr_remain[idx] = 0;
    if (s->pr_up_parse) s->pr_up_parse[idx] = PR_UP_HDR;
    if (s->pr_up_cl) s->pr_up_cl[idx] = 0;
    if (s->pr_chunk_rem) s->pr_chunk_rem[idx] = 0;
    if (s->pr_chunk_state) s->pr_chunk_state[idx] = 0;
    if (s->pr_up_hlen) s->pr_up_hlen[idx] = 0;
    if (s->pr_req_is_head) s->pr_req_is_head[idx] = 0;
    s->pr_free[++(s->pr_free_top)] = idx;
    if (s->pr_count > 0) s->pr_count--;
}

// 忽略大小写判断 [s, s+n) 是否以 prefix 开头（prefix 为小写字面量）
static int pr_ci_starts(const char* s, size_t n, const char* prefix) {
    size_t i = 0;
    for (; prefix[i]; i++) {
        if (i >= n) return 0;
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != prefix[i]) return 0;
    }
    return 1;
}
// 忽略大小写判断 [s, s+n) 是否包含 needle（needle 为小写字面量）
static int pr_ci_has(const char* s, size_t n, const char* needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 1;
    if (n < nl) return 0;
    for (size_t i = 0; i + nl <= n; i++) {
        size_t j = 0;
        for (; j < nl; j++) {
            char c = s[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != needle[j]) break;
        }
        if (j == nl) return 1;
    }
    return 0;
}
// 在 [s, s+n) 中查找 needle（可移植，替代非标准 memmem）
static const char* pr_memfind(const char* s, size_t n, const char* needle, size_t nl) {
    if (nl == 0) return s;
    if (n < nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++)
        if (s[i] == needle[0] && memcmp(s + i, needle, nl) == 0) return s + i;
    return NULL;
}

// 解析响应头块，确定 body framing 与是否可复用（结果写入 pr_up_parse 等字段）
static void pr_up_parse_headers(nl_web_server_t* s, int idx, const char* hdr, int hlen) {
    const char* end = hdr + hlen;
    // 状态行：第一个空格后 3 位为状态码
    int status = 0;
    const char* sp = (const char*)memchr(hdr, ' ', (size_t)hlen);
    if (sp && sp + 4 <= end) {
        status = (sp[1] - '0') * 100 + (sp[2] - '0') * 10 + (sp[3] - '0');
    }
    int no_body = (s->pr_req_is_head && s->pr_req_is_head[idx]) ||
                  (status >= 100 && status < 200) || status == 204 || status == 304;
    int conn_close = 0;
    long long cl = -1;
    int chunked = 0;
    const char* line = pr_memfind(hdr, (size_t)hlen, "\r\n", 2);
    if (line) line += 2; else line = end;
    while (line < end) {
        const char* le = pr_memfind(line, (size_t)(end - line), "\r\n", 2);
        if (!le) break;
        size_t ll = (size_t)(le - line);
        if (ll == 0) break;
        if (ll >= 11 && pr_ci_starts(line, ll, "connection:")) {
            if (pr_ci_has(line + 11, ll - 11, "close")) conn_close = 1;
        } else if (ll >= 15 && pr_ci_starts(line, ll, "content-length:")) {
            cl = atoll(line + 15);
        } else if (ll >= 18 && pr_ci_starts(line, ll, "transfer-encoding:")) {
            if (pr_ci_has(line + 18, ll - 18, "chunked")) chunked = 1;
        }
        line = le + 2;
    }
    if (conn_close) { s->pr_up_parse[idx] = PR_UP_EOF; return; }  // 对端将关闭 → 不可复用
    if (no_body)    { s->pr_up_parse[idx] = PR_UP_DONE; return; } // 无 body → 完成
    if (cl == 0)    { s->pr_up_parse[idx] = PR_UP_DONE; return; }
    if (cl > 0) {
        s->pr_up_cl[idx] = (int64_t)cl;
        s->pr_up_parse[idx] = PR_UP_CL;
        return;
    }
    if (chunked) {
        s->pr_chunk_state[idx] = PR_CHUNK_SIZE;
        s->pr_chunk_rem[idx] = 0;
        s->pr_up_hlen[idx] = 0;
        s->pr_up_parse[idx] = PR_UP_CHUNK;
        return;
    }
    // 无 Content-Length 且非 chunked：body 以 EOF 结束 → 不可复用
    s->pr_up_parse[idx] = PR_UP_EOF;
}

// 消费 chunked body 字节，推进至终止帧（0\r\n\r\n）后置 PR_UP_DONE
static void pr_up_parse_chunked(nl_web_server_t* s, int idx, const char* data, size_t* ppos, size_t len) {
    size_t pos = *ppos;
    while (pos < len) {
        uint8_t cs = s->pr_chunk_state[idx];
        if (cs == PR_CHUNK_SIZE || cs == PR_CHUNK_TRAILER) {
            // 累积一行到 pr_hbuf_out（临时行缓冲），pr_up_hlen 记长度
            char* lb = s->pr_hbuf_out[idx];
            int* ll = &s->pr_up_hlen[idx];
            while (pos < len) {
                char c = data[pos++];
                if (*ll < 8191) lb[(*ll)++] = c;
                else { s->pr_up_parse[idx] = PR_UP_EOF; *ppos = pos; return; }
                if (c == '\n') break;
            }
            if (*ll > 0 && lb[*ll - 1] == '\n') {
                int line_len = *ll;
                while (line_len > 0 && (lb[line_len - 1] == '\n' || lb[line_len - 1] == '\r')) line_len--;
                if (cs == PR_CHUNK_SIZE) {
                    long long sz = 0; int ok = 0;
                    for (int i = 0; i < line_len; i++) {
                        char c = lb[i];
                        int d;
                        if (c == ';') break;                       // chunk 扩展，忽略其后
                        else if (c >= '0' && c <= '9') d = c - '0';
                        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                        else if (c == ' ' || c == '\t') { if (!ok) continue; else break; }
                        else { s->pr_up_parse[idx] = PR_UP_EOF; *ll = 0; *ppos = pos; return; }
                        sz = sz * 16 + d; ok = 1;
                    }
                    *ll = 0;
                    if (sz == 0) { s->pr_chunk_state[idx] = PR_CHUNK_TRAILER; }
                    else { s->pr_chunk_rem[idx] = sz; s->pr_chunk_state[idx] = PR_CHUNK_DATA; }
                } else {
                    *ll = 0;
                    if (line_len == 0) { s->pr_up_parse[idx] = PR_UP_DONE; *ppos = pos; return; }
                }
            }
            continue;
        }
        if (cs == PR_CHUNK_DATA) {
            long long take = (long long)(len - pos);
            if (take > s->pr_chunk_rem[idx]) take = s->pr_chunk_rem[idx];
            pos += (size_t)take;
            s->pr_chunk_rem[idx] -= take;
            if (s->pr_chunk_rem[idx] == 0) { s->pr_chunk_state[idx] = PR_CHUNK_CRLF; s->pr_chunk_rem[idx] = 2; }
            continue;
        }
        if (cs == PR_CHUNK_CRLF) {
            long long take = (long long)(len - pos);
            if (take > s->pr_chunk_rem[idx]) take = s->pr_chunk_rem[idx];
            pos += (size_t)take;
            s->pr_chunk_rem[idx] -= take;
            if (s->pr_chunk_rem[idx] == 0) s->pr_chunk_state[idx] = PR_CHUNK_SIZE;
            continue;
        }
        break;
    }
    *ppos = pos;
}

// 旁路解析被转发的上游字节（bytes 方向：上游→客户端），判定响应是否完整可复用。
// 绝不修改 bytes（转发必须字节级原样）。
static void pr_parse_upstream_bytes(nl_web_server_t* s, int idx, const char* data, size_t len) {
    if (!s || !s->pr_up_parse || !s->pr_hbuf_out || !data || len == 0) return;
    size_t pos = 0;
    while (pos < len) {
        uint8_t st = s->pr_up_parse[idx];
        if (st == PR_UP_DONE || st == PR_UP_EOF) return;  // 终态：停止解析
        if (st == PR_UP_HDR) {
            char* hb = s->pr_hbuf_out[idx];
            int* hl = &s->pr_up_hlen[idx];
            int found = 0;
            while (pos < len) {
                if (*hl >= 8190) { s->pr_up_parse[idx] = PR_UP_EOF; return; }  // 头过大：判不可复用
                char c = data[pos++];
                hb[(*hl)++] = c;
                if (*hl >= 4 && hb[*hl - 4] == '\r' && hb[*hl - 3] == '\n' &&
                    hb[*hl - 2] == '\r' && hb[*hl - 1] == '\n') { found = 1; break; }
            }
            if (found) { pr_up_parse_headers(s, idx, hb, *hl); *hl = 0; }
            continue;
        }
        if (st == PR_UP_CL) {
            long long rem = s->pr_up_cl[idx];
            long long take = (long long)(len - pos);
            if (take >= rem) { pos += (size_t)rem; s->pr_up_cl[idx] = 0; s->pr_up_parse[idx] = PR_UP_DONE; }
            else { s->pr_up_cl[idx] = rem - take; pos = len; }
            continue;
        }
        if (st == PR_UP_CHUNK) { pr_up_parse_chunked(s, idx, data, &pos, len); continue; }
        return;
    }
}

static int pr_lookup_route_idx(nl_web_server_t* s, const char* path) {
    int idx = 0;
    pthread_mutex_lock(&s->mutex);
    for (nl_web_route_t* r = s->routes; r; r = r->next, idx++) {
        if (strcmp(r->path, path) == 0) {
            pthread_mutex_unlock(&s->mutex);
            return idx;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return -1;
}

// 把 proxy 连接注册进 kqueue：客户端 + 上游双 fd，EVFILT_READ
// kqueue 在 close(fd) 时自动 detach 对应 event，无需 EVFILT_READ + EV_DELETE。
static void pr_register(nl_web_server_t* s, int idx, int cfd, int ufd) {
    struct kevent ev;
    EV_SET(&ev, cfd, EVFILT_READ, EV_ADD, 0, 0, (void*)PR_UDATA(idx, 0));
    kevent(s->kqfd, &ev, 1, NULL, 0, NULL);
    if (ufd > 0) {
        EV_SET(&ev, ufd, EVFILT_READ, EV_ADD, 0, 0, (void*)PR_UDATA(idx, 1));
        kevent(s->kqfd, &ev, 1, NULL, 0, NULL);
    }
}

// 非阻塞泵：先补发上次未发完的余量（pr_remain），再读新数据转发。
// 返回值：1=还有数据（EAGAIN/读满），0=对端 EOF，-1=错误。
// 调用方需传入 per-slot 独立缓冲（upstream→client 方向用 pr_up_buf[idx]，
// client→upstream 方向用 pr_hbuf[idx]），避免多连接并发时数据串包。
// 每次 send 后若只发了 w 字节，剩余数据 memmove 前移至 buf[0..remain-w]，
// 确保下次从 buf[0] 继续发，避免重复发送已发数据。
static int pr_pump_once(nl_web_server_t* s, int idx, int from, int to, char* buf, size_t cap, int parse_up) {
    int* remain = s->pr_remain;
    // 先补发上次未发完的余量
    while (remain && remain[idx] > 0) {
        ssize_t w = send(to, buf, (size_t)remain[idx], 0);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;  // 写满，等 EVFILT_WRITE 再补
            return -1;
        }
        if (w == 0) return 0;
        // 只发了 w 字节：剩余前移，下次 send 从 buf[0] 继续
        if (w > 0 && w < remain[idx])
            memmove(buf, buf + w, (size_t)(remain[idx] - w));
        remain[idx] -= (int)w;
    }
    // 再读新数据
    ssize_t n = recv(from, buf, cap, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;
        return -1;
    }
    if (n == 0) {
        // 对端 EOF；若还有余量未发完，标记"有待发"，调用方下轮继续补
        if (remain && remain[idx] > 0) return 1;
        return 0;
    }
    // 上游→客户端方向：把新读到的 n 字节旁路喂给 framing 解析器（不修改 buf）
    if (parse_up) pr_parse_upstream_bytes(s, idx, buf, (size_t)n);
    remain[idx] = (int)n;
    while (remain[idx] > 0) {
        ssize_t w = send(to, buf, (size_t)remain[idx], 0);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;  // 写满，留待 EVFILT_WRITE 补发
            return -1;
        }
        if (w == 0) return 0;
        // 只发了 w 字节：剩余前移
        if (w > 0 && w < remain[idx])
            memmove(buf, buf + w, (size_t)(remain[idx] - w));
        remain[idx] -= (int)w;
    }
    return 1;  // 还有更多数据可读（recv 写满）
}

// 构造上游请求，输出到 out，返回字节数（-1=失败）
static int pr_build_upstream_http(nl_web_server_t* s, int idx,
                                  const char* method, const char* path,
                                  const char* proto,
                                  const char* raw_hdrs, size_t raw_hdrs_len,
                                  char* out, size_t out_cap) {
    nl_web_route_t* r;
    pthread_mutex_lock(&s->mutex);
    int rd = s->pr_route_idx[idx];
    r = NULL;
    int cur = 0;
    for (nl_web_route_t* it = s->routes; it; it = it->next, cur++) {
        if (cur == rd) { r = it; break; }
    }
    if (rd < 0 || !r) { pthread_mutex_unlock(&s->mutex); return -1; }
    int total = 0;
    if (r->protocol == NL_PROXY_TCP) {
        total = (int)raw_hdrs_len;
        if (total > 0 && total < (int)out_cap) memcpy(out, raw_hdrs, total);
        else total = 0;
    } else {
        total = snprintf(out, out_cap,
                         "%s %s %s\r\nHost: %s\r\nX-Forwarded-For: 127.0.0.1\r\nConnection: keep-alive\r\n",
                         method, path, proto, r->upstream);
        if ((size_t)total >= out_cap) total = (int)out_cap - 1;
        if (raw_hdrs_len > 0 && (size_t)total + raw_hdrs_len < out_cap) {
            memcpy(out + total, raw_hdrs, raw_hdrs_len);
            total += (int)raw_hdrs_len;
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return total;
}

// kqueue 数据驱动主循环：单线程 kevent 驱动 N 条 proxy 连接
// 通过 kevent(256) + 1000ms 超时实现极低占用（空闲 1s 退避）
// 上游侧用 data->ident 标记（高位 0x40000000），listen fd 用哨兵 0xFFFFFFFF
void nl_web_proxy_engine(nl_web_server_t* s, int listen_fd) {
    s->kqfd = kqueue();
    if (s->kqfd < 0) return;

    int cap = nl_web_proxy_ring_capacity();
    s->pr_cap = cap;
    s->pr_fd_client   = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_fd_upstream = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_route_idx   = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_state       = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_hbuf        = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_hbuf_out    = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_up_buf      = (char**)calloc((size_t)cap, sizeof(char*));
    s->pr_hdr_len     = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_remain      = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_free        = (int*)malloc((size_t)cap * sizeof(int));
    // 上游 framing 解析 + 连接池相关数组
    s->pr_up_parse    = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_up_cl       = (int64_t*)calloc((size_t)cap, sizeof(int64_t));
    s->pr_chunk_rem   = (int64_t*)calloc((size_t)cap, sizeof(int64_t));
    s->pr_chunk_state = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_up_hlen     = (int*)calloc((size_t)cap, sizeof(int));
    s->pr_req_is_head = (uint8_t*)calloc((size_t)cap, 1);
    s->pr_up_key      = (char**)calloc((size_t)cap, sizeof(char*));
    for (int i = 0; i < cap; i++) s->pr_free[i] = cap - 1 - i;
    s->pr_free_top = cap - 1;
    s->pr_count = 0;
    if (!s->pr_fd_client || !s->pr_fd_upstream || !s->pr_route_idx ||
        !s->pr_state || !s->pr_hbuf || !s->pr_hbuf_out || !s->pr_up_buf ||
        !s->pr_hdr_len || !s->pr_remain || !s->pr_free ||
        !s->pr_up_parse || !s->pr_up_cl || !s->pr_chunk_rem ||
        !s->pr_chunk_state || !s->pr_up_hlen || !s->pr_req_is_head ||
        !s->pr_up_key) {
        close(s->kqfd); s->kqfd = -1;
        return;
    }
    // 初始化上游 keep-alive 连接池（单线程持有，无需锁）
    s->up_pool = (nl_web_upstream_pool_t*)malloc(sizeof(nl_web_upstream_pool_t));
    if (s->up_pool) pool_init(s->up_pool, 256, 8, 30000);

    // 注册 listen fd：udata 哨兵 = PR_UDATA(0,0)=0 会误判，改用高位编码 client 方向
    // kqueue EVFILT_READ 在 close 时自动 detach，无需手动 EV_DELETE
    struct kevent lev;
    EV_SET(&lev, listen_fd, EVFILT_READ, EV_ADD, 0, 0,
           (void*)((uintptr_t)0x7FFFFFFF));  /* listen 哨兵：client 方向 idx=0x7FFFFFF */
    kevent(s->kqfd, &lev, 1, NULL, 0, NULL);

    // 设 listen fd 非阻塞
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    struct kevent events[256];

    while (s->running) {
        struct timespec timeout;
        timeout.tv_sec = 1;
        timeout.tv_nsec = 0;
        int n = kevent(s->kqfd, NULL, 0, events, 256, &timeout);
        if (n < 0) { if (errno == EINTR) continue; break; }
        pool_reap(s->up_pool, pr_pool_now_ms());   // 每轮回收空闲超时上游连接
        for (int i = 0; i < n; i++) {
            uint64_t udata = (uint64_t)(uintptr_t)events[i].ud;
            int is_up      = (udata & PR_UDATA_UP_BIT) ? 1 : 0;
            int idx        = (int)(udata & ~PR_UDATA_UP_BIT);
            if (idx == 0x7FFFFFF) {
                // accept 客户端连接
                int cfd = accept(listen_fd, NULL, NULL);
                if (cfd < 0) continue;
                int cf = fcntl(cfd, F_GETFL, 0);
                fcntl(cfd, F_SETFL, cf | O_NONBLOCK);
                int si = pr_alloc_slot(s);
                if (si < 0) { close(cfd); continue; }
                s->pr_fd_client[si] = cfd;
                s->pr_state[si] = PR_ST_ACTIVE;
                // 每连接独立 8KB 首包缓冲 + 8KB 上游请求构造缓冲 + 64KB 上游响应缓冲（归还即释放）
                s->pr_hbuf[si]     = (char*)malloc(8192);
                s->pr_hbuf_out[si] = (char*)malloc(8192);
                s->pr_up_buf[si]   = (char*)malloc(PR_PROXY_BUF_SZ);
                s->pr_up_key[si]   = (char*)malloc(576);
                s->pr_hdr_len[si]  = 0;
                s->pr_remain[si]   = 0;
                // 重置上游响应 framing 状态
                s->pr_up_parse[si]    = PR_UP_HDR;
                s->pr_up_cl[si]       = 0;
                s->pr_chunk_rem[si]   = 0;
                s->pr_chunk_state[si] = 0;
                s->pr_up_hlen[si]     = 0;
                s->pr_req_is_head[si] = 0;
                if (!s->pr_hbuf[si] || !s->pr_hbuf_out[si] || !s->pr_up_buf[si] || !s->pr_up_key[si]) {
                    close(cfd);
                    pr_free_slot(s, si);
                    continue;
                }
                struct kevent ev;
                EV_SET(&ev, cfd, EVFILT_READ, EV_ADD, 0, 0, (void*)PR_UDATA(si, 0));
                kevent(s->kqfd, &ev, 1, NULL, 0, NULL);
                continue;
            }
            if (idx >= s->pr_cap || !(s->pr_state[idx] & PR_ST_ACTIVE)) continue;
            int cfd = s->pr_fd_client[idx];
            int ufd = s->pr_fd_upstream[idx];
            if (cfd <= 0) continue;

            if (!is_up) {
                // 客户端可写：补发上游→客户端方向未发完的余量（客户端写慢导致 send EAGAIN）
                if (events[i].filter == EVFILT_WRITE) {
                    int* remain = s->pr_remain;
                    int werr = 0;
                    while (remain && remain[idx] > 0) {
                        ssize_t w = send(cfd, s->pr_up_buf[idx], (size_t)remain[idx], 0);
                        if (w < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 仍写满，保持注册
                            werr = 1; break;
                        }
                        if (w == 0) break;
                        // 若只发了 w 字节，剩余前移消除空洞（下次 send 从 buf[0] 发新数据）
                        if (w > 0 && w < remain[idx])
                            memmove(s->pr_up_buf[idx], s->pr_up_buf[idx] + w, (size_t)(remain[idx] - w));
                        remain[idx] -= (int)w;
                    }
                    if (werr) {
                        // 写错误：连接不可用，关闭两端（close 自动 detach）
                        if (s->pr_fd_upstream[idx] > 0) close(s->pr_fd_upstream[idx]);
                        close(cfd);
                        pr_free_slot(s, idx);
                        continue;
                    }
                    if (remain[idx] == 0) {
                        // 余量排空：摘除客户端写事件
                        struct kevent dwv;
                        EV_SET(&dwv, cfd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
                        kevent(s->kqfd, &dwv, 1, NULL, 0, NULL);
                        // 排空后立即做池化完成判定（与既有完成分支一致）
                        if (s->pr_up_parse[idx] == PR_UP_DONE) {
                            int ufd2 = s->pr_fd_upstream[idx];
                            struct kevent dev;
                            EV_SET(&dev, ufd2, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                            kevent(s->kqfd, &dev, 1, NULL, 0, NULL);
                            if (s->up_pool && s->pr_up_key[idx] && s->pr_up_key[idx][0])
                                pool_put(s->up_pool, s->pr_up_key[idx], ufd2);
                            else
                                close(ufd2);
                            close(cfd);        // close 自动 detach kqueue
                            pr_free_slot(s, idx);
                            continue;
                        }
                    }
                    // kqueue 单事件单 filter：可读事件会作为独立 kevent 返回，无需再处理读
                    continue;
                }
                // 客户端侧：收请求
                char*  hbuf = s->pr_hbuf[idx];
                int*   hlen = &s->pr_hdr_len[idx];
                int maxsz = 8192;
                ssize_t nread = recv(cfd, hbuf + *hlen, (size_t)(maxsz - *hlen), 0);
                if (nread <= 0) {
                    if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    // 客户端 EOF：清理双 fd（kqueue close 自动 detach）
                    if (s->pr_fd_upstream[idx] > 0) close(s->pr_fd_upstream[idx]);
                    close(cfd);
                    pr_free_slot(s, idx);
                    continue;
                }
                *hlen += (int)nread;
                hbuf[*hlen] = '\0';

                if (s->pr_fd_upstream[idx] == 0) {
                    char* line_end = memchr(hbuf, '\n', (size_t)*hlen);
                    if (!line_end) continue;
                    *line_end = '\0';
                    char method[16] = {0}, path[4096] = {0}, proto[32] = {0};
                    char* sp1 = strchr(hbuf, ' ');
                    if (!sp1) { send_http_error(cfd, 400, "Bad Request"); close(cfd); pr_free_slot(s, idx); continue; }
                    char* sp2 = strchr(sp1 + 1, ' ');
                    if (!sp2) { send_http_error(cfd, 400, "Bad Request"); close(cfd); pr_free_slot(s, idx); continue; }
                    size_t mlen = sp1 - hbuf;
                    size_t plen = sp2 - (sp1 + 1);
                    size_t vlen = line_end - (sp2 + 1);
                    if (mlen == 0 || mlen >= sizeof(method) ||
                        plen == 0 || plen >= sizeof(path) ||
                        vlen == 0 || vlen >= sizeof(proto)) {
                        send_http_error(cfd, 400, "Bad Request");
                        close(cfd);
                        pr_free_slot(s, idx);
                        continue;
                    }
                    memcpy(method, hbuf, mlen);
                    memcpy(path, sp1 + 1, plen);
                    memcpy(proto, sp2 + 1, vlen);
                    *hlen = 0;

                    int ridx = pr_lookup_route_idx(s, path);
                    nl_web_route_t* r = NULL;
                    pthread_mutex_lock(&s->mutex);
                    int cur = 0;
                    for (nl_web_route_t* it = s->routes; it; it = it->next, cur++)
                        if (cur == ridx) { r = it; break; }
                    pthread_mutex_unlock(&s->mutex);
                    if (!r) {
                        send_http_error(cfd, 404, "Not Found");
                        close(cfd);
                        pr_free_slot(s, idx);
                        continue;
                    }
                    // 完整路由分发（解决混合路由限制：引擎同时处理 PROXY + CONTENT + FILE + REDIRECT）
                    // 非 proxy 路由：临时切阻塞（复用 worker 池已验证的 send_http_response / redirect 模板），
                    // 处理完直接 close + free slot；proxy 路由保持非阻塞走引擎 pump 链路。
                    if (r->type != NL_ROUTE_TYPE_PROXY) {
                        // 临时切阻塞：去掉 O_NONBLOCK
                        int fl_tmp = fcntl(cfd, F_GETFL, 0);
                        fcntl(cfd, F_SETFL, fl_tmp & ~O_NONBLOCK);

                        if (r->type == NL_ROUTE_TYPE_REDIRECT) {
                            char resp[1024];
                            snprintf(resp, sizeof(resp),
                                "HTTP/1.1 302 Found\r\n"
                                "Location: %s\r\n"
                                "Content-Length: 0\r\n"
                                "Connection: close\r\n\r\n",
                                r->redirect_url);
                            write(cfd, resp, strlen(resp));
                        }
                        else if (r->type == NL_ROUTE_TYPE_FILE) {
                            FILE* fp = fopen(r->file_path, "rb");
                            if (!fp) {
                                send_http_error(cfd, 404, "File Not Found");
                            } else {
                                fseek(fp, 0, SEEK_END);
                                long fsz = ftell(fp);
                                fseek(fp, 0, SEEK_SET);
                                if (fsz > 0 && fsz <= 10*1024*1024) {
                                    char* fc = (char*)malloc(fsz + 1);
                                    if (fc) {
                                        size_t rs = fread(fc, 1, fsz, fp);
                                        fc[rs] = '\0';
                                        send_http_response(cfd, r->content_type, fc, rs);
                                        free(fc);
                                    } else {
                                        send_http_error(cfd, 500, "Memory Error");
                                    }
                                } else {
                                    send_http_error(cfd, 500, "File Too Large");
                                }
                                fclose(fp);
                            }
                        }
                        else { // NL_ROUTE_TYPE_CONTENT
                            send_http_response(cfd, r->content_type,
                                              r->content, strlen(r->content));
                        }
                        close(cfd);   // close 自动 detach kqueue
                        pr_free_slot(s, idx);
                        continue;
                    }
                    // proxy 路由：连接上游（优先复用池中空闲连接，未命中再新建）
                    s->pr_req_is_head[idx] = (strcmp(method, "HEAD") == 0) ? 1 : 0;
                    char up_host[512];
                    int  up_port = 0;
                    {
                        char uc[512];
                        snprintf(uc, sizeof(uc), "%s", r->upstream);
                        char* colon = strrchr(uc, ':');
                        if (colon) { *colon = '\0'; up_port = atoi(colon + 1); snprintf(up_host, sizeof(up_host), "%s", uc); }
                        else { up_port = (r->protocol == NL_PROXY_HTTPS) ? 443 : 80; snprintf(up_host, sizeof(up_host), "%s", uc); }
                        if (up_port <= 0) up_port = (r->protocol == NL_PROXY_HTTPS) ? 443 : 80;
                    }
                    int nfdu = -1;
                    if (r->protocol != NL_PROXY_TCP && s->up_pool)
                        nfdu = pool_get(s->up_pool, up_host, up_port, (int)r->protocol);
                    if (nfdu < 0) nfdu = nl_proxy_connect_upstream(r);
                    if (nfdu < 0) {
                        send_http_error(cfd, 502, "Bad Gateway");
                        close(cfd);
                        pr_free_slot(s, idx);
                        continue;
                    }
                    s->pr_fd_upstream[idx] = nfdu;
                    // 记录池 key（tcp 不参与池，置空），响应完成归还池时使用
                    if (s->pr_up_key[idx]) {
                        if (r->protocol != NL_PROXY_TCP)
                            pr_pool_make_key(s->pr_up_key[idx], 576, up_host, up_port, (int)r->protocol);
                        else
                            s->pr_up_key[idx][0] = '\0';
                    }
                    int uf_flags = fcntl(nfdu, F_GETFL, 0);
                    fcntl(nfdu, F_SETFL, uf_flags | O_NONBLOCK);
                    s->pr_route_idx[idx] = ridx;

                    char* ubuf = s->pr_hbuf_out[idx];
                    int build_n = pr_build_upstream_http(s, idx, method, path, proto, NULL, 0, ubuf, 8192);
                    if (build_n > 0) {
                        ssize_t off = 0;
                        while (off < build_n) {
                            ssize_t w = send(nfdu, ubuf + off, (size_t)(build_n - off), 0);
                            if (w < 0) break;
                            off += w;
                        }
                    }
                    struct kevent uev;
                    EV_SET(&uev, nfdu, EVFILT_READ, EV_ADD, 0, 0, (void*)PR_UDATA(idx, 1));
                    kevent(s->kqfd, &uev, 1, NULL, 0, NULL);
                    s->pr_state[idx] = PR_ST_DIR_W | PR_ST_ACTIVE;
                    continue;
                }

                // 后续客户端数据（body 等）→ 非阻塞转发上游（该方向不解析）
                int rc = pr_pump_once(s, idx, cfd, s->pr_fd_upstream[idx], hbuf, 8192, 0);
                if (rc < 0) {
                    if (s->pr_fd_upstream[idx] > 0) close(s->pr_fd_upstream[idx]);
                    close(cfd);
                    pr_free_slot(s, idx);
                }
                continue;
            }

            // 上游侧：读响应 → 写客户端（该方向旁路喂 framing 解析器）
            int rc = pr_pump_once(s, idx, ufd, cfd, s->pr_up_buf[idx], PR_PROXY_BUF_SZ, 1);
            if (rc == 0 || rc < 0) {
                // 上游 EOF / 错误：连接已不可复用，关闭两端
                s->pr_state[idx] |= PR_ST_UP_EOF;
                if (s->pr_fd_upstream[idx] > 0) close(s->pr_fd_upstream[idx]);
                close(cfd);
                pr_free_slot(s, idx);
                continue;
            }
            // 客户端写慢（send EAGAIN）导致仍有未发完的响应余量：
            // 给客户端 fd 注册 EVFILT_WRITE，等可写事件唤醒补发
            if (rc == 1 && s->pr_remain[idx] > 0) {
                struct kevent wev;
                EV_SET(&wev, cfd, EVFILT_WRITE, EV_ADD, 0, 0, (void*)PR_UDATA(idx, 0));
                kevent(s->kqfd, &wev, 1, NULL, 0, NULL);
            }
            // 响应 framing 判定完成且已全部转发客户端 → 上游归还池，关闭客户端
            if (s->pr_up_parse[idx] == PR_UP_DONE && s->pr_remain[idx] == 0) {
                int ufd2 = s->pr_fd_upstream[idx];
                // kqueue EV_DELETE 摘除上游读事件（不 close），供池复用
                struct kevent dev;
                EV_SET(&dev, ufd2, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                kevent(s->kqfd, &dev, 1, NULL, 0, NULL);
                if (s->up_pool && s->pr_up_key[idx] && s->pr_up_key[idx][0])
                    pool_put(s->up_pool, s->pr_up_key[idx], ufd2);
                else
                    close(ufd2);   // 无池/tcp：退回关闭
                close(cfd);        // close 自动 detach kqueue
                pr_free_slot(s, idx);
            }
        }
    }

    // 清理所有活动连接
    for (int i = 0; i < s->pr_cap; i++) {
        if (s->pr_state[i] & PR_ST_ACTIVE) {
            if (s->pr_fd_client[i] > 0) close(s->pr_fd_client[i]);
            if (s->pr_fd_upstream[i] > 0) close(s->pr_fd_upstream[i]);
            s->pr_state[i] = 0;
        }
        if (s->pr_hbuf[i])     free(s->pr_hbuf[i]);
        if (s->pr_hbuf_out[i]) free(s->pr_hbuf_out[i]);
        if (s->pr_up_buf[i])   free(s->pr_up_buf[i]);
        if (s->pr_up_key && s->pr_up_key[i]) free(s->pr_up_key[i]);
    }
    // 销毁上游连接池（关闭全部空闲上游连接）
    if (s->up_pool) { pool_destroy(s->up_pool); free(s->up_pool); s->up_pool = NULL; }
    close(s->kqfd);
    s->kqfd = -1;
    free(s->pr_fd_client);   free(s->pr_fd_upstream);   free(s->pr_route_idx);
    free(s->pr_state);       free(s->pr_hbuf);          free(s->pr_hbuf_out);
    free(s->pr_up_buf);
    free(s->pr_hdr_len);     free(s->pr_remain);        free(s->pr_free);
    free(s->pr_up_parse);    free(s->pr_up_cl);         free(s->pr_chunk_rem);
    free(s->pr_chunk_state); free(s->pr_up_hlen);       free(s->pr_req_is_head);
    free(s->pr_up_key);
    s->pr_fd_client = NULL;
    s->pr_fd_upstream = NULL;
    s->pr_route_idx = NULL;
    s->pr_state = NULL;
    s->pr_hbuf = NULL;
    s->pr_hbuf_out = NULL;
    s->pr_up_buf = NULL;
    s->pr_hdr_len = NULL;
    s->pr_remain = NULL;
    s->pr_free = NULL;
    s->pr_up_parse = NULL;
    s->pr_up_cl = NULL;
    s->pr_chunk_rem = NULL;
    s->pr_chunk_state = NULL;
    s->pr_up_hlen = NULL;
    s->pr_req_is_head = NULL;
    s->pr_up_key = NULL;
    s->pr_cap = s->pr_count = 0;
}

// 引擎线程入口
static void* nl_web_proxy_engine_thread(void* arg) {
    nl_web_server_t* s = (nl_web_server_t*)arg;
    nl_web_proxy_engine(s, s->sock);
    return NULL;
}

static void nl_route_hash_rebuild(nl_web_server_t* server) {
    if (!server->route_hash) nl_route_hash_init(server);
    for (int i = 0; i < server->route_hash_size; i++) server->route_hash[i] = NULL;
    server->route_hash_count = 0;
    for (nl_web_route_t* r = server->routes; r; r = r->next) nl_route_hash_insert_simple(server, r);
}

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

// 连接上游服务器，成功返回套接字，失败返回 -1。
// 依据 route 中已解析好的 upstream(host:port) 与 protocol 建立 TCP/TLS 连接。
static int nl_proxy_connect_upstream(const nl_web_route_t* route) {
    char host[512];
    int port = 0;
    nl_proxy_protocol_t proto = route->protocol;

    char upstream_copy[512];
    snprintf(upstream_copy, sizeof(upstream_copy), "%s", route->upstream);
    char* colon = strrchr(upstream_copy, ':');
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
        snprintf(host, sizeof(host), "%s", upstream_copy);
    } else {
        port = (proto == NL_PROXY_HTTPS) ? 443 : 80;
        snprintf(host, sizeof(host), "%s", upstream_copy);
    }
    if (port <= 0) port = (proto == NL_PROXY_HTTPS) ? 443 : 80;

    struct sockaddr_in up_addr;
    memset(&up_addr, 0, sizeof(up_addr));
    up_addr.sin_family = AF_INET;
    up_addr.sin_port = htons((unsigned short)port);

    struct hostent* he = gethostbyname(host);
    if (he && he->h_addr_list[0]) {
        memcpy(&up_addr.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));
    } else {
        if (inet_pton(AF_INET, host, &up_addr.sin_addr) != 1) {
            up_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        }
    }

    int up_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (up_sock < 0) return -1;

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(up_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(up_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(up_sock, (struct sockaddr*)&up_addr, sizeof(up_addr)) < 0) {
        close(up_sock);
        return -1;
    }

    // 完成反代：HTTPS 上游需要 TLS 握手。优先通过扩展系统动态发现 TLS 握手钩子，
    // 取代原先依赖 #ifdef NL_HTTPS_ENABLE 的编译期死代码（核心库默认不定义该宏）。
    if (proto == NL_PROXY_HTTPS) {
        int tls_ok = 0;
#ifdef NL_HTTPS_ENABLE
        extern int nl_tls_handshake(int sock, const char* host);
        tls_ok = nl_tls_handshake(up_sock, host);
#else
        extern void* nl_extension_get_func_by_id(const char* ext_id, const char* func_name);
        void* handshake_fn = nl_extension_get_func_by_id("https", "nl_tls_handshake");
        if (handshake_fn) {
            typedef int (*nl_tls_handshake_fn)(int, const char*);
            tls_ok = ((nl_tls_handshake_fn)handshake_fn)(up_sock, host);
        }
#endif
        if (!tls_ok) {
            printf("[NetLeaf] proxy: TLS not available for upstream %s, falling back to plaintext\n", host);
        }
    }

    return up_sock;
}

static void nl_handle_client(nl_web_server_t* server, int client) {
    char buffer[8192];
    ssize_t received = recv(client, buffer, sizeof(buffer) - 1, 0);
    
    if (received <= 0) {
        close(client);
        return;
    }
    
    buffer[received] = '\0';

    /* 取长补短：核心库旧路径原先用 sscanf 粗糙解析（无法提取请求头），
     * 此处改用与优化层一致的"首行三段 + 请求头"结构化解析，得到
     * method/path/version 及完整请求头，同时保留核心库特有的
     * 路由表/反代/静态文件优势。解析失败则返回 400。 */
    char method[16], path[4096], protocol[32];
    method[0] = path[0] = protocol[0] = '\0';
    {
        char* line_end = memchr(buffer, '\n', (size_t)received);
        if (!line_end || line_end == buffer) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            return;
        }
        *line_end = '\0';
        char* sp1 = strchr(buffer, ' ');
        if (!sp1 || sp1 == buffer) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            return;
        }
        *sp1 = '\0';
        char* sp2 = strchr(sp1 + 1, ' ');
        if (!sp2) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            return;
        }
        size_t mlen = sp1 - buffer;
        size_t plen = sp2 - (sp1 + 1);
        size_t vlen = (size_t)(line_end - (sp2 + 1));
        if (mlen == 0 || mlen >= sizeof(method) || plen == 0 || plen >= sizeof(path) ||
            vlen == 0 || vlen >= sizeof(protocol)) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            return;
        }
        memcpy(method, buffer, mlen);
        method[mlen] = '\0';
        memcpy(path, sp1 + 1, plen);
        path[plen] = '\0';
        memcpy(protocol, sp2 + 1, vlen);
        protocol[vlen] = '\0';
    }

    pthread_mutex_lock(&server->mutex);
    // 使用 hash 表 O(1) 查找路由；若 hash 未初始化则回退到线性链表
    nl_web_route_t* route = nl_route_hash_lookup_simple(server, path);
    if (!route) {
        for (nl_web_route_t* r = server->routes; r; r = r->next) {
            if (strcmp(r->path, path) == 0) { route = r; break; }
        }
    }
    if (route) {
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
                return;
            }
            else if (route->type == NL_ROUTE_TYPE_FILE) {
                // Hot reload: read file content on each request
                pthread_mutex_unlock(&server->mutex);
                
                FILE* fp = fopen(route->file_path, "rb");
                if (!fp) {
                    send_http_error(client, 404, "File Not Found");
                    close(client);
                    return;
                }
                
                fseek(fp, 0, SEEK_END);
                long file_size = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                
                if (file_size <= 0 || file_size > 10 * 1024 * 1024) {
                    fclose(fp);
                    send_http_error(client, 500, "File Too Large");
                    close(client);
                    return;
                }
                
                char* file_content = (char*)malloc(file_size + 1);
                if (!file_content) {
                    fclose(fp);
                    send_http_error(client, 500, "Memory Error");
                    close(client);
                    return;
                }
                
                size_t read_size = fread(file_content, 1, file_size, fp);
                fclose(fp);
                file_content[read_size] = '\0';
                
                send_http_response(client, route->content_type, file_content, read_size);
                free(file_content);
                close(client);
                return;
            }
            else if (route->type == NL_ROUTE_TYPE_PROXY) {
                // 反向代理：连接上游并双向透传
                pthread_mutex_unlock(&server->mutex);
                int up_sock = nl_proxy_connect_upstream(route);
                if (up_sock < 0) {
                    send_http_error(client, 502, "Bad Gateway");
                    close(client);
                    return;
                }

                // 高压优化：代理双向透传缓冲堆分配一次复用，避免 16KB 栈占用。
                char* proxy_buf = (char*)malloc(65536);
                if (!proxy_buf) {
                    close(up_sock);
                    send_http_error(client, 500, "Memory Error");
                    close(client);
                    return;
                }

                // 高压优化：TCP 上游加 5s 超时，避免上游无响应时 worker 线程被无限占用。
                struct timeval up_tv;
                up_tv.tv_sec = 5;
                up_tv.tv_usec = 0;
                setsockopt(up_sock, SOL_SOCKET, SO_RCVTIMEO, &up_tv, sizeof(up_tv));
                setsockopt(up_sock, SOL_SOCKET, SO_SNDTIMEO, &up_tv, sizeof(up_tv));

                if (route->protocol == NL_PROXY_TCP) {
                    /* 完成反代：纯字节透传（TCP 语义），不构造 HTTP 请求行。 */
                    ssize_t sent = send(up_sock, buffer, (size_t)received, 0);
                    if (sent <= 0) {
                        free(proxy_buf);
                        close(up_sock);
                        send_http_error(client, 502, "Bad Gateway");
                        close(client);
                        return;
                    }
                    ssize_t n;
                    while ((n = recv(up_sock, proxy_buf, sizeof(proxy_buf), 0)) > 0) {
                        if (write(client, proxy_buf, n) < 0) break;
                    }
                    free(proxy_buf);
                    close(up_sock);
                    close(client);
                    return;
                }

                // HTTP/HTTPS：重组 request line + 重写 Host/Connection + 透传
                // X-Forwarded-For，并原样保留客户端原始请求头与请求体。
                char upstream_req_full[12288];
                int total = snprintf(upstream_req_full, sizeof(upstream_req_full),
                    "%s %s %s\r\nHost: %s\r\nX-Forwarded-For: 127.0.0.1\r\nConnection: close\r\n",
                    method, path, protocol, route->upstream);
                /* buffer 已被 memchr/strchr 改写（首行以 \0 收尾），
                 * 必须用 memchr 重新定位首行换行符，取剩余部分即原始
                 * 请求头与 body，避免 strstr 命中被破坏的 \r\n。 */
                const char* line_term = memchr(buffer, '\n', (size_t)received);
                if (line_term) {
                    const char* rest = line_term + 1;
                    size_t rest_len = 0;
                    while (rest[rest_len] &&
                           rest_len + total < (size_t)sizeof(upstream_req_full))
                        rest_len++;
                    if (rest_len > 0) {
                        memcpy(upstream_req_full + total, rest, rest_len);
                        total += (int)rest_len;
                    }
                }
                ssize_t sent2 = send(up_sock, upstream_req_full, total, 0);
                if (sent2 <= 0) {
                    free(proxy_buf);
                    close(up_sock);
                    send_http_error(client, 502, "Bad Gateway");
                    close(client);
                    return;
                }

                ssize_t n;
                while ((n = recv(up_sock, proxy_buf, sizeof(proxy_buf), 0)) > 0) {
                    if (write(client, proxy_buf, n) < 0) break;
                }
                free(proxy_buf);
                close(up_sock);
                close(client);
                return;
            }
            else {
                pthread_mutex_unlock(&server->mutex);
                send_http_response(client, route->content_type, route->content, route->content_size);
                close(client);
                return;
            }
    }
    pthread_mutex_unlock(&server->mutex);
    send_http_error(client, 404, "Not Found");
    close(client);
}

static void* nl_worker_thread(void* arg) {
    nl_web_server_t* server = (nl_web_server_t*)arg;
    while (server->running) {
        int client = accept(server->sock, NULL, NULL);
        if (client < 0) {
            usleep(10000);
            continue;
        }
        nl_handle_client(server, client);
    }
    return NULL;
}

static void* web_server_thread(void* arg) {
    nl_web_server_t* server = (nl_web_server_t*)arg;

    // 启动 worker 线程池
    for (int i = 0; i < server->worker_count; i++) {
        if (pthread_create(&server->workers[i], NULL, nl_worker_thread, server) != 0) {
            server->worker_count = i;
            break;
        }
    }
    server->worker_active = 1;

    // 若 worker 全部启动成功，则 accept 线程退出，让 worker 承担所有连接
    if (server->worker_count > 0) {
        for (int i = 0; i < server->worker_count; i++) {
            if (server->workers[i]) {
                pthread_join(server->workers[i], NULL);
                server->workers[i] = 0;
            }
        }
        server->worker_active = 0;
        return NULL;
    }

    // 回退：worker 未启动（启动失败或 worker_count=0），accept 线程自己承担主 accept 循环
    while (server->running) {
        int client = accept(server->sock, NULL, NULL);
        if (client < 0) {
            usleep(10000);
            continue;
        }
        nl_handle_client(server, client);
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
    server->kqfd = -1;  // 数据驱动代理引擎初始化为未启用
    pthread_mutex_init(&server->mutex, NULL);
    nl_route_hash_init(server);
    strncpy(server->encoding, "UTF-8", sizeof(server->encoding) - 1);
    server->next = g_web_servers;
    g_web_servers = server;
    pthread_mutex_unlock(&g_web_servers_mutex);
    
    // 懒启动：create 仅注册并初始化，不绑定 socket、不起线程；
    // 由调用方显式调用 nl_web_start 启动，降低服务启动阻塞时间。
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
    free(server->route_hash);
    free(server->workers);
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

    // 检测是否存在 proxy 路由：有则走 kqueue 数据驱动引擎（单线程泵 N 条连接），
    // 否则回退到 worker 池（兼容旧行为：static/content/redirect/file 路由）
    int has_proxy = 0;
    {
        pthread_mutex_lock(&server->mutex);
        for (nl_web_route_t* r = server->routes; r; r = r->next)
            if (r->type == NL_ROUTE_TYPE_PROXY) { has_proxy = 1; break; }
        pthread_mutex_unlock(&server->mutex);
    }

    if (has_proxy) {
        // 数据驱动引擎：kqueue 多路复用，单线程，极低占用（非阻塞 fd + 1s 空闲退避）
        pthread_create(&server->thread, NULL, nl_web_proxy_engine_thread, server);
        return 0;
    }

    // 全动态化：worker 池容量按需分配
    int target = server->worker_count > 0 ? server->worker_count : WEB_WORKER_DEFAULT;
    if (target > 64) target = 64;
    if (server->worker_capacity < target) {
        pthread_t* new_workers = (pthread_t*)realloc(server->workers,
                                                     (size_t)target * sizeof(pthread_t));
        if (!new_workers) { close(server->sock); server->running = 0; return -1; }
        server->workers = new_workers;
        server->worker_capacity = target;
    }
    server->worker_count = target;
    pthread_create(&server->thread, NULL, web_server_thread, server);
    return 0;
}

// 全动态化：运行时调整 worker 池目标容量（0 = 关闭池，回退 accept 线程自处理）。
// 在 nl_web_start 前调用生效；已 start 的服务器下次重启后生效。
int nl_web_set_worker_count(nl_web_server_t* server, int target) {
    if (!server) return -1;
    if (target < 0) target = 0;
    if (target > 64) target = 64;
    server->worker_count = target;
    return 0;
}

void nl_web_stop(nl_web_server_t* server) {
    if (!server || !server->running) return;
    server->running = 0;
    if (server->thread) {
        pthread_join(server->thread, NULL);
        server->thread = 0;
    }
    // 清理 worker 线程（兜底：正常情况下 web_server_thread 内已 join）
    for (int i = 0; i < server->worker_count; i++) {
        if (server->workers && server->workers[i]) {
            pthread_join(server->workers[i], NULL);
            server->workers[i] = 0;
        }
    }
    if (server->sock >= 0) {
        close(server->sock);
        server->sock = -1;
    }
    // 兜底：数据驱动代理引擎 kqueue fd（正常已在引擎退出时关闭）
    if (server->kqfd > 0) {
        close(server->kqfd);
        server->kqfd = -1;
    }
    // 兜底：上游连接池（正常已在引擎退出时销毁）
    if (server->up_pool) {
        pool_destroy(server->up_pool);
        free(server->up_pool);
        server->up_pool = NULL;
    }
}

static void add_web_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type) {
    if (!server || !path || !content) return;
    // content_type 为空时给出默认类型，避免后续 strncpy 解引用空指针
    const char* ctype = content_type ? content_type : "application/octet-stream";
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
        strncpy(route->content_type, ctype, sizeof(route->content_type) - 1);
        route->content_type[sizeof(route->content_type) - 1] = '\0';
        route->type = NL_ROUTE_TYPE_CONTENT;  // Default: static content
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
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
    if (!template) {
        // template 为空指针时直接返回空字符串，避免 strlen(NULL)
        char* result = (char*)malloc(1);
        if (result) result[0] = '\0';
        return result;
    }
    if (!vars || !values || count <= 0) {
        char* result = (char*)malloc(strlen(template) + 1);
        if (result) strcpy(result, template);
        return result;
    }
    // 第一遍：完整扫描模板，精确计算替换后所需总长度。
    // 旧实现仅保证“当前替换点”装得下，未给替换点之后的字面量预留空间，
    // 且逐字符拷贝分支无边界检查，故改为先扫描计算总长再一次性分配。
    size_t total_len = 0;
    const char* scan = template;
    while (*scan) {
        if (strncmp(scan, "{{<var>", 7) == 0) {
            const char* var_start = scan + 7;
            const char* var_end = strstr(var_start, "</var>}}");
            if (var_end) {
                size_t var_len = var_end - var_start;
                char var_name[256];
                if (var_len >= sizeof(var_name)) var_len = sizeof(var_name) - 1;
                strncpy(var_name, var_start, var_len);
                var_name[var_len] = '\0';
                const char* replacement = "";
                for (int i = 0; i < count; i++) {
                    if (strcmp(vars[i], var_name) == 0) {
                        replacement = values[i] ? values[i] : "";
                        break;
                    }
                }
                total_len += strlen(replacement);
                scan = var_end + 8;
                continue;
            }
        }
        total_len++;
        scan++;
    }
    // 一次性分配精确容量（+1 存放结尾 '\0'），从根本上杜绝堆越界
    char* result = (char*)malloc(total_len + 1);
    if (!result) return NULL;
    // 第二遍：按同样规则拷贝，逐字符分支同样受剩余容量约束
    char* ptr = result;
    const char* src = template;
    size_t remaining = total_len;
    while (*src) {
        if (strncmp(src, "{{<var>", 7) == 0) {
            const char* var_start = src + 7;
            const char* var_end = strstr(var_start, "</var>}}");
            if (var_end) {
                size_t var_len = var_end - var_start;
                char var_name[256];
                // 变量名长度可能超过缓冲区，需截断避免栈溢出
                if (var_len >= sizeof(var_name)) var_len = sizeof(var_name) - 1;
                strncpy(var_name, var_start, var_len);
                var_name[var_len] = '\0';
                const char* replacement = "";
                for (int i = 0; i < count; i++) {
                    if (strcmp(vars[i], var_name) == 0) {
                        replacement = values[i] ? values[i] : "";
                        break;
                    }
                }
                size_t rep_len = strlen(replacement);
                if (rep_len > remaining) rep_len = remaining;
                if (rep_len > 0) {
                    memcpy(ptr, replacement, rep_len);
                    ptr += rep_len;
                    remaining -= rep_len;
                }
                src = var_end + 8;
                continue;
            }
        }
        if (remaining == 0) break;
        *ptr++ = *src++;
        remaining--;
    }
    *ptr = '\0';
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
    nl_route_hash_insert_simple(server, route);
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
    nl_route_hash_insert_simple(server, route);
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
    nl_route_hash_insert_simple(server, route);
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
        nl_route_hash_insert_simple(server, route);
    }
    pthread_mutex_unlock(&server->mutex);
    
    printf("Added redirect: %s -> %s (302)\n", path, target_url);
}

// 解析 upstream 字符串为 host + port + protocol（支持 scheme 前缀或默认端口）
// 返回 0 成功，-1 失败。host/port 由调用方提供足够缓冲。
static int nl_parse_upstream(const char* upstream, char* host, size_t host_size,
                             int* port_out, nl_proxy_protocol_t* proto_out) {
    if (!upstream || upstream[0] == '\0') return -1;

    const char* p = upstream;
    nl_proxy_protocol_t proto = NL_PROXY_HTTP;

    if (strncmp(p, "https://", 8) == 0) {
        proto = NL_PROXY_HTTPS;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        proto = NL_PROXY_HTTP;
        p += 7;
    } else if (strncmp(p, "tcp://", 6) == 0) {
        proto = NL_PROXY_TCP;
        p += 6;
    }

    // 分离 host 与 port
    char host_port[512];
    size_t len = strlen(p);
    if (len >= sizeof(host_port)) len = sizeof(host_port) - 1;
    memcpy(host_port, p, len);
    host_port[len] = '\0';

    int port = 0;
    char* colon = strrchr(host_port, ':');
    char* host_end = host_port;
    if (colon && colon != host_port) {
        *colon = '\0';
        host_end = host_port;
        port = atoi(colon + 1);
        if (port <= 0) port = 0;
    }

    if (port == 0) {
        // 未显式指定端口：按协议取默认端口
        if (proto == NL_PROXY_HTTPS) port = 443;
        else port = 80;
    }

    snprintf(host, host_size, "%s", host_end);
    *port_out = port;
    *proto_out = proto;
    return 0;
}

// 数据驱动反向代理：将 path 路径的请求转发至上游（http/https/tcp）
int nl_web_add_proxy(nl_web_server_t* server, const char* path, const char* upstream) {
    if (!server || !path || !upstream) return NL_EINVAL;

    char host[512];
    int port = 0;
    nl_proxy_protocol_t proto = NL_PROXY_HTTP;
    if (nl_parse_upstream(upstream, host, sizeof(host), &port, &proto) != 0) {
        return NL_EINVAL;
    }

    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (route) {
        strncpy(route->path, path, sizeof(route->path) - 1);
        route->path[sizeof(route->path) - 1] = '\0';
        // 上游地址统一存 host:port（不带 scheme），protocol 单独存放
        snprintf(route->upstream, sizeof(route->upstream), "%s:%d", host, port);
        route->protocol = proto;
        route->type = NL_ROUTE_TYPE_PROXY;
        route->content = NULL;
        route->content_size = 0;
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
        pthread_mutex_unlock(&server->mutex);
        printf("Added proxy route: %s -> %s (%d)\n", path, route->upstream, port);
        return NL_OK;
    }
    pthread_mutex_unlock(&server->mutex);
    return NL_ENOMEM;
}

// Runtime route management APIs (v2.2.2)

int nl_web_add_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type) {
    if (!server || !path || !content) return NL_EINVAL;
    
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = (nl_web_route_t*)calloc(1, sizeof(nl_web_route_t));
    if (route) {
        strncpy(route->path, path, sizeof(route->path) - 1);
        route->path[sizeof(route->path) - 1] = '\0';
        route->content_size = strlen(content);
        route->content = (char*)malloc(route->content_size + 1);
        if (!route->content) {
            free(route);
            pthread_mutex_unlock(&server->mutex);
            return NL_ENOMEM;
        }
        strcpy(route->content, content);
        strncpy(route->content_type, content_type, sizeof(route->content_type) - 1);
        route->content_type[sizeof(route->content_type) - 1] = '\0';
        route->type = NL_ROUTE_TYPE_CONTENT;
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
        pthread_mutex_unlock(&server->mutex);
        printf("Added route: %s\n", path);
        return NL_OK;
    }
    pthread_mutex_unlock(&server->mutex);
    return NL_ENOMEM;
}

int nl_web_remove_route(nl_web_server_t* server, const char* path) {
    if (!server || !path) return NL_EINVAL;
    
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t** pp = &server->routes;
    while (*pp) {
        if (strcmp((*pp)->path, path) == 0) {
            nl_web_route_t* target = *pp;
            *pp = target->next;
            if (target->content) free(target->content);
            free(target);
            // 路由已删除，重建 hash 表以剔除指向已释放 route 的指针
            nl_route_hash_rebuild(server);
            pthread_mutex_unlock(&server->mutex);
            printf("Removed route: %s\n", path);
            return NL_OK;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&server->mutex);
    return NL_ENOENT;
}

int nl_web_get_route_count(nl_web_server_t* server) {
    if (!server) return 0;
    
    pthread_mutex_lock(&server->mutex);
    int count = 0;
    nl_web_route_t* route = server->routes;
    while (route) {
        count++;
        route = route->next;
    }
    pthread_mutex_unlock(&server->mutex);
    return count;
}

int nl_web_list_routes(nl_web_server_t* server, char** paths, int max_paths) {
    if (!server || !paths) return NL_EINVAL;
    
    pthread_mutex_lock(&server->mutex);
    int count = 0;
    nl_web_route_t* route = server->routes;
    while (route) {
        if (count < max_paths && paths[count]) {
            strncpy(paths[count], route->path, 256);
            paths[count][255] = '\0';
        }
        count++;
        route = route->next;
    }
    pthread_mutex_unlock(&server->mutex);
    return count;
}

int nl_web_update_route(nl_web_server_t* server, const char* path, const char* content, const char* content_type) {
    if (!server || !path || !content) return NL_EINVAL;
    
    pthread_mutex_lock(&server->mutex);
    nl_web_route_t* route = server->routes;
    while (route) {
        if (strcmp(route->path, path) == 0) {
            if (route->type != NL_ROUTE_TYPE_REDIRECT && route->type != NL_ROUTE_TYPE_FILE) {
                if (route->content) free(route->content);
                route->content_size = strlen(content);
                route->content = (char*)malloc(route->content_size + 1);
                if (!route->content) {
                    pthread_mutex_unlock(&server->mutex);
                    return NL_ERROR;
                }
                strcpy(route->content, content);
                if (content_type) {
                    strncpy(route->content_type, content_type, sizeof(route->content_type) - 1);
                    route->content_type[sizeof(route->content_type) - 1] = '\0';
                }
            }
            pthread_mutex_unlock(&server->mutex);
            printf("Updated route: %s\n", path);
            return NL_OK;
        }
        route = route->next;
    }
    pthread_mutex_unlock(&server->mutex);
    return NL_ENOENT;
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
        free(server->route_hash);
        free(server->workers);
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

// JSON 字符串转义：把任意字符串（含 token/key 等敏感字段）转义为安全 JSON 字符串字面量。
// 调用方可将返回值拼接进 JSON 再输出，避免原值被直接打印造成信息泄露或注入。
// 返回值需调用方 free。NULL 输入返回指向空串的静态常量。
char* nl_json_escape(const char* input) {
    if (!input) return (char*)"";
    size_t len = strlen(input);
    size_t cap = len * 6 + 2;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    char* p = out;
    *p++ = '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        switch (c) {
            case '"':  *p++ = '\\'; *p++ = '"';  break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b';  break;
            case '\f': *p++ = '\\'; *p++ = 'f';  break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:
                if (c < 0x20) {
                    p += sprintf(p, "\\u%04x", c);
                } else {
                    *p++ = (char)c;
                }
                break;
        }
    }
    *p++ = '"';
    *p = '\0';
    return out;
}
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
        vars->server_version ? vars->server_version : "NetLeaf v2.4.0");
    
    return result;
}

char* nl_make_error_response(int status_code, const char* error_message, const char* requested_path, const char* suggestion) {
    nl_error_page_vars_t vars;
    memset(&vars, 0, sizeof(vars));
    vars.status_code = status_code;
    vars.error_message = error_message ? error_message : get_error_code_string(status_code);
    vars.requested_path = requested_path;
    vars.suggestion = suggestion;
    vars.server_version = "NetLeaf v2.4.0";
    
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
