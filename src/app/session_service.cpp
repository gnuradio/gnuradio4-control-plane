#include "gr4cp/app/session_service.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace gr4cp::app {

namespace {

gr4cp::domain::Timestamp now_utc() {
    return std::chrono::system_clock::now();
}

std::string generate_session_id() {
    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_int_distribution<unsigned long long> distribution;

    std::ostringstream out;
    out << "sess_" << std::hex << std::setw(16) << std::setfill('0') << distribution(generator);
    return out.str();
}

std::string runtime_message(const std::string& action, const std::string& id, const std::exception& error) {
    return "runtime " + action + " failed for session " + id + ": " + error.what();
}

std::string runtime_message(const std::string& action, const std::string& id) {
    return "runtime " + action + " failed for session " + id;
}

bool is_transient_http_stream_connect_error(const std::exception& error) {
    constexpr std::string_view marker = "failed to reach internal HTTP stream endpoint";
    return std::string_view(error.what()).find(marker) != std::string_view::npos;
}

}  // namespace

SessionService::SessionService(storage::SessionRepository& repository,
                               runtime::RuntimeManager& runtime_manager)
    : repository_(repository), runtime_manager_(runtime_manager) {}

SessionService::InternalLifecyclePhase SessionService::phase_for_locked(const domain::Session& session) {
    const auto it = phases_.find(session.id);
    if (it != phases_.end()) {
        return it->second;
    }

    switch (session.state) {
    case domain::SessionState::Stopped:
        return InternalLifecyclePhase::Idle;
    case domain::SessionState::Running:
        return InternalLifecyclePhase::Running;
    case domain::SessionState::Error:
        return InternalLifecyclePhase::Failed;
    }

    return InternalLifecyclePhase::Failed;
}

domain::Session SessionService::create(const std::string& name,
                                       const std::string& grc,
                                       std::optional<std::string> scheduler_alias) {
    std::lock_guard lock(mutex_);

    if (grc.empty()) {
        throw ValidationError("grc_content must not be empty");
    }

    auto session = domain::Session{};
    session.name = name;
    session.grc_content = grc;
    session.scheduler_alias = std::move(scheduler_alias);
    session.state = domain::SessionState::Stopped;

    do {
        session.id = generate_session_id();
    } while (repository_.get(session.id).has_value());

    session.created_at = now_utc();
    session.updated_at = session.created_at;

    repository_.create(session);
    phases_[session.id] = InternalLifecyclePhase::Idle;
    return session;
}

std::vector<domain::Session> SessionService::list() {
    std::lock_guard lock(mutex_);
    auto sessions = repository_.list();
    for (auto& session : sessions) {
        session = load_or_throw(session.id);
    }
    std::sort(sessions.begin(), sessions.end(), [](const domain::Session& left, const domain::Session& right) {
        if (left.created_at == right.created_at) {
            return left.id < right.id;
        }
        return left.created_at < right.created_at;
    });
    return sessions;
}

domain::Session SessionService::get(const std::string& id) {
    std::lock_guard lock(mutex_);
    return load_or_throw(id);
}

domain::Session SessionService::start(const std::string& id) {
    auto session = domain::Session{};
    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        const auto phase = phase_for_locked(session);

        if (session.state == domain::SessionState::Running || phase == InternalLifecyclePhase::Running) {
            throw InvalidStateError("session already running: " + id);
        }
        if (phase == InternalLifecyclePhase::Starting) {
            throw InvalidStateError("session start already in progress: " + id);
        }
        if (phase == InternalLifecyclePhase::Stopping) {
            throw InvalidStateError("session stop in progress: " + id);
        }

        phases_[id] = InternalLifecyclePhase::Starting;
    }

    try {
        runtime_manager_.prepare(session);
        runtime_manager_.start(session);
    } catch (const std::exception& error) {
        record_runtime_failure(std::move(session), runtime_message("start", id, error));
    } catch (...) {
        record_runtime_failure(std::move(session), runtime_message("start", id));
    }

    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        session.state = domain::SessionState::Running;
        session.last_error.reset();
        session.updated_at = now_utc();
        store_session_locked(session, InternalLifecyclePhase::Running);
        return load_or_throw(id);
    }
}

domain::Session SessionService::stop(const std::string& id) {
    auto session = domain::Session{};
    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        const auto phase = phase_for_locked(session);

        if (phase == InternalLifecyclePhase::Stopping) {
            throw InvalidStateError("session stop already in progress: " + id);
        }
        if (phase == InternalLifecyclePhase::Starting) {
            throw InvalidStateError("session start in progress: " + id);
        }
        if (session.state == domain::SessionState::Stopped && phase == InternalLifecyclePhase::Idle) {
            return session;
        }

        phases_[id] = InternalLifecyclePhase::Stopping;
    }

    try {
        runtime_manager_.stop(session);
    } catch (const std::exception& error) {
        record_runtime_failure(std::move(session), runtime_message("stop", id, error));
    } catch (...) {
        record_runtime_failure(std::move(session), runtime_message("stop", id));
    }

    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        session.state = domain::SessionState::Stopped;
        if (auto observed = runtime_manager_.observe(session)) {
            session.execution_id      = observed->execution_id;
            session.completion_reason = observed->completion_reason;
        }
        session.last_error.reset();
        session.updated_at = now_utc();
        store_session_locked(session, InternalLifecyclePhase::Idle);
        return session;
    }
}

