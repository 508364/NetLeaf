#define _POSIX_C_SOURCE 200809L
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>

#include "netleaf_optimize.h"

#define ALIGN_SIZE 64
#define CACHE_LINE_SIZE 64

static volatile int optimization_enabled = 1;

struct nl_memory_block {
    struct nl_memory_block* next;
    int in_use;
    char data[0];
};

struct nl_memory_pool {
    size_t block_size;
    size_t pool_size;
    void* memory;
    struct nl_memory_block* free_list;
    pthread_mutex_t mutex;
    int fd;
};

struct nl_ring_buffer {
    char* buffer;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    pthread_mutex_t mutex;
};

struct nl_slice {
    const void* data;
    size_t length;
};

static void* align_ptr(void* ptr, size_t align) __attribute__((unused));
static void* align_ptr(void* ptr, size_t align) {
    return (void*)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

nl_memory_pool_t* nl_mempool_create(size_t block_size, size_t pool_size) {
    if (block_size < sizeof(struct nl_memory_block)) {
        block_size = sizeof(struct nl_memory_block);
    }
    
    block_size = (block_size + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);
    
    nl_memory_pool_t* pool = calloc(1, sizeof(nl_memory_pool_t));
    if (!pool) return NULL;
    
    pool->block_size = block_size;
    pool->pool_size = pool_size;
    
    size_t total_size = block_size * pool_size;
    
    pool->memory = malloc(total_size);
    if (!pool->memory) {
        free(pool);
        return NULL;
    }
    memset(pool->memory, 0, total_size);
    
    if (!pool->memory) {
        free(pool);
        return NULL;
    }
    
    pthread_mutex_init(&pool->mutex, NULL);
    
    char* ptr = (char*)pool->memory;
    for (size_t i = 0; i < pool_size; i++) {
        struct nl_memory_block* block = (struct nl_memory_block*)ptr;
        block->next = pool->free_list;
        block->in_use = 0;
        pool->free_list = block;
        ptr += block_size;
    }
    
    return pool;
}

void nl_mempool_destroy(nl_memory_pool_t* pool) {
    if (!pool) return;
    
    pthread_mutex_destroy(&pool->mutex);
    
    if (pool->memory) {
        free(pool->memory);
    }
    
    free(pool);
}

void* nl_mempool_alloc(nl_memory_pool_t* pool) {
    if (!pool) return NULL;
    
    pthread_mutex_lock(&pool->mutex);
    
    struct nl_memory_block* block = pool->free_list;
    if (block) {
        pool->free_list = block->next;
        block->in_use = 1;
    }
    
    pthread_mutex_unlock(&pool->mutex);
    
    return block ? block->data : NULL;
}

void nl_mempool_free(nl_memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;
    
    pthread_mutex_lock(&pool->mutex);
    
    struct nl_memory_block* block = (struct nl_memory_block*)
        ((char*)ptr - sizeof(struct nl_memory_block));
    
    block->next = pool->free_list;
    block->in_use = 0;
    pool->free_list = block;
    
    pthread_mutex_unlock(&pool->mutex);
}

nl_ring_buffer_t* nl_ringbuf_create(size_t capacity) {
    nl_ring_buffer_t* buf = calloc(1, sizeof(nl_ring_buffer_t));
    if (!buf) return NULL;
    
    capacity = (capacity + CACHE_LINE_SIZE - 1) & ~(CACHE_LINE_SIZE - 1);
    
    void* aligned = NULL;
    if (posix_memalign(&aligned, CACHE_LINE_SIZE, capacity) != 0) {
        free(buf);
        return NULL;
    }
    buf->buffer = (char*)aligned;
    
    buf->capacity = capacity;
    pthread_mutex_init(&buf->mutex, NULL);
    
    return buf;
}

void nl_ringbuf_destroy(nl_ring_buffer_t* buf) {
    if (!buf) return;
    
    pthread_mutex_destroy(&buf->mutex);
    if (buf->buffer) free(buf->buffer);
    free(buf);
}

static size_t ringbuf_distance(size_t a, size_t b, size_t capacity) {
    return (b >= a) ? (b - a) : (capacity - a + b);
}

size_t nl_ringbuf_write(nl_ring_buffer_t* buf, const void* data, size_t len) {
    if (!buf || !data || len == 0) return 0;
    
    pthread_mutex_lock(&buf->mutex);
    
    size_t available = ringbuf_distance(buf->read_pos, buf->write_pos, buf->capacity);
    if (len > buf->capacity - available) {
        len = buf->capacity - available;
    }
    
    if (len == 0) {
        pthread_mutex_unlock(&buf->mutex);
        return 0;
    }
    
    size_t write_pos = buf->write_pos;
    size_t first_chunk = buf->capacity - write_pos;
    
    if (first_chunk >= len) {
        memcpy(buf->buffer + write_pos, data, len);
        buf->write_pos = (write_pos + len) % buf->capacity;
    } else {
        memcpy(buf->buffer + write_pos, data, first_chunk);
        memcpy(buf->buffer, (const char*)data + first_chunk, len - first_chunk);
        buf->write_pos = len - first_chunk;
    }
    
    pthread_mutex_unlock(&buf->mutex);
    return len;
}

size_t nl_ringbuf_read(nl_ring_buffer_t* buf, void* data, size_t len) {
    if (!buf || !data || len == 0) return 0;
    
    pthread_mutex_lock(&buf->mutex);
    
    size_t available = ringbuf_distance(buf->read_pos, buf->write_pos, buf->capacity);
    if (len > available) len = available;
    
    if (len == 0) {
        pthread_mutex_unlock(&buf->mutex);
        return 0;
    }
    
    size_t read_pos = buf->read_pos;
    size_t first_chunk = buf->capacity - read_pos;
    
    if (first_chunk >= len) {
        memcpy(data, buf->buffer + read_pos, len);
        buf->read_pos = (read_pos + len) % buf->capacity;
    } else {
        memcpy(data, buf->buffer + read_pos, first_chunk);
        memcpy((char*)data + first_chunk, buf->buffer, len - first_chunk);
        buf->read_pos = len - first_chunk;
    }
    
    pthread_mutex_unlock(&buf->mutex);
    return len;
}

size_t nl_ringbuf_available(nl_ring_buffer_t* buf) {
    if (!buf) return 0;
    
    pthread_mutex_lock(&buf->mutex);
    size_t available = ringbuf_distance(buf->read_pos, buf->write_pos, buf->capacity);
    pthread_mutex_unlock(&buf->mutex);
    
    return available;
}

size_t nl_ringbuf_free_space(nl_ring_buffer_t* buf) {
    if (!buf) return 0;
    
    pthread_mutex_lock(&buf->mutex);
    size_t free = buf->capacity - ringbuf_distance(buf->read_pos, buf->write_pos, buf->capacity) - 1;
    pthread_mutex_unlock(&buf->mutex);
    
    return free;
}

void nl_ringbuf_clear(nl_ring_buffer_t* buf) {
    if (!buf) return;
    
    pthread_mutex_lock(&buf->mutex);
    buf->read_pos = 0;
    buf->write_pos = 0;
    pthread_mutex_unlock(&buf->mutex);
}

nl_slice_t* nl_slice_create(const void* data, size_t len) {
    nl_slice_t* slice = calloc(1, sizeof(nl_slice_t));
    if (!slice) return NULL;
    
    slice->data = data;
    slice->length = len;
    
    return slice;
}

void nl_slice_destroy(nl_slice_t* slice) {
    free(slice);
}

const void* nl_slice_data(const nl_slice_t* slice) {
    return slice ? slice->data : NULL;
}

size_t nl_slice_length(const nl_slice_t* slice) {
    return slice ? slice->length : 0;
}

nl_slice_t* nl_slice_sub(const nl_slice_t* slice, size_t offset, size_t len) {
    if (!slice || offset >= slice->length) return NULL;
    
    if (offset + len > slice->length) {
        len = slice->length - offset;
    }
    
    nl_slice_t* sub = calloc(1, sizeof(nl_slice_t));
    if (!sub) return NULL;
    
    sub->data = (const char*)slice->data + offset;
    sub->length = len;
    
    return sub;
}

int nl_zero_copy_send(int fd, const void* buf, size_t len) {
#ifdef __linux__
    (void)buf;
    return sendfile(fd, -1, NULL, len);
#else
    return send(fd, buf, len, 0);
#endif
}

int nl_zero_copy_send_file(int sock_fd, int file_fd, off_t offset, size_t len) {
#ifdef __linux__
    off_t pos = offset;
    ssize_t sent = sendfile(sock_fd, file_fd, &pos, len);
    if (sent == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        return -1;
    }
    return (int)sent;
#else
    (void)offset;
    return sendfile(sock_fd, file_fd, &len);
#endif
}

int nl_file_get_size(const char* path, size_t* size) {
    struct stat st;
    if (stat(path, &st) == -1) return -1;
    *size = (size_t)st.st_size;
    return 0;
}

int nl_file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

int nl_file_read(const char* path, void* buf, size_t len, size_t* bytes_read) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return -1;
    
    ssize_t r = read(fd, buf, len);
    if (r == -1) {
        close(fd);
        return -1;
    }
    
    if (bytes_read) *bytes_read = (size_t)r;
    close(fd);
    return 0;
}

