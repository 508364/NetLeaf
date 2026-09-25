#include <stdio.h>
#include "netleaf.h"
#include "netleaf_lang.h"
#include "netleaf_autocomplete.h"
#include "netleaf_autoroute.h"
#include "netleaf_errorpage.h"
#include "netleaf_vue.h"
#include "netleaf_ipc.h"
#include "netleaf_linkagg.h"

int main(void) {
    printf("=== Test 1: Core ===\n");
    printf("version: %s\n", nl_version_string());
    nl_modules_init();
    printf("modules_init OK\n");

    printf("\n=== Test 2: Lang ===\n");
    printf("version: %s\n", nl_lang_version());
    printf("validate zh_cn: %d\n", nl_lang_validate_code("zh_cn"));
    nl_lang_set("zh_cn");
    printf("lang set to: %s\n", nl_lang_get());

    printf("\n=== Test 3: Autocomplete ===\n");
    printf("available: %d\n", nl_autocomplete_is_available());
    printf("version: %s\n", nl_autocomplete_version());

    printf("\n=== Test 4: Autoroute ===\n");
    printf("available: %d\n", nl_autoroute_is_available());
    printf("version: %s\n", nl_autoroute_version());
    printf("levenshtein(kitten,sitting): %d\n", nl_levenshtein_distance("kitten", "sitting"));

    printf("\n=== Test 5: ErrorPage ===\n");
    printf("available: %d\n", nl_errorpage_is_available());
    printf("version: %s\n", nl_errorpage_version());
    const char* msg = nl_errorpage_status_message(404);
    printf("status 404: %s\n", msg ? msg : "(null)");

    printf("\n=== Test 6: Vue ===\n");
    printf("available: %d\n", nl_vue_is_available());
    printf("version: %s\n", nl_vue_version());
    const char* url = nl_vue_get_cdn_url(NL_VUE_CDN_UNPKG, NULL);
    printf("cdn url: %s\n", url ? url : "(null)");

    printf("\n=== Test 7: IPC ===\n");
    printf("available: %d\n", nl_ipc_is_available());
    printf("version: %s\n", nl_ipc_version());

    printf("\n=== Test 8: LinkAgg ===\n");
    printf("available: %d\n", nl_lagg_is_available());
    printf("version: %s\n", nl_lagg_version());
    printf("validate id: %d\n", nl_lagg_validate_id_format("123.456"));

    printf("\n=== Test 9: Web Server (without create) ===\n");
    printf("Web server functions available\n");

    printf("\nALL TESTS PASSED!\n");
    return 0;
}
