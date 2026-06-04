#include <cstdio>
#include <mosquitto.h>
#include <unistd.h>
#include <cstring>

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
    
    rc = mosquitto_subscribe(mosq, NULL, "gateway/cmd/#", 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "subscribe failed: %s\n", mosquitto_strerror(rc));
        mosquitto_disconnect(mosq);
        mosquitto_destroy(mosq);
        mosquitto_lib_cleanup();
        return 1;
    }

    mosquitto_loop_start(mosq);

    const char* payload = "25.6";
    for (int i = 0; i <10; i++) {
        int rb = mosquitto_publish(mosq, NULL, "gateway/sensor/temp", static_cast<int>(strlen(payload)), payload, 0,false);
        if (rb != MOSQ_ERR_SUCCESS) {
            fprintf(stderr, "publish failed: %s\n", mosquitto_strerror(rb));
            mosquitto_disconnect(mosq);
            mosquitto_destroy(mosq);      // 注意:失败也要 destroy + cleanup
            mosquitto_lib_cleanup();
            return 1;
        }
        sleep(1);
    }

    mosquitto_loop_stop(mosq, false);

    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
}