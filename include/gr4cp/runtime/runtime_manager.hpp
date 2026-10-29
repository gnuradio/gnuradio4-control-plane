#pragma once

#include "gr4cp/domain/session.hpp"
#include "gr4cp/domain/session_stream.hpp"
#include "gr4cp/runtime/block_settings.hpp"

#include <optional>
#include <string>

namespace gr4cp::runtime {

struct HttpStreamResponse {
    int status{200};
    std::string body;
    std::string content_type{"application/octet-stream"};
};

struct WebSocketStreamRoute {
    domain::InternalStreamBinding internal;
};

struct ExecutionObservation {
    domain::SessionState       state;
    std::string                execution_id;
    std::optional<std::string> completion_reason;
    std::optional<std::string> error;
};

class RuntimeManager {
public:
    virtual ~RuntimeManager() = default;

    // Reconcile an asynchronous execution without starting or restarting it.
    virtual std::optional<ExecutionObservation>      observe(const domain::Session&) { return std::nullopt; }
    virtual void prepare(const domain::Session& session) = 0;
    virtual void start(const domain::Session& session) = 0;
    virtual void stop(const domain::Session& session) = 0;
    virtual void destroy(const domain::Session& session) = 0;
    virtual std::optional<domain::StreamRuntimePlan> active_stream_plan(const domain::Session& session) = 0;
    virtual HttpStreamResponse fetch_http_stream(const domain::Session& session, const std::string& stream_id) = 0;
    virtual WebSocketStreamRoute resolve_websocket_stream(const domain::Session& session, const std::string& stream_id) = 0;
    virtual void set_block_settings(const domain::Session& session,
                                    const std::string& block_unique_name,
                                    const gr::property_map& patch,
                                    BlockSettingsMode mode) = 0;
    virtual gr::property_map get_block_settings(const domain::Session& session,
                                                const std::string& block_unique_name) = 0;
};

}  // namespace gr4cp::runtime