int nl_file_write(const char* path, const void* buf, size_t len, int append) {
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int fd = open(path, flags, 0644);
    if (fd == -1) return -1;
    
    ssize_t w = write(fd, buf, len);
    int result = (w == (ssize_t)len) ? 0 : -1;
    close(fd);
    return result;
}

int nl_file_delete(const char* path) {
    return unlink(path) == 0 ? 0 : -1;
}

int nl_file_append(const char* path, const void* buf, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd == -1) return -1;
    
    ssize_t w = write(fd, buf, len);
    int result = (w == (ssize_t)len) ? 0 : -1;
    close(fd);
    return result;
}

int nl_file_copy(const char* src_path, const char* dest_path, size_t* bytes_copied) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd == -1) return -1;
    
    int dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dest_fd == -1) {
        close(src_fd);
        return -1;
    }
    
    struct stat st;
    if (fstat(src_fd, &st) == -1) {
        close(src_fd);
        close(dest_fd);
        return -1;
    }
    
    off_t total = st.st_size;
    off_t pos = 0;
    char buffer[8192];
    size_t copied = 0;
    
    while (pos < total) {
        size_t to_read = (size_t)(total - pos);
        if (to_read > sizeof(buffer)) to_read = sizeof(buffer);
        
        ssize_t r = read(src_fd, buffer, to_read);
        if (r <= 0) {
            break;
        }
        
        if (write(dest_fd, buffer, (size_t)r) != r) {
            break;
        }
        
        pos += r;
        copied += (size_t)r;
    }
    
    close(src_fd);
    close(dest_fd);
    
    if (bytes_copied) *bytes_copied = copied;
    return (pos == total) ? 0 : -1;
}

