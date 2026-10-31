#include <stdio.h>
#include "netleaf.h"
#include "netleaf_lang.h"
#include "netleaf_autocomplete.h"
#include "netleaf_autoroute.h"
#include "netleaf_errorpage.h"
#include "netleaf_vue.h"
#include "netleaf_ipc.h"
#include "netleaf_linkagg.h"
#ifdef HAVE_NETLEAF_MQTT
#include "netleaf_mqtt.h"
#endif
#ifdef HAVE_NETLEAF_MQTT_SERVER
#include "netleaf_mqtt_server.h"
#endif

int main(void) {
    printf("Test 1: Core\n");
    nl_modules_init();
    printf("  version: %s\n", nl_version_string());

    printf("\nTest 2: Lang\n");
    nl_lang_set("zh_cn");
    printf("  lang: %s\n", nl_lang_get());

    printf("\nTest 3: Autocomplete\n");
    printf("  available: %d\n", nl_autocomplete_is_available());

    printf("\nTest 4: Autoroute\n");
    printf("  available: %d\n", nl_autoroute_is_available());

    printf("\nTest 5: ErrorPage\n");
    printf("  available: %d\n", nl_errorpage_is_available());

    printf("\nTest 6: Vue\n");
    printf("  available: %d\n", nl_vue_is_available());

    printf("\nTest 7: IPC\n");
    printf("  available: %d\n", nl_ipc_is_available());

    printf("\nTest 8: LinkAgg\n");
    printf("  available: %d\n", nl_lagg_is_available());

    printf("\nTest 9: JSON/TOML\n");
    const char* json_str = "{\"name\":\"NetLeaf\"}";
    nl_status_t err;
    void* json = nl_json_parse(json_str, &err, NULL, NULL);
    printf("  json parse: %p\n", json);
    if (json) nl_json_destroy(json);

    printf("\nTest 10: Encoding\n");
    const char* enc = nl_encoding_detect("hello", 5);
    printf("  detected: %s\n", enc ? enc : "(null)");

    printf("\nTest 11: System Info\n");
    const char* os = nl_sys_info_get_os_name();
    printf("  os: %s\n", os ? os : "(null)");

    printf("\nTest 12: Web Server\n");
    printf("  calling nl_web_create(18923)...\n");
    nl_web_server_t* server = nl_web_create(18923);
    printf("  server: %p\n", (void*)server);

    if (server) {
        printf("Test 13: Web Server Operations\n");
        nl_web_set_encoding(server, "UTF-8");
        nl_web_add_route(server, "/test", "<h1>Test</h1>", "text/html");
        int count = nl_web_get_route_count(server);
        printf("  routes: %d\n", count);
        nl_web_stop(server);
        nl_web_destroy(server);
        printf("  web server stopped and destroyed\n");
    } else {
        printf("  WARNING: nl_web_create failed!\n");
    }

    printf("\nTest 14: MQTT\n");
#ifdef HAVE_NETLEAF_MQTT
    printf("  available: %d\n", nl_mqtt_is_available());
    printf("  version: %s\n", nl_mqtt_version());
#else
    printf("  (MQTT module not built)\n");
#endif

    printf("\nTest 15: MQTT Server\n");
#ifdef HAVE_NETLEAF_MQTT_SERVER
    printf("  available: %d\n", nl_mqtt_server_is_available());
    printf("  version: %s\n", nl_mqtt_server_version());
#else
    printf("  (MQTT Server module not built)\n");
#endif

    printf("\nALL TESTS COMPLETED!\n");
    return 0;
}
