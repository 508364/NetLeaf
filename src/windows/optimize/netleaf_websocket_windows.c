#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>

#include "netleaf_websocket.h"

#define MAX_CLIENTS 256
#define BUFFER_SIZE 16384

#pragma comment(lib, "ws2_32.lib")

typedef struct ws_client {
    SOCKET fd;
    int active;
    CRITICAL_SECTION mutex;
    struct ws_client* next;
} ws_client_t;

struct nl_websocket_server {
    SOCKET fd;
    int port;
    int running;
    HANDLE thread;
    ws_client_t* clients;
    CRITICAL_SECTION clients_mutex;
    
    nl_ws_connect_handler on_connect;
    nl_ws_message_handler on_message;
    nl_ws_close_handler on_close;
    void* user_data;
};

static void simple_sha1(const char* input, size_t len, unsigned char* output) {
    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;

    size_t original_len = len;
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    unsigned char* padded = calloc(padded_len, 1);
    if (!padded) return;

    memcpy(padded, input, len);
    padded[len] = 0x80;
    *(uint64_t*)(padded + padded_len - 8) = original_len * 8;

    for (size_t i = 0; i < padded_len; i += 64) {
        uint32_t w[80];
        for (int j = 0; j < 16; j++) {
            w[j] = (padded[i + j * 4] << 24) | (padded[i + j * 4 + 1] << 16) |
                   (padded[i + j * 4 + 2] << 8) | padded[i + j * 4 + 3];
        }
        for (int j = 16; j < 80; j++) {
            w[j] = ((w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16]) << 1) | ((w[j-3] ^ w[j-8] ^ w[j-14] ^ w[j-16]) >> 31);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int j = 0; j < 80; j++) {
            uint32_t f, k;
            if (j < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (j < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (j < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }

            uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[j];
            e = d; d = c; c = ((b << 30) | (b >> 2)); b = a; a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    free(padded);

    for (int i = 0; i < 4; i++) {
        output[i] = (h0 >> (24 - i * 8)) & 0xFF;
        output[i + 4] = (h1 >> (24 - i * 8)) & 0xFF;
        output[i + 8] = (h2 >> (24 - i * 8)) & 0xFF;
        output[i + 12] = (h3 >> (24 - i * 8)) & 0xFF;
        output[i + 16] = (h4 >> (24 - i * 8)) & 0xFF;
    }
}

static void base64_encode(const char* input, size_t len, char* output) {
    static const char* base64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    size_t i = 0;
    int padding = 0;
    
    while (i < len) {
        uint32_t n = ((uint32_t)(unsigned char)input[i]) << 16;
        if (i + 1 < len) n |= ((uint32_t)(unsigned char)input[i + 1]) << 8;
        else padding++;
        if (i + 2 < len) n |= ((uint32_t)(unsigned char)input[i + 2]);
        else padding++;
        
        output[0] = base64_table[(n >> 18) & 0x3F];
        output[1] = base64_table[(n >> 12) & 0x3F];
        output[2] = padding >= 2 ? '=' : base64_table[(n >> 6) & 0x3F];
        output[3] = padding >= 1 ? '=' : base64_table[n & 0x3F];
        
        output += 4;
        i += 3;
    }
    *output = '\0';
}

static int websocket_handshake(SOCKET client_fd) {
    char buffer[BUFFER_SIZE];
    int bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
    if (bytes <= 0) return -1;
    buffer[bytes] = '\0';
    
    char* upgrade = strstr(buffer, "Upgrade:");
    if (!upgrade) return -1;
    
    char* ws_key = strstr(buffer, "Sec-WebSocket-Key:");
    if (!ws_key) return -1;
    
    ws_key += 17;
    while (*ws_key == ' ') ws_key++;
    
    char key[32];
    strncpy(key, ws_key, 24);
    key[24] = '\0';
    
    char* end = strchr(key, '\r');
    if (end) *end = '\0';
    
    const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char combined[128];
    snprintf(combined, sizeof(combined), "%s%s", key, magic);
    
    unsigned char hash[20];
    simple_sha1(combined, strlen(combined), hash);
    
    char accept_key[64];
    base64_encode((const char*)hash, 20, accept_key);
    
    char response[512];
    snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n",
        accept_key);
    
    send(client_fd, response, (int)strlen(response), 0);
    return 0;
}

static int parse_ws_frame(const char* data, size_t len, char* out_data, size_t* out_len, nl_ws_opcode_t* out_opcode) {
    if (len < 2) return -1;
    
    *out_opcode = data[0] & 0x0F;
    size_t payload_len = data[1] & 0x7F;
    size_t offset = 2;
    
    if (payload_len == 126) {
        if (len < 4) return -1;
        payload_len = ((size_t)(unsigned char)data[2] << 8) | (unsigned char)data[3];
        offset = 4;
    } else if (payload_len == 127) {
        if (len < 10) return -1;
        payload_len = 0;
        for (int i = 0; i < 8; i++) {
            payload_len = (payload_len << 8) | (unsigned char)data[2 + i];
        }
        offset = 10;
    }
    
    if (len < offset + payload_len) return -1;
    
    unsigned char mask[4];
    memcpy(mask, data + offset, 4);
    offset += 4;
    
    for (size_t i = 0; i < payload_len && i < BUFFER_SIZE - 1; i++) {
        out_data[i] = data[offset + i] ^ mask[i % 4];
    }
    out_data[payload_len] = '\0';
    *out_len = payload_len;
    
    return 0;
}

static void build_ws_frame(const char* data, size_t len, nl_ws_opcode_t opcode, char* out_frame, size_t* out_len) {
    out_frame[0] = 0x80 | (opcode & 0x0F);
    
    if (len < 126) {
        out_frame[1] = (unsigned char)len;
        memcpy(out_frame + 2, data, len);
        *out_len = len + 2;
    } else if (len < 65536) {
        out_frame[1] = 126;
        out_frame[2] = (len >> 8) & 0xFF;
        out_frame[3] = len & 0xFF;
        memcpy(out_frame + 4, data, len);
        *out_len = len + 4;
    } else {
        out_frame[1] = 127;
        for (int i = 7; i >= 0; i--) {
            out_frame[2 + i] = len & 0xFF;
            len >>= 8;
        }
        memcpy(out_frame + 10, data, len);
        *out_len = len + 10;
    }
}

typedef struct ws_thread_arg {
    nl_websocket_server_t* server;
    SOCKET client_fd;
} ws_thread_arg_t;

static DWORD WINAPI client_thread(LPVOID arg) {
    ws_thread_arg_t* targ = (ws_thread_arg_t*)arg;
    nl_websocket_server_t* server = targ->server;
    SOCKET client_fd = targ->client_fd;
    free(targ);
    
    if (websocket_handshake(client_fd) != 0) {
        closesocket(client_fd);
        if (server->on_close) {
            server->on_close(server->user_data);
        }
        return 0;
    }
    
    char buffer[BUFFER_SIZE];
    
    while (1) {
        int bytes = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) break;
        
        char data[BUFFER_SIZE];
        size_t data_len;
        nl_ws_opcode_t opcode;
        
        if (parse_ws_frame(buffer, bytes, data, &data_len, &opcode) == 0) {
            if (opcode == NL_WS_CLOSE) {
                char close_frame[2] = {0x88, 0x00};
                send(client_fd, (char*)close_frame, 2, 0);
                break;
            } else if (opcode == NL_WS_PING) {
                char pong_frame[10];
                size_t pong_len;
                build_ws_frame("", 0, NL_WS_PONG, pong_frame, &pong_len);
                send(client_fd, pong_frame, (int)pong_len, 0);
            } else if (server->on_message && (opcode == NL_WS_TEXT || opcode == NL_WS_BINARY)) {
                server->on_message(data, data_len, opcode, server->user_data);
            }
        }
    }
    
    closesocket(client_fd);
    if (server->on_close) {
        server->on_close(server->user_data);
    }
    return 0;
}

static DWORD WINAPI server_thread(LPVOID arg) {
    nl_websocket_server_t* server = (nl_websocket_server_t*)arg;
    
    while (server->running) {
        struct sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        
        SOCKET client_fd = accept(server->fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd != INVALID_SOCKET) {
            ws_thread_arg_t* targ = malloc(sizeof(ws_thread_arg_t));
            targ->server = server;
            targ->client_fd = client_fd;
            
            HANDLE thread = CreateThread(NULL, 0, client_thread, targ, 0, NULL);
            CloseHandle(thread);
            
            if (server->on_connect) {
                server->on_connect(server->user_data);
            }
        }
    }
    
    return 0;
}

nl_websocket_server_t* nl_ws_server_create(int port) {
    nl_websocket_server_t* server = calloc(1, sizeof(nl_websocket_server_t));
    if (!server) return NULL;
    
    server->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server->fd == INVALID_SOCKET) {
        free(server);
        return NULL;
    }
    
    BOOL opt = TRUE;
    setsockopt(server->fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server->fd, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(server->fd);
        free(server);
        return NULL;
    }
    
    if (listen(server->fd, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server->fd);
        free(server);
        return NULL;
    }
    
    InitializeCriticalSection(&server->clients_mutex);
    server->port = port;
    return server;
}

