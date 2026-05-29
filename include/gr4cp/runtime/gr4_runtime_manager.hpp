#pragma once

#include "gr4cp/runtime/runtime_manager.hpp"
#include "gr4cp/runtime/stream_binding_allocator.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gr4cp::runtime {

class Gr4RuntimeManager final : public RuntimeManager {
public:
    explicit Gr4RuntimeManager(std::vector<std::filesystem::path> plugin_directories = {});
    ~Gr4RuntimeManager() override;

    void prepare(const domain::Session& session) override;
    void start(const domain::Session& session) override;
    void stop(const domain::Session& session) override;
    void destroy(const domain::Session& session) override;
    std::optional<domain::StreamRuntimePlan> active_stream_plan(const domain::Session& session) override;
    HttpStreamResponse fetch_http_stream(const domain::Session& session, const std::string& stream_id) override;
    WebSocketStreamRoute resolve_websocket_stream(const domain::Session& session, const std::string& stream_id) override;
    void set_block_settings(const domain::Session& session,
                            const std::string& block_unique_name,
                            const gr::property_map& patch,
                            BlockSettingsMode mode) override;
    gr::property_map get_block_settings(const domain::Session& session, const std::string& block_unique_name) override;

private:
    struct Execution;
    enum class LifecyclePhase {
        Idle,
        Starting,
        Running,
        Stopping,
        Failed,
    };
    struct SessionRuntimeResources;

    static std::vector<std::filesystem::path> default_plugin_directories();

    std::shared_ptr<SessionRuntimeResources> get_or_create_resources(const domain::Session& session);
    std::shared_ptr<SessionRuntimeResources> find_resources(const std::string& session_id);
    Execution& prepare_locked(const domain::Session& session, SessionRuntimeResources& resources);
    void release_stream_bindings_locked(SessionRuntimeResources& resources);
    void drain_errors_locked(SessionRuntimeResources& resources);
    void stop_locked(const domain::Session& session,
                     SessionRuntimeResources& resources,
                     std::unique_lock<std::mutex>& lock);

    std::vector<std::filesystem::path> plugin_directories_;
    StreamBindingAllocator stream_allocator_;
    std::unordered_map<std::string, std::shared_ptr<SessionRuntimeResources>> resources_;
    std::mutex resources_mutex_;
};

}  // namespace gr4cp::runtime
