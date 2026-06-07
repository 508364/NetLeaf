#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "netleaf.h"

// API Route: GET /api/hello
void handle_hello(const char* path, nl_http_method_t method,
                  const char* body, size_t body_size,
                  char** response, size_t* response_size,
                  void* user_data) {
    const char* resp = "{\"message\":\"Hello, NetLeaf!\"}";
    *response_size = strlen(resp);
    *response = (char*)malloc(*response_size + 1);
    if (*response) {
        strcpy(*response, resp);
    }
}

// API Route: POST /api/echo
void handle_echo(const char* path, nl_http_method_t method,
                 const char* body, size_t body_size,
                 char** response, size_t* response_size,
                 void* user_data) {
    const char* prefix = "{\"echo\":\"";
    const char* suffix = "\"}";
    *response_size = strlen(prefix) + body_size + strlen(suffix);
    *response = (char*)malloc(*response_size + 1);
    if (*response) {
        sprintf(*response, "%s%.*s%s", prefix, (int)body_size, body, suffix);
    }
}

int main(int argc, char* argv[]) {
    printf("=== NetLeaf v2.0.0 - Advanced Server Demo ===\n\n");
    
    nl_router_t* router = nl_router_create();
    if (!router) {
        printf("Failed to create router\n");
        return 1;
    }
    
    // Add API routes
    nl_router_add_route(router, "/api/hello", NL_METHOD_GET, handle_hello, NULL);
    nl_router_add_route(router, "/api/echo", NL_METHOD_POST, handle_echo, NULL);
    
    // Set static files directory
    const char* static_dir = (argc > 1) ? argv[1] : ".";
    nl_router_set_static_dir(router, static_dir);
    
    printf("Starting server on port 8080\n");
    printf("Static files directory: %s\n", static_dir);
    printf("\nAvailable routes:\n");
    printf("  GET  /api/hello    - Hello API\n");
    printf("  POST /api/echo     - Echo API\n");
    printf("  GET  /*            - Static files\n");
    printf("\nServer running at http://localhost:8080\n");
    printf("Press Ctrl+C to stop...\n");
    
    nl_router_serve(router, 8080);
    
    // Note: In a real app, you'd want to add a way to stop the server
    // For this simple example, we just wait
    while (1) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
    }
    
    nl_router_destroy(router);
    return 0;
}