void nl_ws_server_destroy(nl_websocket_server_t* server) {
    if (!server) return;
    
    if (server->fd != INVALID_SOCKET) closesocket(server->fd);
    DeleteCriticalSection(&server->clients_mutex);
    free(server);
}

int nl_ws_server_start(nl_websocket_server_t* server) {
    if (!server || server->running) return -1;
    
    server->running = 1;
    server->thread = CreateThread(NULL, 0, server_thread, server, 0, NULL);
    return server->thread ? 0 : -1;
}

void nl_ws_server_stop(nl_websocket_server_t* server) {
    if (!server || !server->running) return;
    
    server->running = 0;
    closesocket(server->fd);
    if (server->thread) {
        WaitForSingleObject(server->thread, INFINITE);
        CloseHandle(server->thread);
    }
}

void nl_ws_server_set_on_connect(nl_websocket_server_t* server, nl_ws_connect_handler handler, void* user_data) {
    if (!server) return;
    server->on_connect = handler;
    server->user_data = user_data;
}

void nl_ws_server_set_on_message(nl_websocket_server_t* server, nl_ws_message_handler handler, void* user_data) {
    if (!server) return;
    server->on_message = handler;
    server->user_data = user_data;
}

void nl_ws_server_set_on_close(nl_websocket_server_t* server, nl_ws_close_handler handler, void* user_data) {
    if (!server) return;
    server->on_close = handler;
    server->user_data = user_data;
}

