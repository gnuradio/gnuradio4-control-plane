#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "gr4cp/domain/session.hpp"
#include "gr4cp/runtime/runtime_manager.hpp"
#include "gr4cp/storage/session_repository.hpp"

namespace gr4cp::app {

class NotFoundError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ValidationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class InvalidStateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RuntimeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SessionService {
public:
    SessionService(storage::SessionRepository& repository, runtime::RuntimeManager& runtime_manager);

    domain::Session create(const std::string& name,
                           const std::string& grc,
                           std::optional<std::string> scheduler_alias = std::nullopt);
    std::vector<domain::Session> list();
    domain::Session get(const std::string& id);
    domain::Session start(const std::string& id);
    domain::Session stop(const std::string& id);
    domain::Session restart(const std::string& id);
    void remove(const std::string& id);
    std::optional<domain::StreamRuntimePlan> active_stream_plan(const std::string& id);
    runtime::HttpStreamResponse fetch_http_stream(const std::string& id, const std::string& stream_id);
    runtime::WebSocketStreamRoute resolve_websocket_stream(const std::string& id, const std::string& stream_id);

private:
    enum class InternalLifecyclePhase {
        Idle,
        Starting,
        Running,
        Stopping,
        Failed,     // a start, a restart or a removal failed
        RunFailed,  // the run ended with an error or its stop failed, and a stop answers the session as it stands
    };

    InternalLifecyclePhase phase_for_locked(const domain::Session& session);
    // Updates a running session from its run: the last error a block reported, and the run's end. An ended run goes
    // to `ended_runs`, which the caller destroys after releasing the mutex. A caller declares the vector before its
    // lock, or clears it after the lock is released.
    domain::Session observe_locked(domain::Session session, std::vector<std::shared_ptr<void>>& ended_runs);
    // Reads the session as `get` does. Returns it when its run is in progress, and nothing otherwise.
    std::optional<domain::Session> running_session(const std::string& id);
    domain::Session load_or_throw(const std::string& id);
    void store_session_locked(const domain::Session& session, InternalLifecyclePhase phase);
    void erase_phase_locked(const std::string& id);
    // Records the session in error with `message` in `phase`, with `reason` as its completion reason: an error for a
    // stop or a removal that failed, none for a start or a restart that failed. Throws RuntimeError with `message`.
    [[noreturn]] void record_runtime_failure(domain::Session session,
                                             const std::string& message,
                                             std::optional<domain::CompletionReason> reason,
                                             InternalLifecyclePhase phase = InternalLifecyclePhase::Failed);

    storage::SessionRepository& repository_;
    runtime::RuntimeManager& runtime_manager_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InternalLifecyclePhase> phases_;
};

}  // namespace gr4cp::app
