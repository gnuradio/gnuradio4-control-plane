#pragma once

#include "gr4cp/runtime/runtime_manager.hpp"
#include "gr4cp/runtime/stream_binding_allocator.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gr4cp::runtime {

class StubRuntimeManager final : public RuntimeManager {
public:
    StubRuntimeManager() = default;
    ~StubRuntimeManager() override = default;

    void prepare(const domain::Session& session) override;
    void start(const domain::Session& session) override;
    std::optional<std::string> stop(const domain::Session& session) override;
    void destroy(const domain::Session& session) override;
    std::optional<domain::StreamRuntimePlan> active_stream_plan(const domain::Session& session) override;
    HttpStreamResponse fetch_http_stream(const domain::Session& session, const std::string& stream_id) override;
    WebSocketStreamRoute resolve_websocket_stream(const domain::Session& session, const std::string& stream_id) override;
    void set_block_settings(const domain::Session& session,
                            const std::string& block_unique_name,
                            const gr::property_map& patch,
                            BlockSettingsMode mode) override;
    gr::property_map get_block_settings(const domain::Session& session, const std::string& block_unique_name) override;
    RunObservation observe(const domain::Session&) override { return {}; }

private:
    void release_streams(const std::string& session_id);

    StreamBindingAllocator stream_allocator_;
    std::unordered_map<std::string, std::vector<domain::RuntimeStreamBinding>> stream_bindings_;
    std::unordered_map<std::string, std::unordered_map<std::string, gr::property_map>> settings_;
};

}  // namespace gr4cp::runtime
