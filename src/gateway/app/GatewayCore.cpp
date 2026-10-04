#include "gateway/app/GatewayCore.h"

#include "gateway/core/format/Number.h"
#include "gateway/pipeline/TelemetryDecoder.h"

#include <future>
#include <map>
#include <utility>

namespace gateway {
namespace {
HttpResponse json(unsigned status, std::string body) {
    return HttpResponse{status, "application/json", std::move(body)};
}

// URL 解码只用于应用参数；非法编码拒绝，参数使用数据库绑定而非拼接 SQL。
bool decodeUrl(const std::string& input, std::string& output) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.size()) return false;
            const int hi = hex(input[i + 1]), lo = hex(input[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = static_cast<char>((hi << 4) | lo);
            i += 2;
        } else if (c == '+') c = ' ';
        if (c == '\0') return false;
        output += c;
    }
    return true;
}

std::string escapeJson(const std::string& text) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') { result += '\\'; result += ch; }
        else if (c < 0x20) {
            result += "\\u00";
            result += digits[c >> 4]; result += digits[c & 15];
        } else result += ch;
    }
    return result;
}

std::string rowsToJson(const std::vector<DataRow>& rows) {
    std::string body = "[";
    for (const auto& row : rows) {
        if (body.size() > 1) body += ',';
        body += "{\"device_id\":\"" + escapeJson(row.device_id) + "\",\"value\":" +
                formatValue(row.value) + ",\"ts\":" + std::to_string(row.ts) + "}";
    }
    return body + "]";
}
} // namespace

GatewayCore::GatewayCore(TelemetryPipeline& pipeline, SubmitCommand submit, Publish publish,
                         DatabaseProvider database, HttpRuntimeConfigProvider config)
    : pipeline_(pipeline), submit_(std::move(submit)), publish_(std::move(publish)),
      database_(std::move(database)), config_(std::move(config)) {}
GatewayCore::~GatewayCore() { stop(); }

bool GatewayCore::mqttMessage(std::string topic, std::string payload) {
    if (!accepting_.load() || topic.size() > 256 || payload.size() > 4096) return false;
    auto count = admitted_->load();
    do {
        if (count >= kMaxCommands) return false;
    } while (!admitted_->compare_exchange_weak(count, count + 1));
    if (core_.post([this, topic = std::move(topic), payload = std::move(payload)]() mutable {
        handleMqtt(std::move(topic), std::move(payload));
    })) return true;
    releaseCommand();
    return false;
}

void GatewayCore::releaseCommand() { admitted_->fetch_sub(1); }

void GatewayCore::publishCommand(std::string topic, std::string payload) {
    // 持有额度直到控制线程调用 publish；因此终态任务最多 kMaxCommands 个，
    // 不会被持续遥测挤占控制邮箱预留空间。回调不捕获 GatewayCore 生命周期。
    auto credits = admitted_;
    if (!publish_(std::move(topic), std::move(payload), 1,
                  [credits] { credits->fetch_sub(1); })) {
        LOG_ERROR("%s", "command publication rejected during shutdown");
        releaseCommand();
    }
}

bool GatewayCore::telemetry(Frame frame, long received_at) {
    if (!accepting_.load()) return false;
    return core_.post([this, frame = std::move(frame), received_at] {
        handleTelemetry(frame, received_at);
    });
}

bool GatewayCore::httpRequest(HttpRequest request, HttpReply reply) {
    if (!accepting_.load()) return false;
    return core_.post([this, request = std::move(request), reply = std::move(reply)]() mutable {
        handleHttp(std::move(request), std::move(reply));
    });
}

bool GatewayCore::commandFinished(uint64_t id, CommandCompletion result) {
    return core_.post([this, id, result = std::move(result)] { handleFinished(id, result); }, true);
}

bool GatewayCore::commandRejected(uint64_t id, std::string error) {
    return core_.post([this, id, error = std::move(error)] { reject(id, error); }, true);
}

void GatewayCore::handleMqtt(std::string topic, std::string payload) {
    auto translated = translateCommand(topic, payload);
    if (!translated.ok) {
        publishCommand("gateway/ack/rejected", translated.error);
        return;
    }
    const auto id = next_id_++;
    pending_.insert(id);
    if (!submit_(id, std::move(translated.cmd))) reject(id, "gateway_busy");
}

void GatewayCore::reject(uint64_t id, const std::string& error) {
    if (pending_.erase(id) == 0) return;
    publishCommand("gateway/ack/rejected", error);
}

