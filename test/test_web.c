#include <stdio.h>
#include "netleaf.h"

int main(void) {
    printf("Test 1: nl_web_create\n");
    nl_web_server_t* server = nl_web_create(18923);
    printf("  server: %p\n", (void*)server);

    if (server) {
        printf("Test 2: nl_web_stop\n");
        nl_web_stop(server);
        printf("  stop done\n");

        printf("Test 3: nl_web_destroy\n");
        nl_web_destroy(server);
        printf("  destroy done\n");
    } else {
        printf("  nl_web_create failed (port may be in use)\n");
    }

    printf("ALL DONE!\n");
    return 0;
}
