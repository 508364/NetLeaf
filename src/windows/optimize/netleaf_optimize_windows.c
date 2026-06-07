#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <mswsock.h>
#include <io.h>
#include <memory.h>
#include <stdlib.h>
#include <stdio.h>

#include "../../include/optimize/netleaf_optimize.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

#define ALIGN_SIZE 64
#define CACHE_LINE_SIZE 64

static volatile long optimization_enabled = 1;

typedef struct nl_memory_block {
    struct nl_memory_block* next;
    int in_use;
    char data[1];
} nl_memory_block_t;

typedef struct nl_memory_pool {
    size_t block_size;
    size_t pool_size;
    HANDLE heap;
    nl_memory_block_t* free_list;
    CRITICAL_SECTION mutex;
} nl_memory_pool_t;

typedef struct nl_ring_buffer {
    char* buffer;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    CRITICAL_SECTION mutex;
} nl_ring_buffer_t;

typedef struct nl_slice {
    const void* data;
    size_t length;
} nl_slice_t;

static size_t align_size(size_t size) {
    return (size + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);
}

static size_t ringbuf_distance(size_t a, size_t b, size_t capacity) {
    return (b >= a) ? (b - a) : (capacity - a + b);
}

nl_memory_pool_t* nl_mempool_create(size_t block_size, size_t pool_size) {
    if (block_size < sizeof(nl_memory_block_t)) {
        block_size = sizeof(nl_memory_block_t);
    }
    
    block_size = align_size(block_size);
    
    nl_memory_pool_t* pool = calloc(1, sizeof(nl_memory_pool_t));
    if (!pool) return NULL;
    
    pool->block_size = block_size;
    pool->pool_size = pool_size;
    
    pool->heap = HeapCreate(HEAP_GENERATE_EXCEPTIONS | HEAP_NO_SERIALIZE, 
                           block_size * pool_size, 0);
    if (!pool->heap) {
        free(pool);
        return NULL;
    }
    
    InitializeCriticalSection(&pool->mutex);
    
    for (size_t i = 0; i < pool_size; i++) {
        nl_memory_block_t* block = (nl_memory_block_t*)HeapAlloc(
            pool->heap, HEAP_ZERO_MEMORY, block_size);
        if (!block) {
            nl_mempool_destroy(pool);
            return NULL;
        }
        block->next = pool->free_list;
        block->in_use = 0;
        pool->free_list = block;
    }
    
    return pool;
}

void nl_mempool_destroy(nl_memory_pool_t* pool) {
    if (!pool) return;
    
    DeleteCriticalSection(&pool->mutex);
    
    if (pool->heap) {
        HeapDestroy(pool->heap);
    }
    
    free(pool);
}

void* nl_mempool_alloc(nl_memory_pool_t* pool) {
    if (!pool) return NULL;
    
    EnterCriticalSection(&pool->mutex);
    
    nl_memory_block_t* block = pool->free_list;
    if (block) {
        pool->free_list = block->next;
        block->in_use = 1;
    }
    
    LeaveCriticalSection(&pool->mutex);
    
    return block ? block->data : NULL;
}

void nl_mempool_free(nl_memory_pool_t* pool, void* ptr) {
    if (!pool || !ptr) return;
    
    EnterCriticalSection(&pool->mutex);
    
    nl_memory_block_t* block = (nl_memory_block_t*)((char*)ptr - 
        sizeof(nl_memory_block_t));
    
    block->next = pool->free_list;
    block->in_use = 0;
    pool->free_list = block;
    
    LeaveCriticalSection(&pool->mutex);
}

nl_ring_buffer_t* nl_ringbuf_create(size_t capacity) {
    nl_ring_buffer_t* buf = calloc(1, sizeof(nl_ring_buffer_t));
    if (!buf) return NULL;
    
    capacity = align_size(capacity);
    
    buf->buffer = (char*)VirtualAlloc(NULL, capacity, MEM_COMMIT | MEM_RESERVE, 
                                     PAGE_READWRITE);
    if (!buf->buffer) {
        free(buf);
        return NULL;
    }
    
    buf->capacity = capacity;
    InitializeCriticalSection(&buf->mutex);
    
    return buf;
}

void nl_ringbuf_destroy(nl_ring_buffer_t* buf) {
    if (!buf) return;
    
    DeleteCriticalSection(&buf->mutex);
    
    if (buf->buffer) {
        VirtualFree(buf->buffer, 0, MEM_RELEASE);
    }
    
    free(buf);
}

