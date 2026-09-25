#include "netleaf.h"
#include <stdio.h>

int main(int argc, char* argv[]) {
    printf("NetLeaf v%s - Basic Test\n", NL_MODULE_VERSION);
    
    // Test module availability
    const char* modules[16];
    int count = nl_get_modules(modules, 16);
    printf("Available modules: %d\n", count);
    for (int i = 0; i < count; i++) {
        printf("  - %s\n", modules[i]);
    }
    
    // Test MQTT module
    if (nl_module_available("mqtt")) {
        printf("MQTT module is available!\n");
    } else {
        printf("MQTT module not found\n");
    }
    
    printf("Test passed!\n");
    return 0;
}
