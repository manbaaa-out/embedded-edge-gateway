#include <cstdio>
#include <mosquitto.h>

void on_message(struct mosquitto* mosq, void* obj, const struct mosquitto_message* msg) {
    printf("%s : %.*s\n", msg->topic, msg->payloadlen, (char*)msg->payload);
}

int main () {
    mosquitto_lib_init();
    struct mosquitto* mosq = mosquitto_new(NULL, true, NULL);
    if (!mosq) {
        fprintf(stderr, "mosquitto_new failed\n");
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_message_callback_set(mosq, on_message);
    int rc = mosquitto_connect(mosq, "localhost", 1883, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "connect failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(mosq);      // 注意:失败也要 destroy + cleanup
        mosquitto_lib_cleanup();
        return 1;
    }
    mosquitto_subscribe(mosq, NULL, "gateway/#", 1);
    mosquitto_loop_forever(mosq, -1, 1);
}