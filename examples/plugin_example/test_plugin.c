#include "netleaf.h"
#include "netleaf_module.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    printf("=== NetLeaf Plugin System Demo ===\n\n");
    
    nl_modules_init();
    
    printf("Loading example plugin...\n");
    nl_plugin_handle_t plugin = nl_plugin_load("./netleaf_example_plugin.dll");
    
    if (!plugin) {
        printf("Failed to load plugin: %s\n", nl_plugin_get_error());
        return 1;
    }
    
    printf("Plugin loaded successfully\n");
    
    nl_plugin_descriptor_t* desc = nl_plugin_get_descriptor(plugin);
    if (desc) {
        printf("\nPlugin Info:\n");
        printf("  Name: %s\n", desc->name);
        printf("  Version: %s\n", desc->version);
        printf("  Description: %s\n", desc->description);
        printf("  Author: %s\n", desc->author);
    }
    
    printf("\nRegistering plugin module...\n");
    nl_plugin_register(plugin);
    
    printf("\nListing all modules:\n");
    nl_print_modules();
    
    printf("Unloading plugin...\n");
    nl_plugin_unload(plugin);
    
    nl_modules_shutdown();
    
    printf("\n=== Demo completed ===\n");
    return 0;
}