int nl_file_move(const char* src_path, const char* dest_path) {
    if (rename(src_path, dest_path) == 0) return 0;
    
    int result = nl_file_copy(src_path, dest_path, NULL);
    if (result == 0) {
        unlink(src_path);
        return 0;
    }
    return -1;
}

int nl_file_send(int sock_fd, const char* file_path, off_t offset, size_t len, 
                 void (*progress)(size_t, size_t, void*), void* user_data) {
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd == -1) return -1;
    
    struct stat st;
    if (fstat(file_fd, &st) == -1) {
        close(file_fd);
        return -1;
    }
    
    off_t file_size = st.st_size;
    if (offset >= file_size) {
        close(file_fd);
        return 0;
    }
    
    if (len == 0 || (off_t)(offset + len) > file_size) {
        len = (size_t)(file_size - offset);
    }
    
    off_t pos = offset;
    size_t remaining = len;
    size_t sent_total = 0;
    
    while (remaining > 0) {
        size_t chunk_size = remaining;
        if (chunk_size > 65536) chunk_size = 65536;
        
        int sent = nl_zero_copy_send_file(sock_fd, file_fd, pos, chunk_size);
        if (sent == -2) {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 1000000; /* 1ms */
            nanosleep(&ts, NULL);
            continue;
        } else if (sent == -1) {
            break;
        }
        
        pos += sent;
        remaining -= sent;
        sent_total += sent;
        
        if (progress) {
            progress(sent_total, len, user_data);
        }
    }
    
    close(file_fd);
    return (remaining == 0) ? 0 : -1;
}

int nl_file_recv(int sock_fd, const char* file_path, size_t max_len, size_t* received) {
    int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return -1;
    
    char buffer[8192];
    size_t total = 0;
    
    while (total < max_len) {
        ssize_t r = recv(sock_fd, buffer, sizeof(buffer), 0);
        if (r <= 0) break;
        
        if (write(fd, buffer, (size_t)r) != r) {
            break;
        }
        
        total += (size_t)r;
    }
    
    close(fd);
    if (received) *received = total;
    return (total > 0) ? 0 : -1;
}

void* nl_zero_copy_map_file(const char* path, size_t* size) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) return NULL;
    
    struct stat st;
    if (fstat(fd, &st) == -1) {
        close(fd);
        return NULL;
    }
    
    *size = st.st_size;
    void* addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    
    if (addr == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    
    close(fd);
    return addr;
}

void nl_zero_copy_unmap(void* addr, size_t size) {
    if (addr) munmap(addr, size);
}

void nl_optimize_enable(void) {
    optimization_enabled = 1;
}

void nl_optimize_disable(void) {
    optimization_enabled = 0;
}

int nl_optimize_is_enabled(void) {
    return optimization_enabled;
}

/* IoT Optimization Configuration Implementation */
static nl_iot_optimization_level_t current_iot_level = NL_IOT_OPT_STANDARD;
static nl_iot_config_t current_iot_config = {
    1, 1, 1, 1, 1, 1, 1,
    256 * 1024, 4096, 65536, 100,
    0, 0
};