domain::Session SessionService::restart(const std::string& id) {
    auto session = domain::Session{};
    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        const auto phase = phase_for_locked(session);

        if (phase == InternalLifecyclePhase::Stopping) {
            throw InvalidStateError("session stop in progress: " + id);
        }
        if (phase == InternalLifecyclePhase::Starting) {
            throw InvalidStateError("session start already in progress: " + id);
        }

        phases_[id] = InternalLifecyclePhase::Starting;
    }

    try {
        if (session.state == domain::SessionState::Running) {
            runtime_manager_.stop(session);
            session.state = domain::SessionState::Stopped;
        }
        runtime_manager_.destroy(session);
        runtime_manager_.prepare(session);
        runtime_manager_.start(session);
    } catch (const std::exception& error) {
        record_runtime_failure(std::move(session), runtime_message("restart", id, error));
    } catch (...) {
        record_runtime_failure(std::move(session), runtime_message("restart", id));
    }

    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        session.state = domain::SessionState::Running;
        session.last_error.reset();
        session.updated_at = now_utc();
        store_session_locked(session, InternalLifecyclePhase::Running);
        return load_or_throw(id);
    }
}

void SessionService::remove(const std::string& id) {
    auto session = domain::Session{};
    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        const auto phase = phase_for_locked(session);

        if (phase == InternalLifecyclePhase::Stopping) {
            throw InvalidStateError("session stop in progress: " + id);
        }
        if (phase == InternalLifecyclePhase::Starting) {
            throw InvalidStateError("session start in progress: " + id);
        }

        phases_[id] = InternalLifecyclePhase::Stopping;
    }

    try {
        if (session.state == domain::SessionState::Running) {
            runtime_manager_.stop(session);
            session.state = domain::SessionState::Stopped;
        }
        runtime_manager_.destroy(session);
    } catch (const std::exception& error) {
        record_runtime_failure(std::move(session), runtime_message("remove", id, error));
    } catch (...) {
        record_runtime_failure(std::move(session), runtime_message("remove", id));
    }

    {
        std::lock_guard lock(mutex_);
        if (!repository_.remove(id)) {
            throw NotFoundError("session not found: " + id);
        }
        erase_phase_locked(id);
    }
}

std::optional<domain::StreamRuntimePlan> SessionService::active_stream_plan(const std::string& id) {
    domain::Session session;
    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        if (session.state != domain::SessionState::Running || phase_for_locked(session) != InternalLifecyclePhase::Running) {
            return std::nullopt;
        }
    }

    try {
        return runtime_manager_.active_stream_plan(session);
    } catch (const std::exception& error) {
        throw RuntimeError(runtime_message("stream lookup", id, error));
    } catch (...) {
        throw RuntimeError(runtime_message("stream lookup", id));
    }
}

runtime::HttpStreamResponse SessionService::fetch_http_stream(const std::string& id, const std::string& stream_id) {
    domain::Session session;
    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        if (session.state != domain::SessionState::Running || phase_for_locked(session) != InternalLifecyclePhase::Running) {
            throw InvalidStateError("session is not running: " + id);
        }
    }

    static constexpr auto kTransientFetchRetries = 20;
    static constexpr auto kTransientFetchRetryDelay = std::chrono::milliseconds(50);

    for (int attempt = 0; attempt < kTransientFetchRetries; ++attempt) {
        try {
            return runtime_manager_.fetch_http_stream(session, stream_id);
        } catch (const std::exception& error) {
            if (attempt + 1 == kTransientFetchRetries || !is_transient_http_stream_connect_error(error)) {
                throw RuntimeError(runtime_message("stream fetch", id, error));
            }
            std::this_thread::sleep_for(kTransientFetchRetryDelay);
        } catch (...) {
            throw RuntimeError(runtime_message("stream fetch", id));
        }
    }

    throw RuntimeError(runtime_message("stream fetch", id));
}

runtime::WebSocketStreamRoute SessionService::resolve_websocket_stream(const std::string& id, const std::string& stream_id) {
    domain::Session session;
    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        if (session.state != domain::SessionState::Running || phase_for_locked(session) != InternalLifecyclePhase::Running) {
            throw InvalidStateError("session is not running: " + id);
        }
    }

    try {
        return runtime_manager_.resolve_websocket_stream(session, stream_id);
    } catch (const std::exception& error) {
        throw RuntimeError(runtime_message("websocket route", id, error));
    } catch (...) {
        throw RuntimeError(runtime_message("websocket route", id));
    }
}

domain::Session SessionService::load_or_throw(const std::string& id) {
    auto session = repository_.get(id);
    if (!session.has_value()) {
        throw NotFoundError("session not found: " + id);
    }
    auto result = *session;
    if (phase_for_locked(result) == InternalLifecyclePhase::Running) {
        if (auto observed = runtime_manager_.observe(result)) {
            result.execution_id      = observed->execution_id;
            result.completion_reason = observed->completion_reason;
            result.last_error        = observed->error;
            if (result.state != observed->state) {
                result.state      = observed->state;
                result.updated_at = now_utc();
            }
            store_session_locked(result, result.state == domain::SessionState::Running ? InternalLifecyclePhase::Running : result.state == domain::SessionState::Error ? InternalLifecyclePhase::Failed : InternalLifecyclePhase::Idle);
        }
    }
    return result;
}

[[noreturn]] void SessionService::record_runtime_failure(domain::Session session, const std::string& message) {
    std::lock_guard lock(mutex_);
    session = load_or_throw(session.id);
    session.state = domain::SessionState::Error;
    session.last_error = message;
    session.updated_at = now_utc();
    store_session_locked(session, InternalLifecyclePhase::Failed);
    throw RuntimeError(message);
}

void SessionService::store_session_locked(const domain::Session& session, const InternalLifecyclePhase phase) {
    repository_.update(session);
    phases_[session.id] = phase;
}

void SessionService::erase_phase_locked(const std::string& id) {
    phases_.erase(id);
}

}  // namespace gr4cp::app
