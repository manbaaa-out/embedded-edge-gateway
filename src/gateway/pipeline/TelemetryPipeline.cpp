#include "gateway/pipeline/TelemetryPipeline.h"

#include <string>

namespace gateway {

void TelemetryPipeline::submit(const std::vector<Reading>& readings,
                               std::shared_ptr<Database>   db,
                               std::shared_ptr<MqttClient> client,
                               long                        ts) {
    for (const auto& r : readings) {
        // 每条记录一个 task。db / client 按值进 lambda —— 是快照不是引用,
        // 热加载 reset 外层指针时,这份快照让旧对象活到 task 干完(见头文件)。
        pool_.submit([db, client, device = r.device, value = r.value, ts] {
            db->insert(device, value, ts);
            client->publish("gateway/up/" + device, std::to_string(value));
        });
    }
}

}  // namespace gateway