void nl_iot_set_optimization_level(nl_iot_optimization_level_t level) {
    current_iot_level = level;
    
    switch (level) {
        case NL_IOT_OPT_MINIMUM:
            nl_iot_apply_minimum_config(&current_iot_config);
            break;
        case NL_IOT_OPT_BASIC:
            nl_iot_apply_basic_config(&current_iot_config);
            break;
        case NL_IOT_OPT_BALANCED:
            nl_iot_apply_balanced_config(&current_iot_config);
            break;
        case NL_IOT_OPT_STANDARD:
        default:
            nl_iot_apply_standard_config(&current_iot_config);
            break;
    }
}

nl_iot_optimization_level_t nl_iot_get_optimization_level(void) {
    return current_iot_level;
}

void nl_iot_set_config(const nl_iot_config_t* config) {
    if (config) {
        memcpy(&current_iot_config, config, sizeof(nl_iot_config_t));
    }
}

void nl_iot_get_config(nl_iot_config_t* config) {
    if (config) {
        memcpy(config, &current_iot_config, sizeof(nl_iot_config_t));
    }
}

void nl_iot_apply_minimum_config(nl_iot_config_t* config) {
    if (!config) return;
    
    /* 极小设备模式：只保留基本 TCP 功能，禁用所有高级功能 */
    config->enable_memory_pool = 0;
    config->enable_ring_buffer = 0;
    config->enable_zero_copy = 0;
    config->enable_http_server = 0;
    config->enable_websocket = 0;
    config->enable_file_operations = 0;
    config->enable_udp_support = 0;
    
    config->default_mempool_size = 0;
    config->default_ringbuf_size = 0;
    config->max_buffer_size = 1024;
    config->max_connections = 2;
    
    config->enable_aggressive_gc = 1;
    config->reduce_thread_priority = 1;
}

void nl_iot_apply_basic_config(nl_iot_config_t* config) {
    if (!config) return;
    
    /* 基础 IoT 设备模式：基本功能，较小内存占用 */
    config->enable_memory_pool = 1;
    config->enable_ring_buffer = 1;
    config->enable_zero_copy = 0;
    config->enable_http_server = 0;
    config->enable_websocket = 0;
    config->enable_file_operations = 1;
    config->enable_udp_support = 1;
    
    config->default_mempool_size = 16 * 1024;
    config->default_ringbuf_size = 512;
    config->max_buffer_size = 4096;
    config->max_connections = 5;
    
    config->enable_aggressive_gc = 1;
    config->reduce_thread_priority = 0;
}

void nl_iot_apply_balanced_config(nl_iot_config_t* config) {
    if (!config) return;
    
    /* 平衡模式：在内存和性能间取得平衡 */
    config->enable_memory_pool = 1;
    config->enable_ring_buffer = 1;
    config->enable_zero_copy = 1;
    config->enable_http_server = 1;
    config->enable_websocket = 0;
    config->enable_file_operations = 1;
    config->enable_udp_support = 1;
    
    config->default_mempool_size = 64 * 1024;
    config->default_ringbuf_size = 2048;
    config->max_buffer_size = 32768;
    config->max_connections = 20;
    
    config->enable_aggressive_gc = 0;
    config->reduce_thread_priority = 0;
}

void nl_iot_apply_standard_config(nl_iot_config_t* config) {
    if (!config) return;
    
    /* 标准模式：完整功能，适合性能较好的设备 */
    config->enable_memory_pool = 1;
    config->enable_ring_buffer = 1;
    config->enable_zero_copy = 1;
    config->enable_http_server = 1;
    config->enable_websocket = 1;
    config->enable_file_operations = 1;
    config->enable_udp_support = 1;
    
    config->default_mempool_size = 256 * 1024;
    config->default_ringbuf_size = 4096;
    config->max_buffer_size = 65536;
    config->max_connections = 100;
    
    config->enable_aggressive_gc = 0;
    config->reduce_thread_priority = 0;
}

int nl_iot_is_feature_enabled(int feature_flag) {
    switch (feature_flag) {
        case NL_IOT_FEATURE_MEMPOOL:
            return current_iot_config.enable_memory_pool;
        case NL_IOT_FEATURE_RINGBUF:
            return current_iot_config.enable_ring_buffer;
        case NL_IOT_FEATURE_ZEROCOPY:
            return current_iot_config.enable_zero_copy;
        case NL_IOT_FEATURE_HTTP:
            return current_iot_config.enable_http_server;
        case NL_IOT_FEATURE_WEBSOCKET:
            return current_iot_config.enable_websocket;
        case NL_IOT_FEATURE_FILE:
            return current_iot_config.enable_file_operations;
        case NL_IOT_FEATURE_UDP:
            return current_iot_config.enable_udp_support;
        default:
            return 0;
    }
}

