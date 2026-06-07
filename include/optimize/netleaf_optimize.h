#ifndef NETLEAF_OPTIMIZE_H
#define NETLEAF_OPTIMIZE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct nl_memory_pool nl_memory_pool_t;
typedef struct nl_ring_buffer nl_ring_buffer_t;
typedef struct nl_slice nl_slice_t;

typedef struct nl_file_transfer {
    int source_fd;
    int dest_fd;
    size_t total_bytes;
    size_t transferred;
    int cancelled;
    void (*progress_callback)(size_t transferred, size_t total, void* user_data);
    void* user_data;
} nl_file_transfer_t;

nl_memory_pool_t* nl_mempool_create(size_t block_size, size_t pool_size);
void nl_mempool_destroy(nl_memory_pool_t* pool);
void* nl_mempool_alloc(nl_memory_pool_t* pool);
void nl_mempool_free(nl_memory_pool_t* pool, void* ptr);

nl_ring_buffer_t* nl_ringbuf_create(size_t capacity);
void nl_ringbuf_destroy(nl_ring_buffer_t* buf);
size_t nl_ringbuf_write(nl_ring_buffer_t* buf, const void* data, size_t len);
size_t nl_ringbuf_read(nl_ring_buffer_t* buf, void* data, size_t len);
size_t nl_ringbuf_available(nl_ring_buffer_t* buf);
size_t nl_ringbuf_free_space(nl_ring_buffer_t* buf);
void nl_ringbuf_clear(nl_ring_buffer_t* buf);

nl_slice_t* nl_slice_create(const void* data, size_t len);
void nl_slice_destroy(nl_slice_t* slice);
const void* nl_slice_data(const nl_slice_t* slice);
size_t nl_slice_length(const nl_slice_t* slice);
nl_slice_t* nl_slice_sub(const nl_slice_t* slice, size_t offset, size_t len);

int nl_zero_copy_send(int fd, const void* buf, size_t len);
int nl_zero_copy_send_file(int sock_fd, int file_fd, off_t offset, size_t len);
void* nl_zero_copy_map_file(const char* path, size_t* size);
void nl_zero_copy_unmap(void* addr, size_t size);

int nl_file_get_size(const char* path, size_t* size);
int nl_file_exists(const char* path);
int nl_file_read(const char* path, void* buf, size_t len, size_t* bytes_read);
int nl_file_write(const char* path, const void* buf, size_t len, int append);
int nl_file_delete(const char* path);
int nl_file_append(const char* path, const void* buf, size_t len);
int nl_file_copy(const char* src_path, const char* dest_path, size_t* bytes_copied);
int nl_file_move(const char* src_path, const char* dest_path);

int nl_file_send(int sock_fd, const char* file_path, off_t offset, size_t len, 
                 void (*progress)(size_t, size_t, void*), void* user_data);
int nl_file_recv(int sock_fd, const char* file_path, size_t max_len, size_t* received);

void nl_optimize_enable(void);
void nl_optimize_disable(void);
int nl_optimize_is_enabled(void);

/* IoT Device Optimization Configuration */
typedef enum {
    NL_IOT_OPT_MINIMUM = 0,    /* 最小内存模式 - 适合极小设备 */
    NL_IOT_OPT_BASIC = 1,      /* 基础模式 - 适合普通 IoT 设备 */
    NL_IOT_OPT_BALANCED = 2,   /* 平衡模式 - 性能和内存平衡 */
    NL_IOT_OPT_STANDARD = 3    /* 标准模式 - 完整功能 */
} nl_iot_optimization_level_t;

typedef struct nl_iot_config {
    /* 功能开关 */
    int enable_memory_pool;       /* 启用/禁用内存池 */
    int enable_ring_buffer;       /* 启用/禁用环形缓冲区 */
    int enable_zero_copy;         /* 启用/禁用零拷贝传输 */
    int enable_http_server;       /* 启用/禁用 HTTP 服务器 */
    int enable_websocket;         /* 启用/禁用 WebSocket */
    int enable_file_operations;   /* 启用/禁用文件操作 */
    int enable_udp_support;       /* 启用/禁用 UDP 支持 */
    
    /* 内存配置 */
    size_t default_mempool_size;  /* 默认内存池大小 */
    size_t default_ringbuf_size;  /* 默认环形缓冲区大小 */
    size_t max_buffer_size;       /* 最大缓冲区大小 */
    size_t max_connections;       /* 最大连接数 */
    
    /* 性能配置 */
    int enable_aggressive_gc;     /* 启用激进内存回收 */
    int reduce_thread_priority;   /* 降低线程优先级 */
} nl_iot_config_t;

/* IoT 优化模式 API */
void nl_iot_set_optimization_level(nl_iot_optimization_level_t level);
nl_iot_optimization_level_t nl_iot_get_optimization_level(void);

void nl_iot_set_config(const nl_iot_config_t* config);
void nl_iot_get_config(nl_iot_config_t* config);

void nl_iot_apply_minimum_config(nl_iot_config_t* config);
void nl_iot_apply_basic_config(nl_iot_config_t* config);
void nl_iot_apply_balanced_config(nl_iot_config_t* config);
void nl_iot_apply_standard_config(nl_iot_config_t* config);

int nl_iot_is_feature_enabled(int feature_flag);

#define NL_IOT_FEATURE_MEMPOOL     (1 << 0)
#define NL_IOT_FEATURE_RINGBUF     (1 << 1)
#define NL_IOT_FEATURE_ZEROCOPY    (1 << 2)
#define NL_IOT_FEATURE_HTTP        (1 << 3)
#define NL_IOT_FEATURE_WEBSOCKET   (1 << 4)
#define NL_IOT_FEATURE_FILE        (1 << 5)
#define NL_IOT_FEATURE_UDP         (1 << 6)

#endif
