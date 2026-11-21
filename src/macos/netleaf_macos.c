#define _DARWIN_C_SOURCE
#include <sys/event.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>   /* P10: sendfile / struct sf_hdtr（macOS 在此头声明 sf_hdtr，sendfile 另见 sys/socket.h） */
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
#include "../include/nl_util.h"
#include "../common/nl_proxy_core.h"
#include "../common/nl_uri.h"
#include "../common/nl_filesrv.h"
#include "../common/nl_px_pool.h"
#include "../common/nl_px_io.h"
#include "../common/nl_px_h2.h"
#include "../common/nl_px_local.h"

#define MAX_EVENTS 1024
#define BUFFER_SIZE 8192
#define MAX_CLIENTS 65535
#define DEFAULT_CONCURRENCY 4
/* 高压场景：Web 服务（含反向代理）worker 池默认值，受 MAX_CONCURRENCY 约束。 */
#define WEB_WORKER_DEFAULT 16
#define MAX_CONCURRENCY 64
#define MAX_WORKER_QUEUE 1024

/* ---- 第188轮：Web 引擎实例自动伸缩（auto 模式，engine_threads_cfg<=0）----
 * 与 Linux 同构：高负载自动增实例、持续空闲自动减实例，区间
 * [1, min(逻辑核数, NL_ENGINE_MAX_INSTANCES)]；显式 set_engine_threads(n) 固定不伸缩。 */
#define NL_ENGINE_MIN_INSTANCES      1
#define NL_ENGINE_MAX_INSTANCES      32
#define NL_ENGINE_SCALE_SAMPLE_MS    100
#define NL_ENGINE_SCALE_COOLDOWN_MS  3000
#define NL_ENGINE_SCALE_DOWN_TICKS   100    // 100ms ×100 = 连续空闲 10s 才缩容
#define NL_ENGINE_SCALE_UP_PER_CONN  16
#define NL_ENGINE_SCALE_DOWN_PER_CONN 2
#define NL_ENGINE_DRAIN_MS           10000

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
    /* worker 池必须经 nl_server_start_workers() 显式启用；
     * 未启用时 TCP/HTTP/WS 由 kqueue 监听线程就地处理。 */
    int worker_pool_enabled;
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

/* 日志系统已下沉到 src/common/nl_logsys.c（第193轮）：
 * 级别状态、格式化输出（毫秒时间戳+线程ID）、callback 分发、
 * debug 模式、文件落盘/轮转统一由平台无关实现提供，
 * 本文件的业务日志调用点直接使用 nl_log*()。 */

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
    // 只复制首行而非整个头部，且优先用栈缓冲：消除每请求一次 malloc + 全头部 memcpy
    size_t header_len = (size_t)(header_end - data);
    size_t line_len = 0;
    while (line_len < header_len && data[line_len] != '\n') line_len++;
    if (line_len > 0 && data[line_len - 1] == '\r') line_len--;
    char stack_line[256];
    char* line;
    if (line_len < sizeof(stack_line)) {
        line = stack_line;
    } else {
        line = (char*)malloc(line_len + 1);
        if (!line) return;
    }
    memcpy(line, data, line_len);
    line[line_len] = '\0';

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

    if (line != stack_line) free(line);
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
            nl_log(NL_LOG_ERROR, "macOS: kevent() failed: %s", strerror(errno));
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
                            nl_log(NL_LOG_ERROR, "macOS: UDP recvfrom failed: %s", strerror(errno));
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
                                nl_log(NL_LOG_ERROR, "macOS: accept() failed: %s", strerror(errno));
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
                    // Legacy single-thread path：循环 accept 排空 backlog，
                    // 突发建连时每轮只 accept 一个会把 QPS 锁死在建连开销上
                    for (;;) {
                        struct sockaddr_in client_addr;
                        socklen_t client_len = sizeof(client_addr);
                        int client_fd = accept(fd, (struct sockaddr*)&client_addr, &client_len);
                        if (client_fd == -1) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK &&
                                errno != ECONNABORTED && errno != EINTR) {
                                nl_log(NL_LOG_ERROR, "macOS: accept() failed: %s", strerror(errno));
                            }
                            break;
                        }
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
    
    nl_log(NL_LOG_INFO, "macOS: Server created on port %d (kqueue)", port);
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
            nl_log(NL_LOG_ERROR, "macOS: listen() failed: %s", strerror(errno));
            return NL_ERROR;
        }
        
        int use_pool = (server->worker_pool_enabled && server->protocol != NL_PROTO_UDP) ? 1 : 0;
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
                nl_log(NL_LOG_ERROR, "macOS: failed to allocate worker pool");
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
                    nl_log(NL_LOG_ERROR, "macOS: failed to create worker thread %d", i);
                    return NL_ERROR;
                }
            }
        }
    }
    
    struct kevent ev;
    EV_SET(&ev, server->fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    
    if (kevent(server->kqueue_fd, &ev, 1, NULL, 0, NULL) == -1) {
        nl_log(NL_LOG_ERROR, "macOS: kevent() failed: %s", strerror(errno));
        if (pool_created) stop_worker_pool(server);
        return NL_ERROR;
    }
    
    // P0 修复：running 必须无条件置位。UDP 等无 worker pool 的协议
    // pool_created 恒为 0，若包在 if 内，listener_thread 的 while(server->running)
    // 会立即退出，事件循环永远进不去（UDP 收包 0）。与 Windows 版行为对齐。
    server->running = 1;
    if (pthread_create(&server->thread_id, NULL, listener_thread, server) != 0) {
        server->running = 0;
        if (pool_created) stop_worker_pool(server);
        return NL_ERROR;
    }
    
    nl_log(NL_LOG_INFO, "macOS: Server started (kqueue), fd=%d, workers=%d",
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
    nl_log(NL_LOG_INFO, "macOS: Server stopped");
}

int nl_server_set_concurrency(nl_server_t* server, int threads) {
    if (!server) return NL_EINVAL;
    if (threads <= 0 || threads > MAX_CONCURRENCY) return NL_EINVAL;
    server->concurrency = threads;
    return NL_OK;
}

int nl_server_start_workers(nl_server_t* server) {
    if (!server) return NL_EINVAL;
    if (server->running) return NL_ERROR;
    server->worker_pool_enabled = 1;
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
    nl_log(NL_LOG_DEBUG, "macOS: Client created (socket=%d)", client->fd);
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
            nl_log(NL_LOG_ERROR, "macOS: connect() failed: %s", strerror(errno));
            return NL_ECONNECT;
        }
    }
    
    client->connected = 1;
    nl_log(NL_LOG_INFO, "macOS: Client connected to %s:%d", host, port);
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
    
    nl_log(NL_LOG_INFO, "macOS: Config loaded from %s", path);
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

/* nl_log_set_level / nl_log_set_callback 已下沉到 src/common/nl_logsys.c */

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

/* 第192轮：以下文件服务辅助已下沉 src/common/nl_filesrv.c（三平台一份实现）。
 * 保留旧名字的宏别名，全部调用点零改动。 */
