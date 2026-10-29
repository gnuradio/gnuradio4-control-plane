#pragma once

#include "gr4cp/domain/session.hpp"
#include "gr4cp/domain/session_stream.hpp"
#include "gr4cp/runtime/block_settings.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
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

// A session has no run in progress, or its run is ending by itself.
class SessionNotRunningError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The changes in a session's run since the previous observe() call.
struct RunObservation {
    std::optional<std::string> last_error;  // the text of the last error a block reported
    bool ended{false};                      // the run ended by itself
    std::optional<std::string> run_error;   // the run's error when it ended with one
    std::shared_ptr<void> ended_run;        // the ended run's graph, blocks and stream ports, freed with the last copy
};

class RuntimeManager {
public:
    virtual ~RuntimeManager() = default;

    virtual void prepare(const domain::Session& session) = 0;
    virtual void start(const domain::Session& session) = 0;
    // Stops the session's run. Returns the error of a run that ended by itself with one.
    virtual std::optional<std::string> stop(const domain::Session& session) = 0;
    virtual void destroy(const domain::Session& session) = 0;
    virtual std::optional<domain::StreamRuntimePlan> active_stream_plan(const domain::Session& session) = 0;
    virtual HttpStreamResponse fetch_http_stream(const domain::Session& session, const std::string& stream_id) = 0;
    virtual WebSocketStreamRoute resolve_websocket_stream(const domain::Session& session, const std::string& stream_id) = 0;
    virtual void set_block_settings(const domain::Session& session,
                                    const std::string& block_unique_name,
                                    const gr::property_map& patch,
                                    BlockSettingsMode mode) = 0;
    // Reads a block's settings in the run in progress, else the settings the session's last ended run left. Throws
    // SessionNotRunningError when the session has neither, or its run is stopping.
    virtual gr::property_map get_block_settings(const domain::Session& session,
                                                const std::string& block_unique_name) = 0;
    // Reports the last error a block of the run raised since the previous call, and whether the run ended. An ended
    // run's graph, blocks and stream ports go with `ended_run`, and the caller destroys it after releasing its own
    // locks. Throws SessionNotRunningError when the session has no run in progress.
    virtual RunObservation observe(const domain::Session& session) = 0;
};

}  // namespace gr4cp::runtime