size_t nl_ringbuf_write(nl_ring_buffer_t* buf, const void* data, size_t len) {
    if (!buf || !data || len == 0) return 0;
    
    EnterCriticalSection(&buf->mutex);
    
    size_t available = ringbuf_distance(buf->read_pos, buf->write_pos, buf->capacity);
    if (len > buf->capacity - available) {
        len = buf->capacity - available;
    }
    
    if (len == 0) {
        LeaveCriticalSection(&buf->mutex);
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
    
    LeaveCriticalSection(&buf->mutex);
    return len;
}

size_t nl_ringbuf_read(nl_ring_buffer_t* buf, void* data, size_t len) {
    if (!buf || !data || len == 0) return 0;
    
    EnterCriticalSection(&buf->mutex);
    
    size_t available = ringbuf_distance(buf->read_pos, buf->write_pos, buf->capacity);
    if (len > available) len = available;
    
    if (len == 0) {
        LeaveCriticalSection(&buf->mutex);
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
    
    LeaveCriticalSection(&buf->mutex);
    return len;
}

size_t nl_ringbuf_available(nl_ring_buffer_t* buf) {
    if (!buf) return 0;
    
    EnterCriticalSection(&buf->mutex);
    size_t available = ringbuf_distance(buf->read_pos, buf->write_pos, buf->capacity);
    LeaveCriticalSection(&buf->mutex);
    
    return available;
}

size_t nl_ringbuf_free_space(nl_ring_buffer_t* buf) {
    if (!buf) return 0;
    
    EnterCriticalSection(&buf->mutex);
    size_t free = buf->capacity - ringbuf_distance(buf->read_pos, buf->write_pos, buf->capacity) - 1;
    LeaveCriticalSection(&buf->mutex);
    
    return free;
}

void nl_ringbuf_clear(nl_ring_buffer_t* buf) {
    if (!buf) return;
    
    EnterCriticalSection(&buf->mutex);
    buf->read_pos = 0;
    buf->write_pos = 0;
    LeaveCriticalSection(&buf->mutex);
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
    WSABUF wsabuf;
    wsabuf.buf = (char*)buf;
    wsabuf.len = (u_long)len;
    
    DWORD sent = 0;
    return WSASend(fd, &wsabuf, 1, &sent, 0, NULL, NULL) == 0 ? (int)sent : -1;
}

void* nl_zero_copy_map_file(const char* path, size_t* size) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file, &file_size)) {
        CloseHandle(file);
        return NULL;
    }
    
    *size = (size_t)file_size.QuadPart;
    
    HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        CloseHandle(file);
        return NULL;
    }
    
    void* addr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    
    CloseHandle(mapping);
    CloseHandle(file);
    
    return addr;
}

void nl_zero_copy_unmap(void* addr, size_t size) {
    if (addr) UnmapViewOfFile(addr);
}

void nl_optimize_enable(void) {
    InterlockedExchange(&optimization_enabled, 1);
}

void nl_optimize_disable(void) {
    InterlockedExchange(&optimization_enabled, 0);
}

int nl_optimize_is_enabled(void) {
    return optimization_enabled;
}

int nl_zero_copy_send_file(int sock_fd, int file_fd, off_t offset, size_t len) {
    // Windows: Use TransmitFile for optimized file transfer
    HANDLE hFile = (HANDLE)_get_osfhandle(file_fd);
    if (hFile == INVALID_HANDLE_VALUE) {
        return -1;
    }

    DWORD bytesToTransmit = (DWORD)len;
    if (TransmitFile((SOCKET)sock_fd, hFile, bytesToTransmit, 0, NULL, NULL, 0)) {
        return (int)bytesToTransmit;
    }
    
    if (WSAGetLastError() == WSAEWOULDBLOCK) {
        return -2;
    }
    return -1;
}

int nl_file_get_size(const char* path, size_t* size) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) {
        return -1;
    }
    /* Safely combine 64-bit file size */
#if defined(_WIN64) || defined(__LP64__)
    /* 64-bit platform: use full 64 bits */
    *size = ((size_t)fad.nFileSizeHigh << 32) | (size_t)fad.nFileSizeLow;
