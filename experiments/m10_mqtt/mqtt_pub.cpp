#include <cstdio>
#include <mosquitto.h>
#include <cstring>

int main() {
    mosquitto_lib_init();
    struct mosquitto* mosq = mosquitto_new(NULL, true, NULL);
    if (!mosq) {
        fprintf(stderr, "mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return 1;
    }

    int rc = mosquitto_connect(mosq, "localhost", 1883, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "connect failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);      // 注意:失败也要 destroy + cleanup
        mosquitto_lib_cleanup();
        return 1;
    }

    const char* payload = "25.6";
    int rb = mosquitto_publish(mosq, NULL, "gateway/sensor/temp", static_cast<int>(strlen(payload)), payload, 0,false);
    if (rb != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "publish failed: %s\n", mosquitto_strerror(rb));
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);      // 注意:失败也要 destroy + cleanup
        mosquitto_lib_cleanup();
        return 1;
    }
    mosquitto_loop(mosq, -1, 1);

    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
}