void GatewayCore::handleTelemetry(const Frame& frame, long received_at) {
    const auto readings = decodeTelemetry(frame);
    for (const auto& reading : readings)
        publish_("gateway/up/" + reading.device, formatValue(reading.value), 0, {});
    if (!pipeline_.submit(readings, received_at))
        LOG_WARN("telemetry storage queue full, dropped %zu readings", readings.size());
}

void GatewayCore::handleFinished(uint64_t id, const CommandCompletion& result) {
    if (pending_.erase(id) == 0) return;
    const auto seq = std::to_string(result.seq);
    if (!result.error.empty()) {
        publishCommand("gateway/ack/" + seq, result.error);
    } else if (result.result.empty()) {
        publishCommand("gateway/ack/" + seq, "invalid_result");
    } else if (result.result[0] != EDGE_RC_OK) {
        publishCommand("gateway/ack/" + seq, "err," + std::to_string(result.result[0]));
    } else if (result.type == EDGE_TYPE_QUERY_TH && result.result.size() == 5) {
        const double t = edge_u16_be_read(result.result.data() + 1) / double(EDGE_TEMP_SCALE);
        const double h = edge_u16_be_read(result.result.data() + 3) / double(EDGE_HUMI_SCALE);
        publishCommand("gateway/resp/" + seq, "ok," + formatValue(t) + "," + formatValue(h));
    } else if (result.type == EDGE_TYPE_QUERY_LIGHT && result.result.size() == 3) {
        publishCommand("gateway/resp/" + seq,
                 "ok," + std::to_string(edge_u16_be_read(result.result.data() + 1)));
    } else if (result.type == EDGE_TYPE_SET_PERIOD && result.result.size() == 1) {
        publishCommand("gateway/ack/" + seq, "ok");
    } else publishCommand("gateway/ack/" + seq, "invalid_result");
}

void GatewayCore::handleHttp(HttpRequest request, HttpReply reply) {
    if (request.method != "GET" && request.method != "HEAD") {
        reply(json(405, "{\"error\":\"method not allowed\"}")); return;
    }
    const auto qmark = request.target.find('?');
    const auto path = request.target.substr(0, qmark);
    if (auto response = staticHttpResponse(path)) { reply(std::move(*response)); return; }
    if (path != "/api/data") { reply(json(404, "{\"error\":\"not found\"}")); return; }
    std::map<std::string, std::string> params;
    if (qmark != std::string::npos) {
        std::size_t pos = qmark + 1;
        while (pos < request.target.size()) {
            auto end = request.target.find('&', pos);
            if (end == std::string::npos) end = request.target.size();
            const auto part = request.target.substr(pos, end - pos);
            const auto eq = part.find('=');
            if (eq != std::string::npos) {
                std::string key, value;
                if (!decodeUrl(part.substr(0, eq), key) || !decodeUrl(part.substr(eq + 1), value)) {
                    reply(json(400, "{\"error\":\"invalid query encoding\"}")); return;
                }
                params[std::move(key)] = std::move(value);
            }
            pos = end + 1;
        }
    }
    const auto device = params["dev"];
    if (device.empty()) { reply(json(400, "{\"error\":\"missing dev param\"}")); return; }
    const int n = clampReportN(params["n"], config_().report_n);
    if (queries_ >= kMaxQueries) { reply(json(503, "{\"error\":\"query queue full\"}")); return; }
    auto db = database_();
    if (!db) { reply(json(503, "{\"error\":\"database unavailable\"}")); return; }
    ++queries_;
    // 只读连接只在这个执行器中调用；任务持有快照，切库不会让查询悬空。
    const bool accepted = queries_executor_.post([this, db = std::move(db), device, n, reply] {
        std::vector<DataRow> rows;
        bool ok = true;
        try { rows = db->query(device, n); }
        catch (const std::exception& error) { LOG_ERROR("query failed: %s", error.what()); ok = false; }
        if (!core_.post([this, rows = std::move(rows), ok, reply] {
            --queries_;
            reply(ok ? json(200, rowsToJson(rows)) : json(500, "{\"error\":\"query failed\"}"));
        }, true)) LOG_ERROR("%s", "query completion rejected during shutdown");
    });
    if (!accepted) { --queries_; reply(json(503, "{\"error\":\"query queue full\"}")); }
}

void GatewayCore::stop() {
    if (stopped_) return;
    stopped_ = true;
    accepting_.store(false);
    // 仅停机拥有者等待屏障；Reactor 和业务任务从不等待工作线程。
    auto barrier = std::make_shared<std::promise<void>>();
    auto reached = barrier->get_future();
    if (core_.post([barrier] { barrier->set_value(); }, true)) reached.wait();
    queries_executor_.stop(); // 屏障前的 HTTP 请求已提交，所有查询完成事件现在可排空。
    core_.stop();
}

} // namespace gateway