#define get_file_extension nl_filesrv_ext
#define get_mime_type      nl_filesrv_mime
#define read_file          nl_filesrv_read
#define send_http_response nl_filesrv_send_response
#define send_http_error    nl_filesrv_send_error
#define send_418_response  nl_filesrv_send_teapot
#define is_april_fools_day nl_filesrv_is_teapot_day

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
        size_t rl_len = strcspn(buffer, "\r\n");
        if (nl_px_parse_request_line(buffer, rl_len, method, sizeof(method),
                                     path, sizeof(path), protocol, sizeof(protocol)) != 0) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            continue;
        }
        
        /* 第192轮：RFC 3986 端点规范化（剥 query/fragment、percent-decode、
         * dot-segment 归一）。非法端点 400，越根 403。 */
        char norm_path[4096];
        int uri_rc = nl_uri_normalize(path, norm_path, sizeof(norm_path));
        if (uri_rc != NL_URI_OK) {
            int esc = (uri_rc == NL_URI_ESCAPES);
            send_http_error(client, esc ? 403 : 400,
                            esc ? "Forbidden" : "Bad Request");
            close(client);
            continue;
        }

        if (server->enable_easter_egg && is_april_fools_day()) {
            if (strncmp(norm_path, "/tea", 4) == 0) {
                send_418_response(client);
                close(client);
                continue;
            }
        }

        /* 物理文件隔离：请求路径 → 磁盘的拼接统一经 nl_uri_resolve_file */
        char filepath[8192];
        int fr = nl_uri_resolve_file(server->directory, norm_path,
                                     filepath, sizeof(filepath));
        if (fr != NL_URI_OK) {
            int deny403 = (fr == NL_URI_ESCAPES || fr == NL_URI_EROOT);
            send_http_error(client, deny403 ? 403 : 400,
                            deny403 ? "Forbidden" : "Bad Request");
            close(client);
            continue;
        }

        struct stat path_stat;
        if (stat(filepath, &path_stat) == 0 && S_ISDIR(path_stat.st_mode)) {
            char index_path[8192];
            snprintf(index_path, sizeof(index_path), "%s/%s", filepath, server->index_file);
            /* index_file 来自配置，拼接后仍做一次根目录隔离二次校验 */
            if (stat(index_path, &path_stat) == 0 &&
                nl_uri_is_under(server->directory, index_path)) {
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
        size_t rl_len = strcspn(buffer, "\r\n");
        if (nl_px_parse_request_line(buffer, rl_len, method_str, sizeof(method_str),
                                     path, sizeof(path), protocol, sizeof(protocol)) != 0) {
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
        
        /* 第192轮：RFC 3986 端点规范化；路由匹配用 norm_path，
         * handler 仍收原始 path（既有 handler 会自行从 path 截取 query）。 */
        char norm_path[4096];
        int uri_rc = nl_uri_normalize(path, norm_path, sizeof(norm_path));
        if (uri_rc != NL_URI_OK) {
            int esc = (uri_rc == NL_URI_ESCAPES);
            send_http_error(client, esc ? 403 : 400,
                            esc ? "Forbidden" : "Bad Request");
            close(client);
            continue;
        }

        struct nl_route* route = router_find_route(rs->router, norm_path, method);
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
    unsigned char is_prefix;      // 第195轮：1=目录（前缀）路由（nginx location 语义）
    char up_uri[256];             // 第195轮：上游 URI 部分（如 "/v1/"，空=路径原样透传）
    struct nl_web_route* next;
} nl_web_route_t;

// ============ 上游 keep-alive 连接池（公共层 nl_px_pool，单线程引擎持有）============
// 分桶、探活、回收策略统一在 src/common/nl_px_pool.c；这里只提供平台动作。
// 当前毫秒（单调时钟，用于空闲回收）
static int64_t pr_pool_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
// 关闭池中 fd（平台差异集中于此）
static void pr_pool_close_fd(intptr_t fd) { if (fd >= 0) close((int)fd); }
// 探测空闲上游连接是否仍存活：poll 无事件=健康；有可读/挂起事件=对端已关或有残留数据，丢弃
static int pr_pool_probe_alive(intptr_t fd) {
    if (fd < 0) return 0;
    struct pollfd pfd;
    pfd.fd = (int)fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int r = poll(&pfd, 1, 0);
    if (r < 0) return 0;
    return (r == 0);   // 0=无事件（健康）
}
static const nl_px_pool_ops g_pr_pool_ops = {
    pr_pool_now_ms, pr_pool_probe_alive, pr_pool_close_fd
};

/* ------------------------------------------------------------------
 * P1/P6: nl_web_macos_engine_t 引擎子结构（与 Linux 版 nl_web_engine_t 对齐）
 * 把 pr_* 数组 + kqfd + 上游连接池封装为独立引擎实例，
 * server 持有 engines[N]：每个实例独占一条 ring、一个 kqueue、一个 listen fd，
 * P6 多实例时 N 个线程各跑一个引擎循环（SO_REUSEPORT 内核均衡分发）。
 * 路由表为只读共享：多引擎共享 server->routes / route_hash（启动后只增不删，
 * 引擎侧查找经 server->mutex 保护，与单实例一致）。
 * ------------------------------------------------------------------ */
typedef struct {
    int      kqfd;              // kqueue fd（-1=未启用）
    int      sock;              // 本实例 listen socket
    int      idx;               // 槽位号（伸缩/日志用）
    int      cap;               // ring 容量（按核数动态定，上限 4096）
    int      count;             // 当前活动 proxy 连接数
    int      free_top;          // 回收栈顶（O(1) 取 free slot）
    int*     free;              // 回收栈（int 数组，存空闲 idx）
    // per-slot 数组（与 ring 对齐）
    int*     pr_fd_client;      // [idx] -> 客户端 fd
    int*     pr_fd_upstream;    // [idx] -> 上游 fd（0=未连接）
    int*     pr_route_idx;      // [idx] -> 该连接对应 route 在 server->routes 链表的下标
    uint8_t* pr_state;          // [idx] -> 无状态状态位（bit0 方向 / bit1 EOF / bit7 活跃）
    char**   pr_hbuf;           // [idx] -> 每连接 8KB 首包缓冲（活跃时 malloc，归还即 free）
    char**   pr_hbuf_out;       // [idx] -> 每连接 8KB 上游请求构造缓冲
    char**   pr_up_buf;         // [idx] -> 每连接 64KB 上游响应→客户端方向缓冲
    int*     pr_hdr_len;        // [idx] -> 当前连接已收到的请求头累计字节数
    int*     pr_remain;         // [idx] -> 该方向已收待发的剩余字节数（0=全部发完）
    // 上游 keep-alive 连接池（公共层实现，单线程引擎持有，无需锁）
    nl_px_pool* up_pool;
    // 响应 framing 解析（每上游连接一份状态，旁路判定响应是否完整可复用）
    nl_px_upstate* pr_up;       // [idx] -> framing 状态（解析状态/CL/chunk/HEAD）
    char**   pr_up_key;         // [idx] -> 池 key（归还时使用，slot 归还即释放）
    // P7: 空闲超时（slowloris 防护）
    int64_t* pr_last_active_ms; // [idx] -> 最后活跃时间（ms，clock_gettime(CLOCK_MONOTONIC)）
    // HTTPS 上游 TLS 隧道：线程完成回收 + 唤醒管道（单线程 kqueue 引擎不能被隧道线程直接改 ring）
    int*     pr_tls_done;       // [idx] -> 隧道线程已结束，等待引擎回收
    int      wake_pipe[2];      // [0]=读端（注册 kqueue），[1]=隧道线程写端
    volatile int pr_tls_threads; // 存活隧道线程数（拆卸时等待归零）
    volatile int scale_state;    // 0=空闲槽 1=运行中 2=退役排空中
    volatile int retire;         // 1=请求停止 accept 并排空后退出
    volatile int exited;         // 1=引擎线程已返回（等待 join）
    volatile int overload;       // ring 满触发次数（扩容信号，评估时清零）
    // 第190轮：ring 满背压。ring 满时把 listen 从 kqueue 摘除，让连接留在内核
    // backlog 排队（而不是 accept 后直接 close 造成无响应 RST）；有槽位归还时
    // 由 pr_free_slot 重新挂回，避免电平触发 listen 事件忙等。
    int ring_full;               // 1=listen 已摘除（仅引擎线程读写）
    // 回指针
    nl_web_server_t* server;
} nl_web_macos_engine_t;

struct nl_web_server {
    int port;
    int sock;
    int af;                 // 监听地址族（AF_INET6 双栈 / AF_INET）
    int listen_mode;        // -1 = 未设置（读环境变量）；NL_PX_LISTEN_* 见 nl_px_local.h
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
    // P1: 引擎状态抽为 nl_web_macos_engine_t 子结构；P6: server 持有 engines[N]，
    // N>1 时 N 个 listen fd（SO_REUSEPORT）+ N 线程各跑一个引擎循环。
    nl_web_macos_engine_t* engines;  // 引擎槽位数组（P6：[engine_slots]，伸缩复用）
    int engine_count;                // 引擎实例数（=槽位数：单实例=1，多实例=N）
    int engine_slots;                // 伸缩上限（auto: min(逻辑核数,32)；fixed: n）
    int engine_autoscale;            // 1=auto 模式按负载在 [1, engine_slots] 间伸缩
    pthread_mutex_t engine_lock;     // 保护 engines[] 槽位状态与线程句柄增减
    pthread_t scaler_thread;         // 伸缩监督线程（auto 模式存在）
    int scaler_alive;                // 1=监督线程已启动（stop 先 join 它）
    pthread_t* engine_threads;       // P6: [engine_count] 引擎线程句柄（多实例时各线程 join）
    int engine_threads_ready;        // P6: 已就绪线程数（用于启动失败回滚）
    // P6/P7 配置字段（与 Linux 对齐，均已生效）
    int engine_threads_cfg;   // P6: 引擎实例数（0=auto 自动伸缩；>0 固定实例数）
    int max_conns_cfg;        // P6: 总连接预算（0=自动 ncpu*64，按 N 均分每引擎 ring cap）
    int idle_timeout_ms;      // P7: 空闲超时 ms（0=默认 30000）
    int force_legacy_pool;    // P5: 强制走 legacy worker 池（0=默认引擎）
    nl_redirect_type_t redirect_type; // 重定向默认类型（301/302），set/get_redirect_type 控制
};

static struct nl_web_server* g_web_servers = NULL;
static pthread_mutex_t g_web_servers_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_auto_cleanup_enabled = 0;

// 路由 hash 表辅助函数（FNV-1a + 线性探测，桶存 route* 指针，不修改 route->next）
static const int NL_ROUTE_HASH_SIZE = 128;

static unsigned nl_route_hash(const char* path) {
    /* 第192轮：FNV-1a 实现已下沉 src/common/nl_util.c（三平台一份实现） */
    return nl_hash_fnv1a(path) % NL_ROUTE_HASH_SIZE;
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
#define PR_ST_TLS     0x40   // HTTPS 上游：已移交隧道线程，引擎不再投递/关闭其 fd
#define PR_ST_ACTIVE  0x80   // 该槽活跃
#define PR_PROXY_BUF_SZ 65536
// 上游响应 framing / chunked 状态与公共层（src/common/nl_proxy_core.h）共用同一套取值；
// 槽位数组按平台生命周期存储，解析逻辑只此一份（三平台不再各写一遍）。
#define PR_UP_HDR     NL_PX_UP_HDR
#define PR_UP_CL      NL_PX_UP_CL
#define PR_UP_CHUNK   NL_PX_UP_CHUNK
#define PR_UP_DONE    NL_PX_UP_DONE
#define PR_UP_EOF     NL_PX_UP_EOF
#define PR_UP_TUNNEL  NL_PX_UP_TUNNEL

#define PR_CHUNK_SIZE    NL_PX_CHUNK_SIZE
#define PR_CHUNK_DATA    NL_PX_CHUNK_DATA
#define PR_CHUNK_CRLF    NL_PX_CHUNK_CRLF
#define PR_CHUNK_TRAILER NL_PX_CHUNK_TRAILER
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

static int pr_alloc_slot(nl_web_macos_engine_t* eng) {
    if (eng->count >= eng->cap || eng->free_top < 0) return -1;
    int idx = eng->free[eng->free_top--];
    /* 第188轮：count 会被伸缩监督线程跨线程读取，写侧改原子 */
    __atomic_add_fetch(&eng->count, 1, __ATOMIC_RELAXED);
    return idx;
}

static void pr_free_slot(nl_web_macos_engine_t* eng, int idx) {
    if (idx < 0 || idx >= eng->cap) return;
    if (eng->pr_hbuf[idx])     { free(eng->pr_hbuf[idx]);     eng->pr_hbuf[idx] = NULL; }
    if (eng->pr_hbuf_out[idx]) { free(eng->pr_hbuf_out[idx]); eng->pr_hbuf_out[idx] = NULL; }
    if (eng->pr_up_buf[idx])   { free(eng->pr_up_buf[idx]);   eng->pr_up_buf[idx] = NULL; }
    if (eng->pr_up_key && eng->pr_up_key[idx]) { free(eng->pr_up_key[idx]); eng->pr_up_key[idx] = NULL; }
    eng->pr_state[idx] = 0;
    eng->pr_fd_client[idx] = -1;
    eng->pr_fd_upstream[idx] = 0;
    eng->pr_route_idx[idx] = -1;
    if (eng->pr_hdr_len) eng->pr_hdr_len[idx] = 0;
    if (eng->pr_remain) eng->pr_remain[idx] = 0;
    if (eng->pr_up) nl_px_upstate_reset(&eng->pr_up[idx]);
    if (eng->pr_last_active_ms) eng->pr_last_active_ms[idx] = 0;  // P7: 归还即清零时间戳
    eng->free[++(eng->free_top)] = idx;
    if (__atomic_load_n(&eng->count, __ATOMIC_RELAXED) > 0)
        __atomic_sub_fetch(&eng->count, 1, __ATOMIC_RELAXED);
    // 第190轮：背压解除 —— ring 满时 listen 已被摘除，有空闲槽位就挂回 kqueue。
    // 必须在 count-- / free_top++ 之后，保证挂回后 accept 立刻拿得到槽。
    if (eng->ring_full && eng->kqfd >= 0 && eng->sock >= 0) {
        struct kevent ev;
        EV_SET(&ev, eng->sock, EVFILT_READ, EV_ADD, 0, 0,
               (void*)((uintptr_t)0x7FFFFFFF));   /* listen 哨兵（与注册处一致） */
        if (kevent(eng->kqfd, &ev, 1, NULL, 0, NULL) == 0)
            eng->ring_full = 0;
    }
}

// 忽略大小写判断 [s, s+n) 是否以 prefix 开头 / 是否包含 needle 等解析逻辑
// 已下沉至 src/common/nl_proxy_core.c（三平台共用）；此处只保留路径匹配
// 仍需的 pr_memfind 小工具。
// 在 [s, s+n) 中查找 needle（可移植，替代非标准 memmem）
static const char* pr_memfind(const char* s, size_t n, const char* needle, size_t nl) {
    if (nl == 0) return s;
    if (n < nl) return NULL;
    for (size_t i = 0; i + nl <= n; i++)
        if (s[i] == needle[0] && memcmp(s + i, needle, nl) == 0) return s + i;
    return NULL;
}

// 旁路解析被转发的上游字节（bytes 方向：上游→客户端），判定响应是否完整可复用。
// 绝不修改 bytes（转发必须字节级原样）。状态映射已统一在 nl_px_upstate。
static void pr_parse_upstream_bytes(nl_web_macos_engine_t* eng, int idx, const char* data, size_t len) {
    if (!eng || !eng->pr_up || !eng->pr_hbuf_out || !eng->pr_hbuf_out[idx]) return;
    (void)nl_px_upstate_feed(&eng->pr_up[idx], eng->pr_hbuf_out[idx], 8192, data, len);
}

static int pr_lookup_route_idx(nl_web_server_t* s, const char* path) {
    // 路径匹配（剥离 query）在公共层实现，三端语义一致
    // 第195轮：单遍扫描实现"精确 > 最长前缀"（nginx location 规则）——
    // 精确命中立即返回；前缀命中只记录最长者，扫完再回填。
    int idx = 0;
    int best_idx = -1;
    size_t best_len = 0;
    int has_best = 0;
    pthread_mutex_lock(&s->mutex);
    for (nl_web_route_t* r = s->routes; r; r = r->next, idx++) {
        if (nl_px_path_match(r->path, path)) {
            pthread_mutex_unlock(&s->mutex);
            return idx;
        }
        if (r->is_prefix && nl_px_path_match_prefix(r->path, path)) {
            size_t plen = strlen(r->path);
            if (!has_best || plen > best_len) {   // 最长前缀；同长取链表先出现者
                best_len = plen;
                best_idx = idx;
                has_best = 1;
            }
        }
    }
    pthread_mutex_unlock(&s->mutex);
    return has_best ? best_idx : -1;
}

/* ---------------- HTTPS 上游 TLS 隧道（macOS） ----------------
 * 线程独占两个 fd 完成 TLS 握手与双向收发；结束后写完成标记 + 唤醒管道，
 * 槽位由 kqueue 引擎线程统一回收。
 * ------------------------------------------------------------ */
typedef struct pr_tls_tunnel_arg_macos {
    nl_web_macos_engine_t* eng;
    int  idx;
    int  cfd, ufd;
    int  init_len;
    char* init;
    char host[512];
} pr_tls_tunnel_arg_macos_t;

static void* pr_tls_tunnel_thread_macos(void* param) {
    pr_tls_tunnel_arg_macos_t* a = (pr_tls_tunnel_arg_macos_t*)param;
    nl_web_macos_engine_t* eng = a->eng;
    int idx = a->idx, cfd = a->cfd, ufd = a->ufd;
    int init_len = a->init_len;
    char* init = a->init;
    char host[512];
    memcpy(host, a->host, sizeof(host));
    free(a);

    nl_px_tls_api api;
    void* tls_conn = NULL;
    const char* alpn = NULL;
    if (nl_px_tls_contract_get(&api) && api.conn_wrap && api.conn_read &&
        api.conn_write && api.conn_free)
        tls_conn = api.conn_wrap(ufd, host, &alpn);

    if (!tls_conn) {
        nl_log(NL_LOG_ERROR,
                  "proxy: TLS session unavailable for upstream %s (refusing plaintext fallback)", host);
        send_http_error(cfd, 502, "Bad Gateway");
        close(cfd);
        close(ufd);
    } else if (alpn && strcmp(alpn, "h2") == 0) {
        /* 上游协商出 h2：改用 H2 帧交换（内部负责关闭双方 fd 与释放 TLS 会话） */
        (void)nl_px_h2_upstream_run((intptr_t)cfd, (intptr_t)ufd, tls_conn, &api,
                                    init, init_len, 60000);
    } else {
        (void)nl_px_tls_tunnel_run((intptr_t)cfd, (intptr_t)ufd, tls_conn, &api,
                                   init, init_len, 60000);
    }
    free(init);

    eng->pr_fd_client[idx] = -1;
    eng->pr_fd_upstream[idx] = 0;
    __atomic_store_n(&eng->pr_tls_done[idx], 1, __ATOMIC_RELEASE);
    char b = 1;
    ssize_t wn = write(eng->wake_pipe[1], &b, 1);
    (void)wn;
    __atomic_sub_fetch(&eng->pr_tls_threads, 1, __ATOMIC_ACQ_REL);
    return NULL;
}

/* 把已建立的上游连接移交隧道线程；成功 1，失败已回 502 并回收槽位。 */
static int pr_tls_start_tunnel_macos(nl_web_macos_engine_t* eng, int idx, int cfd, int ufd,
                                     const char* host, const char* init, int init_len) {
    int slot_ok = (eng && idx >= 0 && idx < eng->cap &&
                   eng->pr_tls_done && eng->wake_pipe[1] >= 0) ? 1 : 0;
    if (!slot_ok || !init || init_len <= 0) {
        if (cfd >= 0) { send_http_error(cfd, 502, "Bad Gateway"); close(cfd); }
        if (ufd > 0) close(ufd);
        if (slot_ok) { eng->pr_fd_client[idx] = -1; eng->pr_fd_upstream[idx] = 0; pr_free_slot(eng, idx); }
        return 0;
    }

    pr_tls_tunnel_arg_macos_t* a = (pr_tls_tunnel_arg_macos_t*)calloc(1, sizeof(*a));
    if (a) {
        a->init = (char*)malloc((size_t)init_len);
        if (!a->init) { free(a); a = NULL; }
    }
    if (!a) {
        send_http_error(cfd, 502, "Bad Gateway");
        close(cfd);
        close(ufd);
        eng->pr_fd_client[idx] = -1;
        eng->pr_fd_upstream[idx] = 0;
        pr_free_slot(eng, idx);
        return 0;
    }
    memcpy(a->init, init, (size_t)init_len);
    a->init_len = init_len;
    a->eng = eng;
    a->idx = idx;
    a->cfd = cfd;
    a->ufd = ufd;
    snprintf(a->host, sizeof(a->host), "%s", host ? host : "");

    // 引擎不再监听客户端 fd；fd 由隧道线程最终 close（close 自动 detach kqueue）
    struct kevent dev;
    EV_SET(&dev, cfd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(eng->kqfd, &dev, 1, NULL, 0, NULL);
    eng->pr_state[idx] = PR_ST_ACTIVE | PR_ST_TLS;

    __atomic_add_fetch(&eng->pr_tls_threads, 1, __ATOMIC_ACQ_REL);
    pthread_t th;
    if (pthread_create(&th, NULL, pr_tls_tunnel_thread_macos, a) != 0) {
        __atomic_sub_fetch(&eng->pr_tls_threads, 1, __ATOMIC_ACQ_REL);
        free(a->init);
        free(a);
        send_http_error(cfd, 502, "Bad Gateway");
        close(cfd);
        close(ufd);
        pr_free_slot(eng, idx);
        return 0;
    }
    pthread_detach(th);
    return 1;
}

/* ------------------------------------------------------------
 * HTTP/2 明文（h2c）桥接：整条连接移交公共层 nl_px_h2（平台无关）。
 * 槽位复用 PR_ST_TLS 语义：kqueue 不再投递该 fd，线程退出后写完成标记
 * 并唤醒管道，槽位仍由 kqueue 引擎线程统一回收。
 * ------------------------------------------------------------ */
typedef struct pr_h2_arg_macos {
    nl_web_macos_engine_t* eng;
    int  idx;
    int  cfd;
    int  init_len;
    int  is_upgrade;
    char* init;
} pr_h2_arg_macos_t;

/* ============================================================
 * 进程内路由分发（H2 stream 直调，免去本机回放 socket 一跳）
 * ============================================================ */
static int pr_local_lookup_macos(void* ctx, const char* path, nl_px_local_route* out) {
    nl_web_server_t* s = (nl_web_server_t*)ctx;
    if (!s || !path || !out) return 0;
    int ridx = pr_lookup_route_idx(s, path);
    if (ridx < 0) return 0;
    pthread_mutex_lock(&s->mutex);
    nl_web_route_t* r = s->routes;
    for (int i = 0; r && i < ridx; i++) r = r->next;
    if (!r) { pthread_mutex_unlock(&s->mutex); return 0; }
    memset(out, 0, sizeof(*out));
    out->redirect_code = (s->redirect_type == NL_REDIRECT_PERMANENT) ? 301 : 302;
    int ok = 1;
    switch (r->type) {
    case NL_ROUTE_TYPE_CONTENT:
        out->type = NL_PX_LOCAL_CONTENT;
        out->content = r->content;
        out->content_len = r->content_size;
        out->content_type = r->content_type;
        break;
    case NL_ROUTE_TYPE_FILE:
        out->type = NL_PX_LOCAL_FILE;
        out->file_path = r->file_path;
        out->content_type = r->content_type;
        break;
    case NL_ROUTE_TYPE_REDIRECT:
        out->type = NL_PX_LOCAL_REDIRECT;
        out->redirect_url = r->redirect_url;
        break;
    case NL_ROUTE_TYPE_PROXY:
        out->type = NL_PX_LOCAL_PROXY;
        out->protocol = (r->protocol == NL_PROXY_HTTPS) ? NL_PX_LOCAL_PROTO_HTTPS
                      : (r->protocol == NL_PROXY_TCP) ? NL_PX_LOCAL_PROTO_TCP
                      : NL_PX_LOCAL_PROTO_HTTP;
        out->upstream = r->upstream;
        out->dir_prefix = r->is_prefix ? r->path : NULL;   // 第195轮：目录转发
        out->up_uri = r->up_uri;                           // 空=路径原样透传
        break;
    default:
        ok = 0;
        break;
    }
    pthread_mutex_unlock(&s->mutex);
    return ok;
}

static intptr_t pr_local_connect_macos(void* ctx, const nl_px_local_route* lr) {
    (void)ctx;
    if (!lr || !lr->upstream || lr->protocol != NL_PX_LOCAL_PROTO_HTTP) return -1;
    nl_web_route_t r;
    memset(&r, 0, sizeof(r));
    r.protocol = NL_PROXY_HTTP;
    snprintf(r.upstream, sizeof(r.upstream), "%s", lr->upstream);
    int fd = nl_proxy_connect_upstream(&r);
    return fd < 0 ? -1 : (intptr_t)fd;
}

static const nl_px_local_ops g_pr_local_ops_macos = {
    pr_local_lookup_macos, pr_local_connect_macos
};

static void* pr_h2_thread_macos(void* param) {
    pr_h2_arg_macos_t* a = (pr_h2_arg_macos_t*)param;
    nl_web_macos_engine_t* eng = a->eng;
    int idx = a->idx, cfd = a->cfd;
    int init_len = a->init_len, is_upgrade = a->is_upgrade;
    char* init = a->init;
    int port = eng->server->port;
    free(a);

    (void)nl_px_h2_bridge_run((intptr_t)cfd, port, init,
                              (size_t)init_len, is_upgrade, 60000,
                              eng->server, &g_pr_local_ops_macos);
    free(init);

    eng->pr_fd_client[idx] = -1;
    eng->pr_fd_upstream[idx] = 0;
    __atomic_store_n(&eng->pr_tls_done[idx], 1, __ATOMIC_RELEASE);
    char b = 1;
    ssize_t wn = write(eng->wake_pipe[1], &b, 1);
    (void)wn;
    __atomic_sub_fetch(&eng->pr_tls_threads, 1, __ATOMIC_ACQ_REL);
    return NULL;
}

/* 移交成功返回 1；失败已回 502 并回收槽位（返回 0） */
static int pr_h2_start_bridge_macos(nl_web_macos_engine_t* eng, int idx, int cfd,
                                    const char* init, int init_len, int is_upgrade) {
    int slot_ok = (eng && idx >= 0 && idx < eng->cap &&
                   eng->pr_tls_done && eng->wake_pipe[1] >= 0) ? 1 : 0;
    if (!slot_ok || !init || init_len <= 0) {
        if (cfd >= 0) { send_http_error(cfd, 502, "Bad Gateway"); close(cfd); }
        if (slot_ok) { eng->pr_fd_client[idx] = -1; eng->pr_fd_upstream[idx] = 0; pr_free_slot(eng, idx); }
        return 0;
    }

    pr_h2_arg_macos_t* a = (pr_h2_arg_macos_t*)calloc(1, sizeof(*a));
    if (a) {
        a->init = (char*)malloc((size_t)init_len);
        if (!a->init) { free(a); a = NULL; }
    }
    if (!a) {
        send_http_error(cfd, 502, "Bad Gateway");
        close(cfd);
        eng->pr_fd_client[idx] = -1;
        eng->pr_fd_upstream[idx] = 0;
        pr_free_slot(eng, idx);
        return 0;
    }
    memcpy(a->init, init, (size_t)init_len);
    a->init_len = init_len;
    a->eng = eng;
    a->idx = idx;
    a->cfd = cfd;
    a->is_upgrade = is_upgrade;

    // 引擎不再监听客户端 fd；fd 由桥接线程最终 close（close 自动 detach kqueue）
    struct kevent dev;
    EV_SET(&dev, cfd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    kevent(eng->kqfd, &dev, 1, NULL, 0, NULL);
    eng->pr_state[idx] = PR_ST_ACTIVE | PR_ST_TLS;

    __atomic_add_fetch(&eng->pr_tls_threads, 1, __ATOMIC_ACQ_REL);
    pthread_t th;
    if (pthread_create(&th, NULL, pr_h2_thread_macos, a) != 0) {
        __atomic_sub_fetch(&eng->pr_tls_threads, 1, __ATOMIC_ACQ_REL);
        free(a->init);
        free(a);
        send_http_error(cfd, 502, "Bad Gateway");
        close(cfd);
        pr_free_slot(eng, idx);
        return 0;
    }
    pthread_detach(th);
    return 1;
}

// 把 proxy 连接注册进 kqueue：客户端 + 上游双 fd，EVFILT_READ
// kqueue 在 close(fd) 时自动 detach 对应 event，无需 EVFILT_READ + EV_DELETE。
static void pr_register(nl_web_macos_engine_t* eng, int idx, int cfd, int ufd) {
    struct kevent ev;
    EV_SET(&ev, cfd, EVFILT_READ, EV_ADD, 0, 0, (void*)PR_UDATA(idx, 0));
    kevent(eng->kqfd, &ev, 1, NULL, 0, NULL);
    if (ufd > 0) {
        EV_SET(&ev, ufd, EVFILT_READ, EV_ADD, 0, 0, (void*)PR_UDATA(idx, 1));
        kevent(eng->kqfd, &ev, 1, NULL, 0, NULL);
    }
}

// 非阻塞泵：先补发上次未发完的余量（pr_remain），再读新数据转发。
// 返回：1=可继续（EAGAIN/读满），0=对端 EOF，-1=错误。
// 泵送状态机（含半包余量前移、旁路喂解析器）统一在 src/common/nl_px_io.c。
static int pr_io_recv(intptr_t fd, char* buf, int cap) {
    ssize_t n = recv((int)fd, buf, (size_t)cap, 0);
    if (n > 0) return (int)n;
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return NL_PX_IO_WANT;
    return NL_PX_IO_ERR;
}
static int pr_io_send(intptr_t fd, const char* buf, int len) {
    ssize_t w = send((int)fd, buf, (size_t)len, 0);
    if (w > 0) return (int)w;
    if (w == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return NL_PX_IO_WANT;
    return NL_PX_IO_ERR;
}
static const nl_px_io_ops g_pr_io_ops = { pr_io_recv, pr_io_send };

typedef struct {
    nl_web_macos_engine_t* eng;
    int idx;
    int parse_up;
} pr_pump_cb_t;

static void pr_pump_on_bytes(void* ud, const char* data, size_t len) {
    pr_pump_cb_t* c = (pr_pump_cb_t*)ud;
    if (c->eng->pr_last_active_ms) c->eng->pr_last_active_ms[c->idx] = pr_pool_now_ms();
    if (c->parse_up) pr_parse_upstream_bytes(c->eng, c->idx, data, len);
}

static int pr_pump_once(nl_web_macos_engine_t* eng, int idx, int from, int to, char* buf, size_t cap, int parse_up) {
    pr_pump_cb_t cb;
    cb.eng = eng;
    cb.idx = idx;
    cb.parse_up = parse_up;
    return nl_px_pump(&g_pr_io_ops, from, to, buf, cap, &eng->pr_remain[idx],
                      pr_pump_on_bytes, &cb);
}

// 构造上游请求，输出到 out，返回字节数（-1=失败）
static int pr_build_upstream_http(nl_web_macos_engine_t* eng, int idx,
                                  const char* method, const char* path,
                                  const char* proto,
                                  const char* raw_hdrs, size_t raw_hdrs_len,
                                  char* out, size_t out_cap) {
    nl_web_server_t* s = eng->server;   // 路由表共享：经 server 指针 + mutex 访问
    // 持锁解析路由并复制上游地址：解锁后不再解引用共享路由对象
    nl_web_route_t* r;
    pthread_mutex_lock(&s->mutex);
    int rd = eng->pr_route_idx[idx];
    r = NULL;
    int cur = 0;
    for (nl_web_route_t* it = s->routes; it; it = it->next, cur++) {
        if (cur == rd) { r = it; break; }
    }
    if (rd < 0 || !r) { pthread_mutex_unlock(&s->mutex); return -1; }
    int is_tcp = (r->protocol == NL_PROXY_TCP);
    char upstream[512];
    snprintf(upstream, sizeof(upstream), "%s", r->upstream);
    /* 第195轮：目录转发改写所需字段一并复制（解锁后不再解引用共享路由） */
    int is_prefix = r->is_prefix;
    char match_path[256], up_uri[256];
    snprintf(match_path, sizeof(match_path), "%s", r->path);
    snprintf(up_uri, sizeof(up_uri), "%s", r->up_uri);
    pthread_mutex_unlock(&s->mutex);

    if (is_tcp) return nl_px_passthrough(out, out_cap, raw_hdrs, raw_hdrs_len);

    // 取真实客户端 IPv4 作为 X-Forwarded-For；取不到就不加该头
    char peer_ip[48];
    peer_ip[0] = '\0';
    if (eng->pr_fd_client[idx] >= 0) {
        struct sockaddr_storage pa;
        socklen_t palen = sizeof(pa);
        memset(&pa, 0, sizeof(pa));
        if (getpeername(eng->pr_fd_client[idx], (struct sockaddr*)&pa, &palen) == 0)
            (void)nl_px_sockaddr_ip((const struct sockaddr*)&pa, peer_ip, sizeof(peer_ip));
    }
    /* 第195轮：目录转发路径改写（nginx proxy_pass URI 拼接语义）；
     * up_uri 为空时 nl_px_dir_rewrite 原样拷贝，热路径开销仅为一次 strlen。 */
    char rpath[4096];
    const char* fwd_path = path;
    if (up_uri[0]) {
        if (nl_px_dir_rewrite(match_path, is_prefix, up_uri, path,
                              rpath, sizeof(rpath)) > 0)
            fwd_path = rpath;
    }
    // 请求行改写 / Host / XFF / 客户端头透传 / 空行终止符：公共层实现
    return nl_px_build_request(out, out_cap, method, fwd_path, proto, upstream,
                               raw_hdrs, raw_hdrs_len, peer_ip[0] ? peer_ip : NULL);
}

// 第188轮：排空 listen backlog（事件分支与退役排空共用）。ring 满计 overload。
static void pr_engine_accept_all(nl_web_macos_engine_t* eng, int listen_fd);

// kqueue 数据驱动主循环：每个引擎实例单线程 kevent 驱动 N 条 proxy 连接
// 通过 kevent(256) + 1000ms 超时实现极低占用（空闲 1s 退避）
// 上游侧用 data->ident 标记（高位 0x40000000），listen fd 用哨兵 0xFFFFFFFF
// P6: 多实例时每个引擎持有独立 listen fd（SO_REUSEPORT）与独立 ring/kqueue
void nl_web_proxy_engine(nl_web_macos_engine_t* eng, int listen_fd, int ring_cap_override) {
    nl_web_server_t* s = eng->server;   // 回指 server：访问共享路由表 / running / 配置

    eng->kqfd = kqueue();
    if (eng->kqfd < 0) return;

    // 初始化 ring 容量（P6: 多实例时按 ring_cap_override 指定，否则 ncpu*64 自动）
    int cap = (ring_cap_override > 0) ? ring_cap_override : nl_web_proxy_ring_capacity();
    eng->cap = cap;
    eng->pr_fd_client   = (int*)calloc((size_t)cap, sizeof(int));
    eng->pr_fd_upstream = (int*)calloc((size_t)cap, sizeof(int));
    eng->pr_route_idx   = (int*)calloc((size_t)cap, sizeof(int));
    eng->pr_state       = (uint8_t*)calloc((size_t)cap, 1);
    eng->pr_hbuf        = (char**)calloc((size_t)cap, sizeof(char*));
    eng->pr_hbuf_out    = (char**)calloc((size_t)cap, sizeof(char*));
    eng->pr_up_buf      = (char**)calloc((size_t)cap, sizeof(char*));
    eng->pr_hdr_len     = (int*)calloc((size_t)cap, sizeof(int));
    eng->pr_remain      = (int*)calloc((size_t)cap, sizeof(int));
    eng->free        = (int*)malloc((size_t)cap * sizeof(int));
    // 上游 framing 解析状态（单数组）+ 连接池 key
    eng->pr_up          = (nl_px_upstate*)calloc((size_t)cap, sizeof(nl_px_upstate));
    eng->pr_up_key      = (char**)calloc((size_t)cap, sizeof(char*));
    // P7: 空闲超时（slowloris 防护）
    eng->pr_last_active_ms = (int64_t*)calloc((size_t)cap, sizeof(int64_t));
    for (int i = 0; i < cap; i++) eng->free[i] = cap - 1 - i;
    eng->free_top = cap - 1;
    eng->count = 0;
    if (!eng->pr_fd_client || !eng->pr_fd_upstream || !eng->pr_route_idx ||
        !eng->pr_state || !eng->pr_hbuf || !eng->pr_hbuf_out || !eng->pr_up_buf ||
        !eng->pr_hdr_len || !eng->pr_remain || !eng->free ||
        !eng->pr_up || !eng->pr_up_key || !eng->pr_last_active_ms) {
        close(eng->kqfd); eng->kqfd = -1;
        return;
    }
    // HTTPS 上游隧道：完成标记数组 + 唤醒管道（隧道线程写，引擎 kqueue 读）
    eng->pr_tls_done = (int*)calloc((size_t)cap, sizeof(int));
    eng->wake_pipe[0] = eng->wake_pipe[1] = -1;
    if (pipe(eng->wake_pipe) == 0) {
        for (int p = 0; p < 2; p++) {
            int pf = fcntl(eng->wake_pipe[p], F_GETFL, 0);
            fcntl(eng->wake_pipe[p], F_SETFL, pf | O_NONBLOCK);
        }
    }

    // 初始化上游 keep-alive 连接池（公共层实现，单线程持有，无需锁）
    eng->up_pool = (nl_px_pool*)malloc(sizeof(nl_px_pool));
    if (eng->up_pool) nl_px_pool_init(eng->up_pool, &g_pr_pool_ops, 256, 8, 30000);

    // 注册 listen fd：udata 哨兵 = PR_UDATA(0,0)=0 会误判，改用高位编码 client 方向
    // kqueue EVFILT_READ 在 close 时自动 detach，无需手动 EV_DELETE
    struct kevent lev;
    EV_SET(&lev, listen_fd, EVFILT_READ, EV_ADD, 0, 0,
           (void*)((uintptr_t)0x7FFFFFFF));  /* listen 哨兵：client 方向 idx=0x7FFFFFF */
    kevent(eng->kqfd, &lev, 1, NULL, 0, NULL);
    if (eng->wake_pipe[0] >= 0) {
        struct kevent wev;
        EV_SET(&wev, eng->wake_pipe[0], EVFILT_READ, EV_ADD, 0, 0,
               (void*)((uintptr_t)0x7FFFFFE));  /* 隧道完成唤醒哨兵 */
        kevent(eng->kqfd, &wev, 1, NULL, 0, NULL);
    }

    // 设 listen fd 非阻塞
    int flags = fcntl(listen_fd, F_GETFL, 0);
    fcntl(listen_fd, F_SETFL, flags | O_NONBLOCK);

    struct kevent events[256];

    // P7: 空闲超时（slowloris 防护）。0=默认 30s；<=0 视为默认
    int idle_ms = s->idle_timeout_ms;
    if (idle_ms <= 0) idle_ms = 30000;
    int64_t last_scan_ms = pr_pool_now_ms();

    // 第188轮：缩容退役状态。lfd_open=本实例 listen fd 是否仍注册在 kqueue
    int lfd_open = 1;
    int64_t retire_since = 0;

    while (s->running) {
        // 第188轮：缩容退役 —— 先排空 backlog（否则关 listen fd 会 RST 未 accept 连接），
        // 再摘除并关闭本实例独占 listen fd，等活跃连接归零（或排空超时兜底）后退出线程。
        if (__atomic_load_n(&eng->retire, __ATOMIC_ACQUIRE)) {
            if (retire_since == 0) retire_since = pr_pool_now_ms();
            if (lfd_open) {
                pr_engine_accept_all(eng, listen_fd);
                // kqueue 在 close 时自动 detach，无需 EV_DELETE
                if (listen_fd != s->sock) close(listen_fd);   // engine 0 的 listen fd 归 stop 关
                lfd_open = 0;
                eng->sock = -1;
            }
            int left = __atomic_load_n(&eng->count, __ATOMIC_RELAXED);
            if (left == 0) break;
            if (pr_pool_now_ms() - retire_since >= NL_ENGINE_DRAIN_MS) break;
        }
        struct timespec timeout;
        timeout.tv_sec = 1;
        timeout.tv_nsec = 0;
        int n = kevent(eng->kqfd, NULL, 0, events, 256, &timeout);
        if (n < 0) { if (errno == EINTR) continue; break; }
        nl_px_pool_reap(eng->up_pool, pr_pool_now_ms());   // 每轮回收空闲超时上游连接
        // P7: 空闲连接扫描回收（slowloris 防护）：节流扫描全部 slot，回收超时连接。
        // kqueue 在 close(fd) 时自动 detach 对应 event，无需 EV_DELETE（见 pr_register 注释）。
        {
            int64_t now_ms = pr_pool_now_ms();
            if (now_ms - last_scan_ms >= 500) {   // 每 ~500ms 扫一次（kevent 超时返回 0 时也会走到）
                last_scan_ms = now_ms;
                for (int si = 0; si < eng->cap; si++) {
                    if (!(eng->pr_state[si] & PR_ST_ACTIVE)) continue;
                    if (eng->pr_state[si] & PR_ST_TLS) continue;   // 隧道自管超时
                    int64_t last = eng->pr_last_active_ms[si];
                    if (last <= 0 || (now_ms - last) >= idle_ms) {
                        int cfd = eng->pr_fd_client[si];
                        int ufd = eng->pr_fd_upstream[si];
                        if (ufd > 0) {
                            int poolable = (eng->pr_up[si].up_parse == PR_UP_DONE &&
                                            eng->pr_up_key && eng->pr_up_key[si] && eng->pr_up_key[si][0] && eng->up_pool);
                            if (poolable)
                                nl_px_pool_put(eng->up_pool, eng->pr_up_key[si], ufd);
                            else
                                close(ufd);   // close 自动 detach kqueue
                        }
                        if (cfd > 0) close(cfd);
                        pr_free_slot(eng, si);
                    }
                }
            }
        }
        for (int i = 0; i < n; i++) {
            uint64_t udata = (uint64_t)(uintptr_t)events[i].udata;
            if (udata == 0x7FFFFFEULL) {
                // TLS 隧道线程完成唤醒：排空管道并回收完成槽位
                char tmp[64];
                while (read(eng->wake_pipe[0], tmp, sizeof(tmp)) > 0) { }
                for (int ti = 0; ti < eng->cap; ti++) {
                    if (!__atomic_load_n(&eng->pr_tls_done[ti], __ATOMIC_ACQUIRE)) continue;
                    __atomic_store_n(&eng->pr_tls_done[ti], 0, __ATOMIC_RELEASE);
                    // fd 已由隧道线程关闭并置 -1/0；这里仅兜底关闭与回收
                    if (eng->pr_fd_client[ti] >= 0) close(eng->pr_fd_client[ti]);
                    if (eng->pr_fd_upstream[ti] > 0) close(eng->pr_fd_upstream[ti]);
                    pr_free_slot(eng, ti);
                }
                continue;
            }
            int is_up      = (udata & PR_UDATA_UP_BIT) ? 1 : 0;
            int idx        = (int)(udata & ~PR_UDATA_UP_BIT);
            if (idx == 0x7FFFFFF) {
                // accept 客户端连接：循环排空 backlog（突发建连下每轮一个会锁死 QPS）
                if (lfd_open) pr_engine_accept_all(eng, listen_fd);
                continue;
            }
            if (idx >= eng->cap || !(eng->pr_state[idx] & PR_ST_ACTIVE)) continue;
            int cfd = eng->pr_fd_client[idx];
            int ufd = eng->pr_fd_upstream[idx];
            if (cfd <= 0) continue;

            if (!is_up) {
                // 客户端可写：补发上游→客户端方向未发完的余量（客户端写慢导致 send EAGAIN）
                if (events[i].filter == EVFILT_WRITE) {
                    int* remain = eng->pr_remain;
                    int werr = 0;
                    while (remain && remain[idx] > 0) {
                        ssize_t w = send(cfd, eng->pr_up_buf[idx], (size_t)remain[idx], 0);
                        if (w < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;  // 仍写满，保持注册
                            werr = 1; break;
                        }
                        if (w == 0) break;
                        // 若只发了 w 字节，剩余前移消除空洞（下次 send 从 buf[0] 发新数据）
                        if (w > 0 && w < remain[idx])
                            memmove(eng->pr_up_buf[idx], eng->pr_up_buf[idx] + w, (size_t)(remain[idx] - w));
                        remain[idx] -= (int)w;
                    }
                    if (werr) {
                        // 写错误：连接不可用，关闭两端（close 自动 detach）
                        if (eng->pr_fd_upstream[idx] > 0) close(eng->pr_fd_upstream[idx]);
                        close(cfd);
                        pr_free_slot(eng, idx);
                        continue;
                    }
                    if (remain[idx] == 0) {
                        // 余量排空：摘除客户端写事件
                        struct kevent dwv;
                        EV_SET(&dwv, cfd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
                        kevent(eng->kqfd, &dwv, 1, NULL, 0, NULL);
                        // 排空后立即做池化完成判定（与既有完成分支一致）
                        if (eng->pr_up[idx].up_parse == PR_UP_DONE) {
                            int ufd2 = eng->pr_fd_upstream[idx];
                            struct kevent dev;
                            EV_SET(&dev, ufd2, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                            kevent(eng->kqfd, &dev, 1, NULL, 0, NULL);
                            if (eng->up_pool && eng->pr_up_key[idx] && eng->pr_up_key[idx][0])
                                nl_px_pool_put(eng->up_pool, eng->pr_up_key[idx], ufd2);
                            else
                                close(ufd2);
                            close(cfd);        // close 自动 detach kqueue
                            pr_free_slot(eng, idx);
                            continue;
                        }
                    }
                    // kqueue 单事件单 filter：可读事件会作为独立 kevent 返回，无需再处理读
                    continue;
                }
                // 已建立上游连接（后续 body / 101 升级隧道）：必须先直达转发。
                // 不能先 recv 到 hbuf 再 pump —— pump 会再次 recv，非阻塞返回 EAGAIN 时
                // 刚读到的字节会被直接丢弃（WS 隧道回显失败的根因）。
                if (eng->pr_fd_upstream[idx] > 0) {
                    int rc = pr_pump_once(eng, idx, cfd, eng->pr_fd_upstream[idx],
                                          eng->pr_hbuf[idx], 8192, 0);
                    if (rc < 0) {
                        if (eng->pr_fd_upstream[idx] > 0) close(eng->pr_fd_upstream[idx]);
                        close(cfd);
                        pr_free_slot(eng, idx);
                    }
                    continue;
                }
                // 客户端侧：收请求
                char*  hbuf = eng->pr_hbuf[idx];
                int*   hlen = &eng->pr_hdr_len[idx];
                int maxsz = 8192;
                ssize_t nread = recv(cfd, hbuf + *hlen, (size_t)(maxsz - *hlen), 0);
                if (nread <= 0) {
                    if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    // 客户端 EOF：清理双 fd（kqueue close 自动 detach）
                    if (eng->pr_fd_upstream[idx] > 0) close(eng->pr_fd_upstream[idx]);
                    close(cfd);
                    pr_free_slot(eng, idx);
                    continue;
                }
                *hlen += (int)nread;
                hbuf[*hlen] = '\0';
                eng->pr_last_active_ms[idx] = pr_pool_now_ms();  // P7: 收到客户端数据，刷新空闲时间戳

                /* HTTP/2 明文（h2c prior-knowledge）：首包即连接预流，必须在
                 * 请求行解析之前判定，否则 "PRI * HTTP/2.0" 会被当成 HTTP/1。 */
                if (eng->pr_fd_upstream[idx] == 0) {
                    int h2 = nl_px_h2_preface_check(hbuf, (size_t)*hlen);
                    if (h2 == 2) {
                        (void)pr_h2_start_bridge_macos(eng, idx, cfd, hbuf, *hlen, 0);
                        continue;
                    }
                    if (h2 == 1) continue;   /* 预流未收满 24 字节，继续收 */
                }


                if (eng->pr_fd_upstream[idx] == 0) {
                    char* line_end = memchr(hbuf, '\n', (size_t)*hlen);
                    if (!line_end) continue;
                    char method[16] = {0}, path[4096] = {0}, proto[32] = {0};
                    if (nl_px_parse_request_line(hbuf, (size_t)(line_end - hbuf),
                                                 method, sizeof(method),
                                                 path, sizeof(path),
                                                 proto, sizeof(proto)) != 0) {
                        send_http_error(cfd, 400, "Bad Request");
                        close(cfd);
                        pr_free_slot(eng, idx);
                        continue;
                    }
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
                        pr_free_slot(eng, idx);
                        continue;
                    }
                    // 请求头完整性检查：非 TCP 代理必须收到完整的 \r\n\r\n 才能组装上游请求，
                    // 否则上游会一直等请求头（典型表现：请求挂死）
                    int is_tcp_proxy = (r->type == NL_ROUTE_TYPE_PROXY && r->protocol == NL_PROXY_TCP);
                    char* hdr_end = NULL;
                    if (!is_tcp_proxy) {
                        size_t rest_n = (size_t)(*hlen - (int)(line_end + 1 - hbuf));
                        hdr_end = (char*)pr_memfind(line_end + 1, rest_n, "\r\n\r\n", 4);
                        if (!hdr_end) {
                            if (*hlen >= 8192) {
                                // 首包头过大：拒绝，避免无限等待
                                send_http_error(cfd, 400, "Bad Request");
                                close(cfd);
                                pr_free_slot(eng, idx);
                            }
                            continue;   // 头未收全：保持 *hlen 累积，等下一次可读事件
                        }
                    }
                    /* HTTP/1.1 Upgrade: h2c —— 请求头已收齐时移交公共层 H2 桥接，
                     * 该请求本身作为 stream 1；此后平台不再处理这条连接。 */
                    if (!is_tcp_proxy && hdr_end &&
                        nl_px_h2_upgrade_check(line_end + 1,
                            (size_t)(hdr_end - (line_end + 1))) == 1) {
                        (void)pr_h2_start_bridge_macos(eng, idx, cfd, hbuf, *hlen, 1);
                        continue;
                    }

                    // 上游请求的原始头与 body 区间（TCP 透传整段首包）
                    char*  raw      = is_tcp_proxy ? hbuf : (line_end + 1);
                    size_t raw_len  = is_tcp_proxy ? (size_t)*hlen : (size_t)(hdr_end - raw);
                    size_t body_off = is_tcp_proxy ? (size_t)*hlen
                                                   : (size_t)(hdr_end - hbuf) + 4;
                    size_t req_body_len = ((size_t)*hlen > body_off) ? ((size_t)*hlen - body_off) : 0;
                    // 完整路由分发（解决混合路由限制：引擎同时处理 PROXY + CONTENT + FILE + REDIRECT）
                    // 非 proxy 路由：临时切阻塞（复用 worker 池已验证的 send_http_response / redirect 模板），
                    // 处理完直接 close + free slot；proxy 路由保持非阻塞走引擎 pump 链路。
                    if (r->type != NL_ROUTE_TYPE_PROXY) {
                        // 临时切阻塞：去掉 O_NONBLOCK
                        int fl_tmp = fcntl(cfd, F_GETFL, 0);
                        fcntl(cfd, F_SETFL, fl_tmp & ~O_NONBLOCK);

                        // 查路由 + 状态 + 响应头统一由公共层生成（与 H2 进程内分发同源）；
                        // body 仍按平台最优方式发送：CONTENT 直接 send，FILE 走 sendfile 零拷贝。
                        nl_px_local_plan plan;
                        int built = nl_px_local_plan_build(s, &g_pr_local_ops_macos, path,
                                                           strcmp(method, "HEAD") == 0, &plan);
                        if (built == 1) {
                            if (plan.head_len > 0)
                                (void)send(cfd, plan.head, plan.head_len, 0);
                            if (plan.type == NL_PX_LOCAL_CONTENT && plan.body_len > 0) {
                                (void)send(cfd, plan.body, plan.body_len, 0);
                            } else if (plan.type == NL_PX_LOCAL_FILE && plan.file_size > 0 &&
                                       strcmp(method, "HEAD") != 0) {
                                int ffd = open(plan.file_path, O_RDONLY);
                                if (ffd >= 0) {
                                    off_t off = 0;
                                    while (off < plan.file_size) {
                                        off_t len = (off_t)(plan.file_size - off);
                                        if (len > 1024*1024) len = 1024*1024;  // 每次最多 1MB
                                        int sret = sendfile(ffd, cfd, off, &len, NULL, 0);
                                        off += len;
                                        if (sret == 0) continue;
                                        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                                        break;
                                    }
                                    close(ffd);
                                }
                            }
                        } else {
                            send_http_error(cfd, 404, "Not Found");
                        }
                        close(cfd);   // close 自动 detach kqueue
                        pr_free_slot(eng, idx);
                        continue;
                    }
                    // proxy 路由：连接上游（优先复用池中空闲连接，未命中再新建）
                    // 第190轮：proxy 专用缓冲延迟到这里才分配（本地路由 64KB+8KB+576B 全省掉）
                    if (!eng->pr_up_buf[idx])
                        eng->pr_up_buf[idx] = (char*)malloc(PR_PROXY_BUF_SZ);
                    if (!eng->pr_hbuf_out[idx])
                        eng->pr_hbuf_out[idx] = (char*)malloc(8192);
                    if (!eng->pr_up_key[idx])
                        eng->pr_up_key[idx] = (char*)malloc(576);
                    if (!eng->pr_up_buf[idx] || !eng->pr_hbuf_out[idx] || !eng->pr_up_key[idx]) {
                        send_http_error(cfd, 500, "Internal Server Error");
                        close(cfd);
                        pr_free_slot(eng, idx);
                        continue;
                    }
                    eng->pr_up[idx].req_is_head = (strcmp(method, "HEAD") == 0) ? 1 : 0;
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
                    // HTTPS 上游带 TLS 会话状态，不能复用裸 socket 池
                    if (r->protocol != NL_PROXY_TCP && r->protocol != NL_PROXY_HTTPS && eng->up_pool)
                        nfdu = (int)nl_px_pool_get(eng->up_pool, up_host, up_port, (int)r->protocol);
                    if (nfdu < 0) nfdu = nl_proxy_connect_upstream(r);
                    if (nfdu < 0) {
                        send_http_error(cfd, 502, "Bad Gateway");
                        close(cfd);
                        pr_free_slot(eng, idx);
                        continue;
                    }
                    eng->pr_fd_upstream[idx] = nfdu;
                    // 记录池 key（tcp/https 不参与池，置空），响应完成归还池时使用
                    if (eng->pr_up_key[idx]) {
                        if (r->protocol != NL_PROXY_TCP && r->protocol != NL_PROXY_HTTPS)
                            nl_px_pool_make_key(eng->pr_up_key[idx], 576, up_host, up_port, (int)r->protocol);
                        else
                            eng->pr_up_key[idx][0] = '\0';
                    }
                    int uf_flags = fcntl(nfdu, F_GETFL, 0);
                    fcntl(nfdu, F_SETFL, uf_flags | O_NONBLOCK);
                    eng->pr_route_idx[idx] = ridx;

                    char* ubuf = eng->pr_hbuf_out[idx];
                    int build_n = pr_build_upstream_http(eng, idx, method, path, proto,
                                                         raw, raw_len, ubuf, 8192);
                    if (build_n > 0 && req_body_len > 0) {
                        // 首包里的 body（如小 POST）必须紧随请求头一起发给上游，不能丢
                        size_t need = (size_t)build_n + req_body_len;
                        if (need > 8192) {
                            char* nb = (char*)realloc(ubuf, need);
                            if (!nb) build_n = -1;
                            else { ubuf = nb; eng->pr_hbuf_out[idx] = nb; }
                        }
                        if (build_n > 0) {
                            memcpy(ubuf + build_n, hbuf + body_off, req_body_len);
                            build_n += (int)req_body_len;
                        }
                    }
                    // HTTPS 上游：整条连接移交 TLS 隧道线程（握手与双向收发都在线程内）
                    if (r->protocol == NL_PROXY_HTTPS) {
                        (void)pr_tls_start_tunnel_macos(eng, idx, cfd, nfdu, up_host,
                                                        ubuf, build_n);
                        *hlen = 0;
                        continue;
                    }
                    if (build_n > 0) {
                        // 临时切阻塞，保证初始请求（头 + 首包 body）完整发出，避免 EAGAIN 截断
                        int blk_fl = fcntl(nfdu, F_GETFL, 0);
                        fcntl(nfdu, F_SETFL, blk_fl & ~O_NONBLOCK);
                        ssize_t off = 0;
                        while (off < build_n) {
                            ssize_t w = send(nfdu, ubuf + off, (size_t)(build_n - off), 0);
                            if (w <= 0) break;
                            off += w;
                        }
                        fcntl(nfdu, F_SETFL, blk_fl | O_NONBLOCK);
                    }
                    *hlen = 0;   // 首包已消费（请求头与 body 均已发往上游）
                    struct kevent uev;
                    EV_SET(&uev, nfdu, EVFILT_READ, EV_ADD, 0, 0, (void*)PR_UDATA(idx, 1));
                    kevent(eng->kqfd, &uev, 1, NULL, 0, NULL);
                    eng->pr_state[idx] = PR_ST_DIR_W | PR_ST_ACTIVE;
                    continue;
                }
            }

            // 上游侧：读响应 → 写客户端（该方向旁路喂 framing 解析器）
            int rc = pr_pump_once(eng, idx, ufd, cfd, eng->pr_up_buf[idx], PR_PROXY_BUF_SZ, 1);
            if (rc == 0 || rc < 0) {
                // 上游 EOF / 错误：连接已不可复用，关闭两端
                eng->pr_state[idx] |= PR_ST_UP_EOF;
                if (eng->pr_fd_upstream[idx] > 0) close(eng->pr_fd_upstream[idx]);
                close(cfd);
                pr_free_slot(eng, idx);
                continue;
            }
            // 客户端写慢（send EAGAIN）导致仍有未发完的响应余量：
            // 给客户端 fd 注册 EVFILT_WRITE，等可写事件唤醒补发
            if (rc == 1 && eng->pr_remain[idx] > 0) {
                struct kevent wev;
                EV_SET(&wev, cfd, EVFILT_WRITE, EV_ADD, 0, 0, (void*)PR_UDATA(idx, 0));
                kevent(eng->kqfd, &wev, 1, NULL, 0, NULL);
            }
            // 响应 framing 判定完成且已全部转发客户端 → 上游归还池，关闭客户端
            if (eng->pr_up[idx].up_parse == PR_UP_DONE && eng->pr_remain[idx] == 0) {
                int ufd2 = eng->pr_fd_upstream[idx];
                // kqueue EV_DELETE 摘除上游读事件（不 close），供池复用
                struct kevent dev;
                EV_SET(&dev, ufd2, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                kevent(eng->kqfd, &dev, 1, NULL, 0, NULL);
                if (eng->up_pool && eng->pr_up_key[idx] && eng->pr_up_key[idx][0])
                    nl_px_pool_put(eng->up_pool, eng->pr_up_key[idx], ufd2);
                else
                    close(ufd2);   // 无池/tcp：退回关闭
                close(cfd);        // close 自动 detach kqueue
                pr_free_slot(eng, idx);
            }
        }
    }

    // 第188轮：退出兜底 —— 引擎异常退出时若本实例 listen fd 仍开着，先关闭（防 fd 泄漏）
    if (lfd_open && listen_fd >= 0 && listen_fd != s->sock) {
        close(listen_fd);
        lfd_open = 0;
    }
    eng->sock = -1;

    // 先唤醒并等待 TLS 隧道线程退出，之后才能释放 ring 数组
    if (__atomic_load_n(&eng->pr_tls_threads, __ATOMIC_ACQUIRE) > 0) {
        for (int i = 0; i < eng->cap; i++) {
            if (!(eng->pr_state[i] & PR_ST_ACTIVE) || !(eng->pr_state[i] & PR_ST_TLS)) continue;
            if (eng->pr_fd_client[i] >= 0) shutdown(eng->pr_fd_client[i], SHUT_RDWR);
            if (eng->pr_fd_upstream[i] > 0) shutdown(eng->pr_fd_upstream[i], SHUT_RDWR);
        }
        for (int w = 0; w < 1000 &&
             __atomic_load_n(&eng->pr_tls_threads, __ATOMIC_ACQUIRE) > 0; w++)
            usleep(10000);
    }
    if (eng->wake_pipe[0] >= 0) close(eng->wake_pipe[0]);
    if (eng->wake_pipe[1] >= 0) close(eng->wake_pipe[1]);
    eng->wake_pipe[0] = eng->wake_pipe[1] = -1;

    // 清理所有活动连接
    for (int i = 0; i < eng->cap; i++) {
        if (eng->pr_state[i] & PR_ST_ACTIVE) {
            if (eng->pr_fd_client[i] > 0) close(eng->pr_fd_client[i]);
            if (eng->pr_fd_upstream[i] > 0) close(eng->pr_fd_upstream[i]);
            eng->pr_state[i] = 0;
        }
        if (eng->pr_hbuf[i])     free(eng->pr_hbuf[i]);
        if (eng->pr_hbuf_out[i]) free(eng->pr_hbuf_out[i]);
        if (eng->pr_up_buf[i])   free(eng->pr_up_buf[i]);
        if (eng->pr_up_key && eng->pr_up_key[i]) free(eng->pr_up_key[i]);
    }
    // 销毁上游连接池（关闭全部空闲上游连接）
    if (eng->up_pool) { nl_px_pool_destroy(eng->up_pool); free(eng->up_pool); eng->up_pool = NULL; }
    close(eng->kqfd);
    eng->kqfd = -1;
    free(eng->pr_fd_client);   free(eng->pr_fd_upstream);   free(eng->pr_route_idx);
    free(eng->pr_state);       free(eng->pr_hbuf);          free(eng->pr_hbuf_out);
    free(eng->pr_up_buf);
    free(eng->pr_hdr_len);     free(eng->pr_remain);        free(eng->free);
    free(eng->pr_up);
    free(eng->pr_up_key);      free(eng->pr_last_active_ms);
    free(eng->pr_tls_done);    eng->pr_tls_done = NULL;
    eng->pr_last_active_ms = NULL;
    eng->pr_fd_client = NULL;
    eng->pr_fd_upstream = NULL;
    eng->pr_route_idx = NULL;
    eng->pr_state = NULL;
    eng->pr_hbuf = NULL;
    eng->pr_hbuf_out = NULL;
    eng->pr_up_buf = NULL;
    eng->pr_hdr_len = NULL;
    eng->pr_remain = NULL;
    eng->free = NULL;
    eng->pr_up = NULL;
    eng->pr_up_key = NULL;
    eng->cap = 0;
    __atomic_store_n(&eng->count, 0, __ATOMIC_RELAXED);   // 第188轮：与监督线程读侧配对
}

// 第188轮：排空 listen backlog（事件分支与退役排空共用）。ring 满计 overload。
static void pr_engine_accept_all(nl_web_macos_engine_t* eng, int listen_fd) {
    for (;;) {
        /* 第190轮：ring 满背压 —— 不 accept、不 close，把 listen 从 kqueue 摘除，
         * 连接留在内核 backlog 排队，等 pr_free_slot 归还槽位时再挂回。
         * 旧实现 accept 后直接 close，客户端收到无响应 RST。 */
        if (__atomic_load_n(&eng->count, __ATOMIC_RELAXED) >= eng->cap || eng->free_top < 0) {
            if (!eng->ring_full) {
                eng->ring_full = 1;
                __atomic_add_fetch(&eng->overload, 1, __ATOMIC_RELAXED);
                struct kevent dev;
                EV_SET(&dev, listen_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                kevent(eng->kqfd, &dev, 1, NULL, 0, NULL);
            }
            return;
        }
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) break;
        int cf = fcntl(cfd, F_GETFL, 0);
        fcntl(cfd, F_SETFL, cf | O_NONBLOCK);
        int si = pr_alloc_slot(eng);
        if (si < 0) {
            __atomic_add_fetch(&eng->overload, 1, __ATOMIC_RELAXED);
            close(cfd);
            continue;
        }
        eng->pr_fd_client[si] = cfd;
        eng->pr_state[si] = PR_ST_ACTIVE;
        // 第190轮：accept 只分配必需的 8KB 首包缓冲；
        // hbuf_out(8KB)/up_buf(64KB)/up_key(576B) 全部延迟到 proxy 路由分发时按需分配
        eng->pr_hbuf[si]     = (char*)malloc(8193);
        eng->pr_hdr_len[si]  = 0;
        eng->pr_remain[si]   = 0;
        // P7: 空闲超时（slowloris 防护）
        eng->pr_last_active_ms[si] = pr_pool_now_ms();
        // 重置上游响应 framing 状态
        nl_px_upstate_reset(&eng->pr_up[si]);
        if (!eng->pr_hbuf[si]) {
            close(cfd);
            pr_free_slot(eng, si);
            continue;
        }
        struct kevent ev;
        EV_SET(&ev, cfd, EVFILT_READ, EV_ADD, 0, 0, (void*)PR_UDATA(si, 0));
        kevent(eng->kqfd, &ev, 1, NULL, 0, NULL);
    }
}

// P6: 多实例引擎线程入口（SO_REUSEPORT）
struct pr_engine_thread_arg {
    nl_web_server_t* server;
    int listen_fd;
    int ring_cap;   // >0 时按指定容量初始化 ring；0 时自动
    int index;      // 引擎实例序号（定位 engines[index]）
};

static void* nl_web_proxy_engine_thread_multi(void* arg) {
    struct pr_engine_thread_arg* a = (struct pr_engine_thread_arg*)arg;
    nl_web_server_t* s = a->server;
    nl_web_macos_engine_t* eng = &s->engines[a->index];
    eng->sock = a->listen_fd;
    nl_web_proxy_engine(eng, a->listen_fd, a->ring_cap);
    __atomic_store_n(&eng->exited, 1, __ATOMIC_RELEASE);   // 第188轮：通知监督线程可 join
    free(a);   // P6: 线程自释放 arg（start 不持有，避免泄漏）
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

    // 拆分 host:port（支持 [::1]:8080；缺省端口按协议补）
    int def_port = (proto == NL_PROXY_HTTPS) ? 443 : 80;
    port = def_port;
    if (!nl_px_split_hostport(route->upstream, host, sizeof(host), &port, def_port)) {
        snprintf(host, sizeof(host), "%s", route->upstream);
        port = def_port;
    }

    // 地址解析：AF_UNSPEC 同时支持 IPv4 / IPv6 字面量与域名
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, portbuf, &hints, &res) != 0 || !res) {
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
        if (getaddrinfo("127.0.0.1", portbuf, &hints, &res) != 0 || !res) return -1;
    }

    int up_sock = socket(res->ai_family, SOCK_STREAM, 0);
    if (up_sock < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(up_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(up_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(up_sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(up_sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    // 完成反代：HTTPS 上游需要 TLS 会话能力。契约由公共层统一探测与描述，
    // 不满足必须让连接失败 —— 绝不"回退明文"。
    if (proto == NL_PROXY_HTTPS) {
        nl_px_tls_api api;
        if (!nl_px_tls_contract_get(&api)) {
            nl_log(NL_LOG_ERROR, "proxy: %s", nl_px_tls_contract_missing());
            close(up_sock);
            return -1;
        }
        /* TLS 握手/加密由后端隧道线程的连接会话（api.conn_wrap）完成；
         * 此处只确认契约完整，不能要求旧版 api.handshake 钩子。 */
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
    size_t req_line_end_off = 0;   // 首行结束（含换行）在 buffer 中的偏移，供反代透传定位
    {
        char* line_end = memchr(buffer, '\n', (size_t)received);
        if (!line_end || line_end == buffer) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            return;
        }
        req_line_end_off = (size_t)(line_end - buffer) + 1;
        if (nl_px_parse_request_line(buffer, (size_t)(line_end - buffer),
                                     method, sizeof(method),
                                     path, sizeof(path),
                                     protocol, sizeof(protocol)) != 0) {
            send_http_error(client, 400, "Bad Request");
            close(client);
            return;
        }
    }

    /* 第192轮：RFC 3986 端点规范化（剥 query/fragment、percent-decode、
     * dot-segment 归一）。路由查找与本地响应用 norm_path；代理转发
     * （反代请求行 / TCP 透传）保留原始 path（含 query，字节原样）。 */
    char norm_path[4096];
    if (strcmp(path, "*") == 0) {
        norm_path[0] = '*';        /* OPTIONS *（RFC 7230 §7.1）：无路由，维持 404 */
        norm_path[1] = '\0';
    } else {
        int uri_rc = nl_uri_normalize(path, norm_path, sizeof(norm_path));
        if (uri_rc != NL_URI_OK) {
            int esc = (uri_rc == NL_URI_ESCAPES);
            send_http_error(client, esc ? 403 : 400,
                            esc ? "Forbidden" : "Bad Request");
            close(client);
            return;
        }
    }

    pthread_mutex_lock(&server->mutex);
    // 使用 hash 表 O(1) 查找路由；若 hash 未初始化则回退到线性链表
    nl_web_route_t* route = nl_route_hash_lookup_simple(server, norm_path);
    if (!route) {
        // 第195轮：先精确 strcmp（hash 同款语义），未中再按最长前缀挑目录路由
        // ——"精确 > 最长前缀"与引擎路径 pr_lookup_route_idx 一致。
        nl_web_route_t* best = NULL;
        size_t best_len = 0;
        for (nl_web_route_t* r = server->routes; r; r = r->next) {
            if (strcmp(r->path, norm_path) == 0) { route = r; break; }
            if (r->is_prefix && nl_px_path_match_prefix(r->path, norm_path)) {
                size_t plen = strlen(r->path);
                if (!best || plen > best_len) { best = r; best_len = plen; }
            }
        }
        if (!route) route = best;
    }
    if (route) {
        if (route->type == NL_ROUTE_TYPE_REDIRECT) {
                // Send redirect（类型由 redirect_type 决定，默认 302）
                int rcode = (server->redirect_type == NL_REDIRECT_PERMANENT) ? 301 : 302;
                pthread_mutex_unlock(&server->mutex);
                char redirect_response[1024];
                int rn = nl_px_build_redirect_head(redirect_response, sizeof(redirect_response),
                                                   rcode, route->redirect_url);
                if (rn > 0) (void)write(client, redirect_response, (size_t)rn);
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
                    while ((n = recv(up_sock, proxy_buf, 65536, 0)) > 0) {
                        if (write(client, proxy_buf, n) < 0) break;
                    }
                    free(proxy_buf);
                    close(up_sock);
                    close(client);
                    return;
                }

                // HTTP/HTTPS：重组 request line + 重写 Host/XFF/Connection + 透传
                // 客户端原始头与 body（原实现硬编码 XFF 127.0.0.1，且 rest 按 C 串
                // 扫描，body 含 \0 即被截断）
                char upstream_req_full[12288];
                char peer_ip[48];
                peer_ip[0] = '\0';
                {
                    struct sockaddr_storage pa;
                    socklen_t pa_len = sizeof(pa);
                    memset(&pa, 0, sizeof(pa));
                    if (getpeername(client, (struct sockaddr*)&pa, &pa_len) == 0)
                        (void)nl_px_sockaddr_ip((const struct sockaddr*)&pa, peer_ip, sizeof(peer_ip));
                }
                /* 第195轮：目录转发改写（nginx proxy_pass URI 拼接）；
                 * up_uri 为空保持原样透传，与引擎路径同一份公共逻辑。 */
                char rpath[4096];
                const char* fwd_path = path;
                if (route->up_uri[0]) {
                    if (nl_px_dir_rewrite(route->path, route->is_prefix,
                                          route->up_uri, path,
                                          rpath, sizeof(rpath)) > 0)
                        fwd_path = rpath;
                }
                int total = snprintf(upstream_req_full, sizeof(upstream_req_full),
                    "%s %s %s\r\nHost: %s\r\n", method, fwd_path, protocol, route->upstream);
                if (total < 0 || (size_t)total >= sizeof(upstream_req_full)) total = 0;
                if (peer_ip[0]) {
                    int x = snprintf(upstream_req_full + total, sizeof(upstream_req_full) - (size_t)total,
                                     "X-Forwarded-For: %s\r\n", peer_ip);
                    if (x > 0 && (size_t)x < sizeof(upstream_req_full) - (size_t)total) total += x;
                }
                {
                    int x = snprintf(upstream_req_full + total, sizeof(upstream_req_full) - (size_t)total,
                                     "Connection: close\r\n");
                    if (x > 0 && (size_t)x < sizeof(upstream_req_full) - (size_t)total) total += x;
                }
                /* buffer 首行已被改写（'\n' 被置为 '\0'）：用解析阶段记录的
                 * req_line_end_off 定位首行之后的位置（不能再 memchr '\n'，
                 * 那个换行符已被抹掉，会误把首行内部当成行尾）。
                 * 客户端头区 = [首行后, 头终止符)，body = [终止符后, received)；
                 * 全程按 received 字节界处理，避免 C 串扫描把 body 截断。 */
                if (req_line_end_off > 0 && req_line_end_off <= (size_t)received) {
                    const char* hdrs = buffer + req_line_end_off;
                    const char* hend = NULL;
                    size_t hoff = (size_t)(hdrs - buffer);
                    for (size_t i = 0; hoff + i + 3 < (size_t)received; i++) {
                        if (hdrs[i] == '\r' && hdrs[i + 1] == '\n' &&
                            hdrs[i + 2] == '\r' && hdrs[i + 3] == '\n') {
                            hend = hdrs + i;
                            break;
                        }
                    }
                    if (hend) {
                        // 逐行透传客户端头，过滤上方已改写的 Host/XFF/Connection（防重复头）
                        const char* p = hdrs;
                        while (p < hend && total < (int)sizeof(upstream_req_full)) {
                            const char* nl = memchr(p, '\n', (size_t)(hend - p));
                            if (!nl) break;
                            const char* le = nl;
                            if (le > p && le[-1] == '\r') le--;
                            size_t llen = (size_t)(le - p);
                            int skip = nl_px_ci_starts(p, llen, "host:") ||
                                       nl_px_ci_starts(p, llen, "x-forwarded-for:") ||
                                       nl_px_ci_starts(p, llen, "connection:");
                            if (!skip && llen > 0 && memchr(p, ':', llen) &&
                                (size_t)total + llen + 2 < sizeof(upstream_req_full)) {
                                memcpy(upstream_req_full + total, p, llen);
                                total += (int)llen;
                                upstream_req_full[total++] = '\r';
                                upstream_req_full[total++] = '\n';
                            }
                            p = nl + 1;
                        }
                        // 空行终止符
                        if ((size_t)total + 2 < sizeof(upstream_req_full)) {
                            upstream_req_full[total++] = '\r';
                            upstream_req_full[total++] = '\n';
                        }
                        const char* body = hend + 4;
                        size_t body_n = (size_t)(buffer + received - body);
                        if (body_n > 0 && (size_t)total + body_n <= sizeof(upstream_req_full)) {
                            memcpy(upstream_req_full + total, body, body_n);
                            total += (int)body_n;
                        }
                    } else {
                        // 单包内头未收齐：按字节透传首行之后的剩余数据
                        size_t rest_n = (size_t)(buffer + received - hdrs);
                        if ((size_t)total + rest_n <= sizeof(upstream_req_full)) {
                            memcpy(upstream_req_full + total, hdrs, rest_n);
                            total += (int)rest_n;
                        }
                    }
                }
                // HTTPS 上游：请求必须经 TLS 会话发送，禁止明文回退
                if (route->protocol == NL_PROXY_HTTPS) {
                    nl_px_tls_api tls_api;
                    char up_host[512];
                    int up_port = 443;
                    up_host[0] = '\0';
                    (void)nl_px_split_hostport(route->upstream, up_host, sizeof(up_host),
                                               &up_port, 443);
                    if (!nl_px_tls_contract_get(&tls_api) || !tls_api.conn_wrap) {
                        nl_log(NL_LOG_ERROR, "proxy: %s", nl_px_tls_contract_missing());
                        free(proxy_buf);
                        close(up_sock);
                        send_http_error(client, 502, "Bad Gateway");
                        close(client);
                        return;
                    }
                    void* tls_conn = tls_api.conn_wrap(up_sock, up_host, NULL);
                    if (!tls_conn) {
                        nl_log(NL_LOG_ERROR,
                                  "proxy: TLS session unavailable for upstream %s (refusing plaintext fallback)",
                                  up_host);
                        free(proxy_buf);
                        close(up_sock);
                        send_http_error(client, 502, "Bad Gateway");
                        close(client);
                        return;
                    }
                    /* 隧道内部完成握手后的双向收发，并负责关闭双方 fd 与释放会话 */
                    (void)nl_px_tls_tunnel_run((intptr_t)client, (intptr_t)up_sock,
                                               tls_conn, &tls_api,
                                               upstream_req_full, total, 60000);
                    free(proxy_buf);
                    return;
                }
                ssize_t sent2 = send(up_sock, upstream_req_full, (size_t)total, 0);
                if (sent2 <= 0) {
                    free(proxy_buf);
                    close(up_sock);
                    send_http_error(client, 502, "Bad Gateway");
                    close(client);
                    return;
                }

                ssize_t n;
                while ((n = recv(up_sock, proxy_buf, 65536, 0)) > 0) {
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

/* 第189轮 stop 死锁修复：同 Linux —— worker 的 accept 无限阻塞时不检查
 * running，nl_web_stop 必须先 join 再关 listen fd → 空闲时 stop 卡死。
 * 先 poll(200ms) 等可读再 accept，空闲时最多 200ms 唤醒一次检查 running。 */
static int web_worker_accept(nl_web_server_t* server) {
    struct pollfd pfd;
    pfd.fd = server->sock;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int rc = poll(&pfd, 1, 200);
    if (rc <= 0) return -1;
    int c = accept(server->sock, NULL, NULL);
    /* BSD/macOS：accept 的新 fd 继承 listener 非阻塞位，须恢复阻塞 */
    if (c >= 0) set_blocking(c);
    return c;
}

static void* nl_worker_thread(void* arg) {
    nl_web_server_t* server = (nl_web_server_t*)arg;
    while (server->running) {
        int client = web_worker_accept(server);
        if (client < 0) {
            if (!server->running) break;
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
        int client = web_worker_accept(server);
        if (client < 0) {
            if (!server->running) break;
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
    server->listen_mode = -1;  // 未显式设置：启动时读 NETLEAF_LISTEN_MODE（默认双栈）
    server->sock = -1;  // P6: listen socket 由 nl_web_start 创建
    server->redirect_type = NL_REDIRECT_TEMPORARY; // 默认 302
    pthread_mutex_init(&server->mutex, NULL);
    pthread_mutex_init(&server->engine_lock, NULL);   // 第188轮：伸缩保护
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
    pthread_mutex_destroy(&server->engine_lock);   // 第188轮
    free(server->route_hash);
    free(server->workers);
    // P6: 兜底释放引擎状态块（正常已在 nl_web_stop 释放）
    free(server->engines);
    free(server->engine_threads);
    free(server);
}

/* 监听地址：模式由 nl_px_listen_mode_resolve 决定（显式 API > 环境变量 > 双栈）。
 * DUAL = :: + IPV6_V6ONLY=0；V6 = :: + V6ONLY=1；V4 = 0.0.0.0。 */
static int nl_web_listen_addr(nl_web_server_t* server, struct sockaddr_storage* ss,
                              socklen_t* len) {
    int mode = nl_px_listen_mode_resolve(server->listen_mode);
    memset(ss, 0, sizeof(*ss));
    if (mode != NL_PX_LISTEN_V4) {
        struct sockaddr_in6* a6 = (struct sockaddr_in6*)ss;
        a6->sin6_family = AF_INET6;
        a6->sin6_addr = in6addr_any;
        a6->sin6_port = htons((unsigned short)server->port);
        *len = (socklen_t)sizeof(*a6);
        return AF_INET6;
    }
    struct sockaddr_in* a4 = (struct sockaddr_in*)ss;
    a4->sin_family = AF_INET;
    a4->sin_addr.s_addr = INADDR_ANY;
    a4->sin_port = htons((unsigned short)server->port);
    *len = (socklen_t)sizeof(*a4);
    return AF_INET;
}

/* 批量创建/绑定/监听 N 个 socket；DUAL 失败自动整体回退 IPv4。
 * macOS 要求所有 socket 在 bind 前已设好 SO_REUSEPORT，故分三步。 */
static int nl_web_open_listens(nl_web_server_t* server, int* fds, int n,
                               int* af_out, int opt) {
    struct sockaddr_storage addr;
    socklen_t alen = 0;
    int mode = nl_px_listen_mode_resolve(server->listen_mode);
    int af = nl_web_listen_addr(server, &addr, &alen);
    for (int attempt = 0; attempt < 2; attempt++) {
        int cnt = 0, failed = 0;
        for (int i = 0; i < n; i++) {
            int sk = socket(af, SOCK_STREAM, 0);
            if (sk < 0) { failed = 1; break; }
            if (af == AF_INET6) {
                int v6only = (mode == NL_PX_LISTEN_V6) ? 1 : 0;
                setsockopt(sk, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
            }
            setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            if (n > 1 || server->engine_autoscale) {
                // P6/第188轮：多实例或 auto 伸缩模式（扩容实例需与既有实例共 bind）
                setsockopt(sk, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
            }
            fds[cnt++] = sk;
        }
        if (!failed) {
            for (int i = 0; i < cnt; i++)
                if (bind(fds[i], (struct sockaddr*)&addr, alen) < 0) { failed = 1; break; }
        }
        if (!failed) {
            for (int i = 0; i < cnt; i++)
                if (listen(fds[i], SOMAXCONN) < 0) { failed = 1; break; }
        }
        if (!failed) { *af_out = af; return cnt; }
        for (int i = 0; i < cnt; i++) close(fds[i]);
        if (af != AF_INET6) return -1;
        if (mode == NL_PX_LISTEN_V6) return -1;   // 显式要求 V6 时不静默回退
        struct sockaddr_in* a4 = (struct sockaddr_in*)&addr;
        memset(&addr, 0, sizeof(addr));
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = INADDR_ANY;
        a4->sin_port = htons((unsigned short)server->port);
        alen = (socklen_t)sizeof(*a4);
        af = AF_INET;
    }
    return -1;
}

/* ---- 第188轮：Web 引擎实例自动伸缩（与 Linux 同构） ---- */

static int nl_web_engine_slots_used(nl_web_server_t* s) {
    if (!s->engines || s->engine_count <= 0) return 0;
    int n = 0;
    for (int i = 0; i < s->engine_count; i++)
        if (s->engines[i].scale_state != 0) n++;
    return n;
}

/* 为新实例创建 listen socket（macOS 要求所有 socket bind 前设 SO_REUSEPORT） */
static int nl_web_engine_listen(nl_web_server_t* s, int opt) {
    struct sockaddr_storage addr;
    socklen_t alen = 0;
    int mode = nl_px_listen_mode_resolve(s->listen_mode);
    int af = nl_web_listen_addr(s, &addr, &alen);
    for (int attempt = 0; attempt < 2; attempt++) {
        int sk = socket(af, SOCK_STREAM, 0);
        if (sk < 0) return -1;
        if (af == AF_INET6) {
            int v6only = (mode == NL_PX_LISTEN_V6) ? 1 : 0;
            setsockopt(sk, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        }
        setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        setsockopt(sk, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));   // 与既有实例共存
        if (bind(sk, (struct sockaddr*)&addr, alen) == 0 &&
            listen(sk, SOMAXCONN) == 0) {
            s->af = af;
            return sk;
        }
        close(sk);
        if (af != AF_INET6 || mode == NL_PX_LISTEN_V6) return -1;   // 显式 V6 不静默回退
        struct sockaddr_in* a4 = (struct sockaddr_in*)&addr;
        memset(&addr, 0, sizeof(addr));
        a4->sin_family = AF_INET;
        a4->sin_addr.s_addr = INADDR_ANY;
        a4->sin_port = htons((unsigned short)s->port);
        alen = (socklen_t)sizeof(*a4);
        af = AF_INET;
    }
    return -1;
}

/* 扩容：在空闲槽位上开一个新引擎实例。调用方必须持有 engine_lock。 */
static int nl_web_engine_scale_up(nl_web_server_t* s) {
    if (!s->engines || !s->engine_threads || s->engine_count <= 0) return -1;
    int slot = -1;
    for (int i = 0; i < s->engine_count; i++) {
        if (s->engines[i].scale_state == 0) { slot = i; break; }
    }
    if (slot < 0) return -1;   // 无空闲槽位 = 已到上限

    int opt = 1;
    int sk = nl_web_engine_listen(s, opt);
    if (sk < 0) return -1;

    int live = nl_web_engine_slots_used(s) + 1;
    int total = s->max_conns_cfg > 0 ? s->max_conns_cfg : nl_web_proxy_ring_capacity();
    int per_cap = total / (live > 0 ? live : 1);
    if (per_cap < 64) per_cap = 64;
    if (per_cap > 4096) per_cap = 4096;

    nl_web_macos_engine_t* e = &s->engines[slot];
    memset(e, 0, sizeof(*e));
    e->server = s;
    e->kqfd = -1;
    e->sock = sk;
    e->idx = slot;
    e->scale_state = 1;

    struct pr_engine_thread_arg* a =
        (struct pr_engine_thread_arg*)malloc(sizeof(struct pr_engine_thread_arg));
    if (!a) { close(sk); e->sock = -1; e->scale_state = 0; return -1; }
    a->server = s;
    a->listen_fd = sk;
    a->ring_cap = per_cap;
    a->index = slot;
    if (pthread_create(&s->engine_threads[slot], NULL,
                       nl_web_proxy_engine_thread_multi, a) != 0) {
        free(a); close(sk); e->sock = -1; e->scale_state = 0;
        return -1;
    }
    s->engine_threads_ready++;
    nl_log(NL_LOG_INFO, "Web: 引擎扩容 -> %d 实例（槽位 %d）",
              nl_web_engine_slots_used(s), slot);
    return 0;
}

/* 缩容：请求最高槽位的运行中实例退役。调用方必须持有 engine_lock。 */
static int nl_web_engine_scale_down(nl_web_server_t* s) {
    int slot = -1;
    for (int i = s->engine_count - 1; i >= 1; i--) {   // engine 0 永不退役
        if (s->engines[i].scale_state == 1) { slot = i; break; }
    }
    if (slot < 0) return -1;
    s->engines[slot].scale_state = 2;   // 退役排空中
    __atomic_store_n(&s->engines[slot].retire, 1, __ATOMIC_RELEASE);
    nl_log(NL_LOG_INFO, "Web: 引擎缩容请求 -> %d 实例（槽位 %d 排空中）",
              nl_web_engine_slots_used(s), slot);
    return 0;
}

/* 回收已退出的退役线程，释放槽位。调用方必须持有 engine_lock。 */
static void nl_web_engine_reap(nl_web_server_t* s) {
    for (int i = 0; i < s->engine_count; i++) {
        nl_web_macos_engine_t* e = &s->engines[i];
        if (e->scale_state == 2 &&
            __atomic_load_n(&e->exited, __ATOMIC_ACQUIRE) &&
            s->engine_threads[i] != 0) {
            pthread_join(s->engine_threads[i], NULL);
            s->engine_threads[i] = 0;
            e->scale_state = 0;
        }
    }
}

static void* nl_web_engine_scaler(void* arg) {
    nl_web_server_t* s = (nl_web_server_t*)arg;
    int64_t last_scale_ms = pr_pool_now_ms();
    int idle_ticks = 0;
    while (__atomic_load_n(&s->running, __ATOMIC_RELAXED)) {
        usleep(NL_ENGINE_SCALE_SAMPLE_MS * 1000);
        if (!__atomic_load_n(&s->running, __ATOMIC_RELAXED)) break;

        int64_t now = pr_pool_now_ms();
        pthread_mutex_lock(&s->engine_lock);
        nl_web_engine_reap(s);
        int live = nl_web_engine_slots_used(s);
        if (live <= 0 || s->engine_slots <= 0 || !s->engines) {
            pthread_mutex_unlock(&s->engine_lock);
            continue;
        }
        int total = 0, over = 0;
        for (int i = 0; i < s->engine_count; i++) {
            nl_web_macos_engine_t* e = &s->engines[i];
            if (e->scale_state != 1) continue;
            total += __atomic_load_n(&e->count, __ATOMIC_RELAXED);
            over += __atomic_exchange_n(&e->overload, 0, __ATOMIC_RELAXED);
        }
        if (over > 0 || total >= live * NL_ENGINE_SCALE_UP_PER_CONN) {
            idle_ticks = 0;   // 高负载：先清空闲计数再扩容
            if (live < s->engine_slots && now - last_scale_ms >= 500) {
                if (nl_web_engine_scale_up(s) == 0) last_scale_ms = now;
            }
        } else if (total <= live * NL_ENGINE_SCALE_DOWN_PER_CONN) {
            idle_ticks++;
            if (idle_ticks >= NL_ENGINE_SCALE_DOWN_TICKS &&
                live > NL_ENGINE_MIN_INSTANCES &&
                now - last_scale_ms >= NL_ENGINE_SCALE_COOLDOWN_MS) {
                if (nl_web_engine_scale_down(s) == 0) {
                    last_scale_ms = now; idle_ticks = 0;
                }
            }
        } else {
            idle_ticks = 0;
        }
        pthread_mutex_unlock(&s->engine_lock);
    }
    return NULL;
}

int nl_web_start(nl_web_server_t* server) {
    if (!server || server->running) return -1;

    int opt = 1;
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    int af = nl_web_listen_addr(server, &addr, &addr_len);
    server->af = af;
    (void)addr;
    (void)addr_len;

    // 检测是否存在 proxy 路由：有则走 kqueue 数据驱动引擎，
    // 否则回退到 worker 池（兼容旧行为：static/content/redirect/file 路由）。
    // P5: force_legacy_pool=1 强制走 worker 池（回退开关）
    int has_proxy = 0;
    {
        pthread_mutex_lock(&server->mutex);
        for (nl_web_route_t* r = server->routes; r; r = r->next)
            if (r->type == NL_ROUTE_TYPE_PROXY) { has_proxy = 1; break; }
        pthread_mutex_unlock(&server->mutex);
    }
    if (server->force_legacy_pool) has_proxy = 0;

    // P5/P6: 默认走数据驱动引擎（kqueue），除非显式 nl_web_force_legacy_pool(1)
    // 第188轮：engine_threads_cfg <= 0 = auto 模式（按负载在 [1, slots] 间伸缩）
    int n_engines = 0;   // 初始实例数
    int slots = 0;       // 槽位数组容量 = 伸缩上限
    int autoscale = 0;
    if (!has_proxy) {
        n_engines = 0;  // legacy worker 池
    } else if (server->engine_threads_cfg <= 0) {
        autoscale = 1;
        n_engines = 1;
        slots = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (slots < 1) slots = 1;
        if (slots > NL_ENGINE_MAX_INSTANCES) slots = NL_ENGINE_MAX_INSTANCES;
    } else {
        n_engines = server->engine_threads_cfg;
        if (n_engines > 256) n_engines = 256;   // listen_fds[256]
        slots = n_engines;
    }

    if (n_engines >= 1) {
        // 第188轮：只开初始实例数的 listen fd（扩容实例由 nl_web_engine_listen 按需创建，
        // 每个 socket 各自 bind 前设 SO_REUSEPORT 即可，无需批量预开）；
        // 槽位数组仍按伸缩上限 slots 分配。
        int listen_fds[256];
        int sock_cnt = 0;
        if (slots > 256) slots = 256;
        if (n_engines > slots) n_engines = slots;
        server->engine_autoscale = autoscale;   // open_listens 据此决定是否设 SO_REUSEPORT
        {
            int got = nl_web_open_listens(server, listen_fds, n_engines, &af, opt);
            if (got < 0) return -1;
            sock_cnt = got;
            server->af = af;
        }
        // 主 sock 记第一个（兜底 nl_web_stop 清理）；其余由 engines[i].sock 持有
        server->sock = listen_fds[0];

        // 第188轮：重启清理上一轮残留（防指针覆盖泄漏）
        if (server->engines) { free(server->engines); server->engines = NULL; }
        if (server->engine_slots > 0 && server->engine_threads) {
            free(server->engine_threads); server->engine_threads = NULL;
        }
        server->engine_count = 0;
        server->engine_slots = 0;

        server->engines = (nl_web_macos_engine_t*)calloc((size_t)slots, sizeof(nl_web_macos_engine_t));
        if (!server->engines) {
            for (int j = 0; j < sock_cnt; j++) close(listen_fds[j]);
            server->sock = -1;
            return -1;
        }
        for (int i = 0; i < slots; i++) {
            server->engines[i].server = server;
            server->engines[i].kqfd = -1;
            server->engines[i].sock = -1;
            server->engines[i].idx = i;
            server->engines[i].scale_state = 0;   // 线程创建成功后才置 1
        }
        for (int i = 0; i < n_engines && i < sock_cnt; i++) {
            server->engines[i].sock = listen_fds[i];   // 启动前记归属
        }
        server->engine_count = slots;
        server->engine_slots = slots;
        server->engine_autoscale = autoscale;

        // P6: 总连接预算按槽位均分（auto 按上限均分，扩容不破 max_conns 预算）
        int total_conns = server->max_conns_cfg;
        if (total_conns <= 0) total_conns = nl_web_proxy_ring_capacity();
        int per_cap = total_conns / slots;
        if (per_cap < 64) per_cap = 64;
        if (per_cap > 4096) per_cap = 4096;

        server->engine_threads = (pthread_t*)calloc((size_t)slots, sizeof(pthread_t));
        if (!server->engine_threads) {
            free(server->engines);
            server->engines = NULL;
            server->engine_count = 0;
            server->engine_slots = 0;
            for (int j = 0; j < sock_cnt; j++) close(listen_fds[j]);
            server->sock = -1;
            return -1;
        }
        server->engine_threads_ready = 0;
        server->running = 1;  // 先置 running，再创建线程（引擎循环检查 s->running）
        for (int i = 0; i < n_engines; i++) {
            struct pr_engine_thread_arg* a =
                (struct pr_engine_thread_arg*)malloc(sizeof(struct pr_engine_thread_arg));
            if (!a) {
                for (int j = i; j < sock_cnt; j++) close(listen_fds[j]);
                if (i == 0) server->sock = -1;
                nl_log(NL_LOG_ERROR, "Web: 引擎参数分配失败，已启动 %d 个实例", i);
                break;
            }
            a->server = server;
            a->listen_fd = listen_fds[i];
            a->ring_cap = per_cap;
            a->index = i;
            if (pthread_create(&server->engine_threads[i], NULL,
                               nl_web_proxy_engine_thread_multi, a) != 0) {
                free(a);
                for (int j = i; j < sock_cnt; j++) {
                    close(listen_fds[j]);
                    if (j < slots) {
                        server->engines[j].sock = -1;
                        server->engines[j].scale_state = 0;
                    }
                }
                if (i == 0) server->sock = -1;
                nl_log(NL_LOG_ERROR, "Web: 引擎线程创建失败，已启动 %d 个实例", i);
                break;
            }
            // 第188轮：把已开的 listen fd 记到本槽位（后续槽位的 fd 留给扩容时使用）
            server->engines[i].sock = listen_fds[i];
            server->engines[i].scale_state = 1;
            server->engine_threads_ready++;
        }
        if (server->engine_threads_ready == 0) {
            // 一个引擎都没起来：回滚
            server->running = 0;
            free(server->engine_threads);
            free(server->engines);
            server->engine_threads = NULL;
            server->engines = NULL;
            server->engine_count = 0;
            server->engine_slots = 0;
            for (int j = 0; j < sock_cnt; j++) close(listen_fds[j]);
            server->sock = -1;
            return -1;
        }
        if (autoscale) {
            // 第188轮：伸缩监督线程（auto 模式）
            if (pthread_create(&server->scaler_thread, NULL,
                               nl_web_engine_scaler, server) == 0) {
                server->scaler_alive = 1;
            } else {
                nl_log(NL_LOG_WARN, "Web: 伸缩监督线程创建失败，实例数固定为 %d",
                          server->engine_threads_ready);
            }
        }
        return 0;
    }

    // legacy worker 池分支（P5 回退开关 force_legacy_pool=1 或无 proxy 路由）
    {
        int one[1];
        int got = nl_web_open_listens(server, one, 1, &af, opt);
        if (got < 1) return -1;
        server->af = af;
        server->sock = one[0];
        /* 第189轮：listen 非阻塞，防多 worker 竞争 accept 时输家永久挂起（同 Linux） */
        set_nonblocking(server->sock);
    }
    server->running = 1;

    // 全动态化：worker 池容量按需分配
    int target = server->worker_count > 0 ? server->worker_count : WEB_WORKER_DEFAULT;
    if (target > 64) target = 64;
    if (server->worker_capacity < target) {
        pthread_t* new_workers = (pthread_t*)realloc(server->workers,
                                                     (size_t)target * sizeof(pthread_t));
        if (!new_workers) { close(server->sock); server->sock = -1; server->running = 0; return -1; }
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

// P6: 引擎实例数（0 = 自动伸缩：按负载在 [1, min(逻辑核数,32)] 间增减；>0 = 固定实例数）。
// 在 nl_web_start 前调用生效；已 start 的服务器下次重启后生效。
int nl_web_set_engine_threads(nl_web_server_t* server, int n) {
    if (!server) return -1;
    if (n < 0) n = 0;
    if (n > 128) n = 128;
    server->engine_threads_cfg = n;
    return 0;
}

// P6: 总连接预算。macOS kqueue 引擎按 N 均分为每引擎 ring cap（nl_web_start 生效）。
int nl_web_set_max_conns(nl_web_server_t* server, int total) {
    if (!server) return -1;
    if (total < 0) total = 0;
    server->max_conns_cfg = total;
    return 0;
}

// P7: 空闲超时。macOS kqueue 引擎已实现节流空闲扫描（nl_web_start 生效，0=默认 30000ms）。
int nl_web_set_idle_timeout(nl_web_server_t* server, int ms) {
    if (!server) return -1;
    if (ms < 0) ms = 0;
    server->idle_timeout_ms = ms;
    return 0;
}

// P5: 强制 legacy worker 池。macOS kqueue 引擎支持 force_legacy_pool 回退。
int nl_web_force_legacy_pool(nl_web_server_t* server, int enable) {
    if (!server) return -1;
    server->force_legacy_pool = enable ? 1 : 0;
    return 0;
}

// 监听地址族模式（须在 nl_web_start 之前设置）。
int nl_web_set_listen_mode(nl_web_server_t* server, nl_web_listen_mode_t mode) {
    if (!server) return NL_EINVAL;
    if (mode != NL_WEB_LISTEN_DUAL && mode != NL_WEB_LISTEN_V4 &&
        mode != NL_WEB_LISTEN_V6)
        return NL_EINVAL;
    server->listen_mode = (int)mode;   /* 与 NL_PX_LISTEN_* 数值一致 */
    return NL_OK;
}

void nl_web_stop(nl_web_server_t* server) {
    if (!server || !server->running) return;
    __atomic_store_n(&server->running, 0, __ATOMIC_RELAXED);
    // 第188轮：先 join 伸缩监督线程（它会做最后一轮 reap），再 join 引擎线程
    if (server->scaler_alive) {
        pthread_join(server->scaler_thread, NULL);
        server->scaler_alive = 0;
    }
    // legacy worker 主线程（引擎模式下 server->thread 为 0）
    if (server->thread) {
        pthread_join(server->thread, NULL);
        server->thread = 0;
    }
    // 第188轮：join 全部引擎线程（含退役排空中实例；已被 reap join 过的句柄为 0，跳过）
    if (server->engine_threads) {
        for (int i = 0; i < server->engine_count; i++) {
            if (server->engine_threads[i]) {
                pthread_join(server->engine_threads[i], NULL);
                server->engine_threads[i] = 0;
            }
        }
        free(server->engine_threads);
        server->engine_threads = NULL;
    }
    // 清理 worker 线程（兜底：正常情况下 web_server_thread 内已 join）
    for (int i = 0; i < server->worker_count; i++) {
        if (server->workers && server->workers[i]) {
            pthread_join(server->workers[i], NULL);
            server->workers[i] = 0;
        }
    }
    // P6: close 所有 listen fd（engines[i].sock；server->sock 即 engines[0].sock，去重）
    if (server->engines) {
        for (int i = 0; i < server->engine_count; i++) {
            int efd = server->engines[i].sock;
            if (efd >= 0 && efd != server->sock) close(efd);
            server->engines[i].sock = -1;
            // 兜底：kqueue fd / 上游连接池（正常已在引擎退出时清理）
            if (server->engines[i].kqfd > 0) {
                close(server->engines[i].kqfd);
                server->engines[i].kqfd = -1;
            }
            if (server->engines[i].up_pool) {
                nl_px_pool_destroy(server->engines[i].up_pool);
                free(server->engines[i].up_pool);
                server->engines[i].up_pool = NULL;
            }
        }
        // P6: 释放每引擎状态块（重复 start/stop 不泄漏）
        free(server->engines);
        server->engines = NULL;
    }
    server->engine_count = 0;
    server->engine_threads_ready = 0;
    if (server->sock >= 0) {
        close(server->sock);
        server->sock = -1;
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

void nl_web_set_redirect_type(nl_web_server_t* server, nl_redirect_type_t type) {
    if (!server) return;
    if (type != NL_REDIRECT_PERMANENT && type != NL_REDIRECT_TEMPORARY) {
        printf("nl_web_set_redirect_type: invalid type %d, keeping %d\n",
               (int)type, (int)server->redirect_type);
        return;
    }
    pthread_mutex_lock(&server->mutex);
    server->redirect_type = type;
    pthread_mutex_unlock(&server->mutex);
}

nl_redirect_type_t nl_web_get_redirect_type(nl_web_server_t* server) {
    if (!server) return NL_REDIRECT_TEMPORARY;
    pthread_mutex_lock(&server->mutex);
    nl_redirect_type_t t = server->redirect_type;
    pthread_mutex_unlock(&server->mutex);
    return t;
}

// 解析 upstream 字符串为 host + port + protocol（支持 scheme 前缀或默认端口）
// 第195轮：新增 uri_out —— authority 之后的第一个 '/' 起为上游 URI 部分
// （如 http://backend/v1/ 的 "/v1/"），供目录转发的 proxy_pass URI 拼接使用。
// 返回 0 成功，-1 失败。host/port/uri 由调用方提供足够缓冲。
static int nl_parse_upstream(const char* upstream, char* host, size_t host_size,
                             int* port_out, nl_proxy_protocol_t* proto_out,
                             char* uri_out, size_t uri_cap) {
    if (!upstream || upstream[0] == '\0') return -1;
    if (uri_out && uri_cap > 0) uri_out[0] = '\0';

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

    // 分离 authority 与 URI（第一个 '/' 起为 URI；IPv6 [::1]:8080 不含 '/' 不受影响）
    const char* slash = strchr(p, '/');
    char host_port[512];
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len >= sizeof(host_port)) len = sizeof(host_port) - 1;
    memcpy(host_port, p, len);
    host_port[len] = '\0';
    if (slash && uri_out && uri_cap > 0) snprintf(uri_out, uri_cap, "%s", slash);

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
// 共享的路由创建实现（第195轮抽出，供精确/目录两个 API 复用）
static int nl_web_add_proxy_common(nl_web_server_t* server, const char* path,
                                   const char* upstream, int is_prefix) {
    if (!server || !path || !upstream) return NL_EINVAL;

    char host[512];
    int port = 0;
    nl_proxy_protocol_t proto = NL_PROXY_HTTP;
    char up_uri[256];
    if (nl_parse_upstream(upstream, host, sizeof(host), &port, &proto,
                          up_uri, sizeof(up_uri)) != 0) {
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
        route->is_prefix = (unsigned char)(is_prefix ? 1 : 0);
        snprintf(route->up_uri, sizeof(route->up_uri), "%s", up_uri);
        route->content = NULL;
        route->content_size = 0;
        route->file_path[0] = '\0';
        route->redirect_url[0] = '\0';
        route->next = server->routes;
        server->routes = route;
        nl_route_hash_insert_simple(server, route);
        pthread_mutex_unlock(&server->mutex);
        printf("Added proxy route: %s -> %s%s%s (%d)\n", path, route->upstream,
               up_uri[0] ? " " : "", up_uri[0] ? up_uri : "", port);
        return NL_OK;
    }
    pthread_mutex_unlock(&server->mutex);
    return NL_ENOMEM;
}

int nl_web_add_proxy(nl_web_server_t* server, const char* path, const char* upstream) {
    return nl_web_add_proxy_common(server, path, upstream, 0);
}

// 第195轮：目录（前缀）转发 —— nginx `location <prefix> { proxy_pass ... }` 语义。
// 命中规则：精确匹配优先，其余按最长前缀选择；upstream 可带 URI
// （http://backend/v1/）命中后剥离前缀拼接，不带则完整路径原样透传。
int nl_web_add_proxy_dir(nl_web_server_t* server, const char* prefix, const char* upstream) {
    if (!prefix || prefix[0] != '/') return NL_EINVAL;   // 目录前缀必须是绝对路径
    return nl_web_add_proxy_common(server, prefix, upstream, 1);
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
        pthread_mutex_destroy(&server->engine_lock);   // 第188轮
        free(server->route_hash);
        free(server->workers);
        // P6: 兜底释放引擎状态块（正常已在 nl_web_stop 释放）
        free(server->engines);
        free(server->engine_threads);
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
