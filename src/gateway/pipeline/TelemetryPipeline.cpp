#include "gateway/pipeline/TelemetryPipeline.h"

#include "gateway/core/format/Number.h"

#include <string>

namespace gateway {

void TelemetryPipeline::submit(const std::vector<Reading>& readings,
                               std::shared_ptr<Database>   db,
                               std::shared_ptr<MqttClient> client,
                               long                        ts) {
    for (const auto& r : readings) {
        // 每条记录一个 task。db / client 按值捕获为快照而非引用:热加载替换外层
        // 指针时,该快照使旧对象存活至 task 完成(见头文件)。
        pool_.submit([db, client, device = r.device, value = r.value, ts] {
            db->insert(device, value, ts);
            // formatValue 而非 to_string:后者固定六位小数,会把 25.3 写成 "25.300000"。
            // 三个文本出口(本处、查询应答、HTTP 的 JSON)共用同一份格式化。
            client->publish("gateway/up/" + device, formatValue(value));
        });
    }
}

}  // namespace gateway
