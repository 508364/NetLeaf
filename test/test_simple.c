#include <stdio.h>
#include "netleaf.h"

int main(void) {
    printf("Test 1: version_string\n");
    const char* ver = nl_version_string();
    printf("  version: %s\n", ver ? ver : "(null)");

    printf("Test 2: modules_init\n");
    int rc = nl_modules_init();
    printf("  init result: %d\n", rc);

    printf("Test 3: module count\n");
    int count = nl_get_module_count();
    printf("  count: %d\n", count);

    printf("Test 4: print modules\n");
    nl_print_modules();

    printf("ALL DONE!\n");
    return 0;
}
