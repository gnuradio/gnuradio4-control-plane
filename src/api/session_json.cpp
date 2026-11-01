#include "session_json.hpp"

#include <array>
#include <random>

namespace gr4cp::api {

namespace {

using Json = nlohmann::json;

Json stream_to_json(const domain::BrowserStreamDescriptor& stream) {
    return Json{
        {"id", stream.id},
        {"block_instance_name", stream.block_instance_name},
        {"transport", stream.transport},
        {"payload_format", stream.payload_format},
        {"path", stream.path},
        {"ready", stream.ready},
    };
}

Json stream_plan_to_json(const domain::StreamRuntimePlan& plan) {
    auto body = Json::array();
    for (const auto& stream_plan : plan.streams) {
        body.push_back(stream_to_json(domain::to_browser_stream_descriptor(stream_plan)));
    }
    return body;
}

}  // namespace

const std::string& server_instance_id() {
    // One UUID v4 per server process or loaded WebAssembly module. Use the
    // standard library so the serializer does not depend on native Boost headers.
    static const std::string identity = [] {
        std::random_device random;
        std::uniform_int_distribution<unsigned int> distribution(0, 255);
        std::array<unsigned char, 16> bytes;
        for (auto& byte : bytes) {
            byte = static_cast<unsigned char>(distribution(random));
        }
        bytes[6] = (bytes[6] & 0x0f) | 0x40;
        bytes[8] = (bytes[8] & 0x3f) | 0x80;

        constexpr char hex[] = "0123456789abcdef";
        std::string uuid;
        uuid.reserve(36);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) {
                uuid += '-';
            }
            uuid += hex[bytes[i] >> 4];
            uuid += hex[bytes[i] & 0x0f];
        }
        return uuid;
    }();
    return identity;
}

Json session_to_json(const domain::Session& session, const std::optional<domain::StreamRuntimePlan>& stream_plan) {
    auto body = Json{
        {"id", session.id},
        {"server_instance_id", server_instance_id()},
        {"name", session.name},
        {"state", domain::to_string(session.state)},
        {"created_at", domain::format_timestamp_utc(session.created_at)},
        {"updated_at", domain::format_timestamp_utc(session.updated_at)},
        {"last_error", session.last_error ? Json(*session.last_error) : Json(nullptr)},
    };

    if (session.scheduler_alias.has_value()) {
        body["scheduler_id"] = *session.scheduler_alias;
    }

    if (stream_plan.has_value()) {
        body["streams"] = stream_plan_to_json(*stream_plan);
    }

    return body;
}

Json sessions_to_json(const std::vector<domain::Session>& sessions,
                      const std::vector<std::optional<domain::StreamRuntimePlan>>& stream_plans) {
    auto body = Json::array();
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        const auto& plan = index < stream_plans.size() ? stream_plans[index] : std::optional<domain::StreamRuntimePlan>{};
        body.push_back(session_to_json(sessions[index], plan));
    }
    return body;
}

}  // namespace gr4cp::api