#else
    /* 32-bit platform: only use low 32 bits, check if high bits are zero */
    if (fad.nFileSizeHigh != 0) {
        return -1; /* File too large for 32-bit platform */
    }
    *size = (size_t)fad.nFileSizeLow;
#endif
    return 0;
}

int nl_file_exists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

int nl_file_read(const char* path, void* buf, size_t len, size_t* bytes_read) {
    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return -1;

    DWORD read = 0;
    BOOL result = ReadFile(hFile, buf, (DWORD)len, &read, NULL);
    CloseHandle(hFile);

    if (!result) return -1;
    if (bytes_read) *bytes_read = (size_t)read;
    return 0;
}

int nl_file_write(const char* path, const void* buf, size_t len, int append) {
    DWORD creation = append ? OPEN_ALWAYS : CREATE_ALWAYS;
    DWORD flags = append ? FILE_FLAG_WRITE_THROUGH : FILE_ATTRIBUTE_NORMAL;
    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, creation, flags, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return -1;

    if (append) {
        SetFilePointer(hFile, 0, NULL, FILE_END);
    }

    DWORD written = 0;
    BOOL result = WriteFile(hFile, buf, (DWORD)len, &written, NULL);
    CloseHandle(hFile);
    return (result && written == len) ? 0 : -1;
}

int nl_file_delete(const char* path) {
    return DeleteFileA(path) ? 0 : -1;
}

int nl_file_append(const char* path, const void* buf, size_t len) {
    return nl_file_write(path, buf, len, 1);
}

int nl_file_copy(const char* src_path, const char* dest_path, size_t* bytes_copied) {
    if (!CopyFileA(src_path, dest_path, FALSE)) {
        return -1;
    }
    
    size_t size;
    if (nl_file_get_size(src_path, &size) == 0 && bytes_copied) {
        *bytes_copied = size;
    }
    
    return 0;
}

int nl_file_move(const char* src_path, const char* dest_path) {
    if (MoveFileA(src_path, dest_path)) return 0;
    
    if (CopyFileA(src_path, dest_path, FALSE)) {
        DeleteFileA(src_path);
        return 0;
    }
    return -1;
}

int nl_file_send(int sock_fd, const char* file_path, off_t offset, size_t len, 
                 void (*progress)(size_t, size_t, void*), void* user_data) {
    HANDLE hFile = CreateFileA(file_path, GENERIC_READ, FILE_SHARE_READ, NULL, 
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return -1;

    LARGE_INTEGER fileSize;
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return -1;
    }

    if (offset >= fileSize.QuadPart) {
        CloseHandle(hFile);
        return 0;
    }

    if (len == 0 || (size_t)(offset + len) > (size_t)fileSize.QuadPart) {
        len = (size_t)(fileSize.QuadPart - offset);
    }

    LARGE_INTEGER li;
    li.QuadPart = offset;
    SetFilePointerEx(hFile, li, NULL, FILE_BEGIN);

    char buffer[8192];
    size_t sent_total = 0;
    size_t remaining = len;

    while (remaining > 0) {
        DWORD toRead = (DWORD)min(remaining, sizeof(buffer));
        DWORD read = 0;

        if (!ReadFile(hFile, buffer, toRead, &read, NULL) || read == 0) {
            break;
        }

        int sent = send((SOCKET)sock_fd, buffer, read, 0);
        if (sent == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                Sleep(1);
                continue;
            }
            break;
        }

        sent_total += (size_t)sent;
        remaining -= (size_t)sent;

        if (progress) {
            progress(sent_total, len, user_data);
        }
    }

    CloseHandle(hFile);
    return (remaining == 0) ? 0 : -1;
}

int nl_file_recv(int sock_fd, const char* file_path, size_t max_len, size_t* received) {
    HANDLE hFile = CreateFileA(file_path, GENERIC_WRITE, 0, NULL, 
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return -1;

    char buffer[8192];
    size_t total = 0;

    while (total < max_len) {
        int r = recv((SOCKET)sock_fd, buffer, sizeof(buffer), 0);
        if (r <= 0) break;

        DWORD written = 0;
        if (!WriteFile(hFile, buffer, (DWORD)r, &written, NULL)) {
            break;
        }

        total += (size_t)r;
    }

    CloseHandle(hFile);
    if (received) *received = total;
    return (total > 0) ? 0 : -1;
}

/* IoT Optimization Configuration Implementation for Windows */
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

