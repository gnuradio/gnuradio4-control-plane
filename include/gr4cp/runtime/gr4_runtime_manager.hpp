#pragma once

#include "gr4cp/runtime/runtime_manager.hpp"
#include "gr4cp/runtime/stream_binding_allocator.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gr {
class PluginLoader;
}

namespace gr4cp::runtime {

// A scheduler id the package offers in place of a core scheduler type.
struct SchedulerAlias {
    std::string_view alias;
    std::string_view type;
};

// The package's scheduler aliases, each with the core type it names.
std::span<const SchedulerAlias> scheduler_aliases();

// The scheduler type a session's `scheduler_id` names: the core type behind one of the package's aliases, else the id
// itself.
std::string core_scheduler_type(std::string_view scheduler_id);

// Registers each alias into the global scheduler registry as a name that creates its core type. A second call does
// nothing.
void register_scheduler_aliases();

class Gr4RuntimeManager final : public RuntimeManager {
public:
    // Registers the package's scheduler aliases. A document's nested scheduler can name one from the first session on.
    explicit Gr4RuntimeManager(std::shared_ptr<gr::PluginLoader> loader);
    ~Gr4RuntimeManager() override;

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
    RunObservation observe(const domain::Session& session) override;

    // The type of the scheduler the session's prepared run holds, or nothing without a prepared run.
    std::optional<std::string> scheduler_type(const domain::Session& session);

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

    std::shared_ptr<SessionRuntimeResources> get_or_create_resources(const domain::Session& session);
    std::shared_ptr<SessionRuntimeResources> find_resources(const std::string& session_id);
    struct RunningExecution;
    std::optional<RunningExecution> running_execution(const domain::Session& session);
    std::optional<RunningExecution> live_execution(const domain::Session& session);
    Execution& prepare_locked(const domain::Session& session, SessionRuntimeResources& resources);
    void release_stream_bindings_locked(SessionRuntimeResources& resources);
    std::optional<std::string> stop_locked(const domain::Session& session,
                                           SessionRuntimeResources& resources,
                                           std::unique_lock<std::mutex>& lock);

    std::shared_ptr<gr::PluginLoader> loader_;
    StreamBindingAllocator stream_allocator_;
    std::unordered_map<std::string, std::shared_ptr<SessionRuntimeResources>> resources_;
    std::mutex resources_mutex_;
};

}  // namespace gr4cp::runtime