int nl_ws_server_broadcast(nl_websocket_server_t* server, const char* data, size_t len, nl_ws_opcode_t opcode) {
    if (!server) return -1;
    
    EnterCriticalSection(&server->clients_mutex);
    ws_client_t* client = server->clients;
    int count = 0;
    
    while (client) {
        if (client->active) {
            char frame[BUFFER_SIZE * 2];
            size_t frame_len;
            build_ws_frame(data, len, opcode, frame, &frame_len);
            
            EnterCriticalSection(&client->mutex);
            send(client->fd, frame, (int)frame_len, 0);
            LeaveCriticalSection(&client->mutex);
            count++;
        }
        client = client->next;
    }
    
    LeaveCriticalSection(&server->clients_mutex);
    return count;
}

int nl_ws_server_send_text(nl_websocket_server_t* server, const char* data, size_t len) {
    return nl_ws_server_broadcast(server, data, len, NL_WS_TEXT);
}

int nl_ws_server_send_binary(nl_websocket_server_t* server, const void* data, size_t len) {
    return nl_ws_server_broadcast(server, (const char*)data, len, NL_WS_BINARY);
}

int nl_ws_server_send_ping(nl_websocket_server_t* server) {
    return nl_ws_server_broadcast(server, "", 0, NL_WS_PING);
}

int nl_ws_server_send_pong(nl_websocket_server_t* server, const char* data, size_t len) {
    return nl_ws_server_broadcast(server, data, len, NL_WS_PONG);
}
