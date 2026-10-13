#include "gr4cp/app/session_service.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <memory>
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
    std::vector<std::shared_ptr<void>> ended_runs;  // destroyed after the lock is released
    std::lock_guard lock(mutex_);
    auto sessions = repository_.list();
    std::sort(sessions.begin(), sessions.end(), [](const domain::Session& left, const domain::Session& right) {
        if (left.created_at == right.created_at) {
            return left.id < right.id;
        }
        return left.created_at < right.created_at;
    });
    for (auto& session : sessions) {
        session = observe_locked(std::move(session), ended_runs);
    }
    return sessions;
}

domain::Session SessionService::get(const std::string& id) {
    std::vector<std::shared_ptr<void>> ended_runs;  // destroyed after the lock is released
    std::lock_guard lock(mutex_);
    return observe_locked(load_or_throw(id), ended_runs);
}

domain::Session SessionService::start(const std::string& id) {
    auto session = domain::Session{};
    std::vector<std::shared_ptr<void>> ended_runs;
    {
        std::lock_guard lock(mutex_);
        session = observe_locked(load_or_throw(id), ended_runs);
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
    ended_runs.clear();

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
        return session;
    }
}

domain::Session SessionService::stop(const std::string& id) {
    auto session = domain::Session{};
    std::vector<std::shared_ptr<void>> ended_runs;
    {
        std::lock_guard lock(mutex_);
        const auto stored = load_or_throw(id);
        session = observe_locked(stored, ended_runs);
        const auto phase = phase_for_locked(session);

        if (phase == InternalLifecyclePhase::Stopping) {
            throw InvalidStateError("session stop already in progress: " + id);
        }
        if (phase == InternalLifecyclePhase::Starting) {
            throw InvalidStateError("session start in progress: " + id);
        }
        if (stored.state == domain::SessionState::Running && session.state != domain::SessionState::Running) {
            return session;  // the run ended by itself, and the read recorded its end
        }
        if (session.state == domain::SessionState::Stopped && phase == InternalLifecyclePhase::Idle) {
            return session;
        }

        phases_[id] = InternalLifecyclePhase::Stopping;
    }
    ended_runs.clear();

    std::optional<std::string> run_error;
    try {
        run_error = runtime_manager_.stop(session);
    } catch (const std::exception& error) {
        record_runtime_failure(std::move(session), runtime_message("stop", id, error));
    } catch (...) {
        record_runtime_failure(std::move(session), runtime_message("stop", id));
    }

    {
        std::lock_guard lock(mutex_);
        session = load_or_throw(id);
        session.state = run_error.has_value() ? domain::SessionState::Error : domain::SessionState::Stopped;
        session.last_error = run_error;
        session.updated_at = now_utc();
        store_session_locked(session, run_error.has_value() ? InternalLifecyclePhase::Failed : InternalLifecyclePhase::Idle);
        return session;
    }
}

domain::Session SessionService::restart(const std::string& id) {
    auto session = domain::Session{};
    std::vector<std::shared_ptr<void>> ended_runs;
    {
        std::lock_guard lock(mutex_);
        session = observe_locked(load_or_throw(id), ended_runs);
        const auto phase = phase_for_locked(session);

        if (phase == InternalLifecyclePhase::Stopping) {
            throw InvalidStateError("session stop in progress: " + id);
        }
        if (phase == InternalLifecyclePhase::Starting) {
            throw InvalidStateError("session start already in progress: " + id);
        }

        phases_[id] = InternalLifecyclePhase::Starting;
    }
    ended_runs.clear();

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
        return session;
    }
}

void SessionService::remove(const std::string& id) {
    auto session = domain::Session{};
    std::vector<std::shared_ptr<void>> ended_runs;
    {
        std::lock_guard lock(mutex_);
        session = observe_locked(load_or_throw(id), ended_runs);
        const auto phase = phase_for_locked(session);

        if (phase == InternalLifecyclePhase::Stopping) {
            throw InvalidStateError("session stop in progress: " + id);
        }
        if (phase == InternalLifecyclePhase::Starting) {
            throw InvalidStateError("session start in progress: " + id);
        }

        phases_[id] = InternalLifecyclePhase::Stopping;
    }
    ended_runs.clear();

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
    const auto session = running_session(id);
    if (!session.has_value()) {
        return std::nullopt;
    }

    try {
        return runtime_manager_.active_stream_plan(*session);
    } catch (const std::exception& error) {
        throw RuntimeError(runtime_message("stream lookup", id, error));
    } catch (...) {
        throw RuntimeError(runtime_message("stream lookup", id));
    }
}

runtime::HttpStreamResponse SessionService::fetch_http_stream(const std::string& id, const std::string& stream_id) {
    const auto session = running_session(id);
    if (!session.has_value()) {
        throw InvalidStateError("session is not running: " + id);
    }

    static constexpr auto kTransientFetchRetries = 20;
    static constexpr auto kTransientFetchRetryDelay = std::chrono::milliseconds(50);

    for (int attempt = 0; attempt < kTransientFetchRetries; ++attempt) {
        try {
            return runtime_manager_.fetch_http_stream(*session, stream_id);
        } catch (const runtime::SessionNotRunningError& error) {
            throw InvalidStateError(error.what());
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
    const auto session = running_session(id);
    if (!session.has_value()) {
        throw InvalidStateError("session is not running: " + id);
    }

    try {
        return runtime_manager_.resolve_websocket_stream(*session, stream_id);
    } catch (const runtime::SessionNotRunningError& error) {
        throw InvalidStateError(error.what());
    } catch (const std::exception& error) {
        throw RuntimeError(runtime_message("websocket route", id, error));
    } catch (...) {
        throw RuntimeError(runtime_message("websocket route", id));
    }
}

std::optional<domain::Session> SessionService::running_session(const std::string& id) {
    std::vector<std::shared_ptr<void>> ended_runs;  // destroyed after the lock is released
    std::lock_guard lock(mutex_);
    auto session = observe_locked(load_or_throw(id), ended_runs);
    if (session.state != domain::SessionState::Running || phase_for_locked(session) != InternalLifecyclePhase::Running) {
        return std::nullopt;
    }
    return session;
}

domain::Session SessionService::observe_locked(domain::Session session, std::vector<std::shared_ptr<void>>& ended_runs) {
    if (session.state != domain::SessionState::Running || phase_for_locked(session) != InternalLifecyclePhase::Running) {
        return session;
    }

    runtime::RunObservation observation;
    try {
        observation = runtime_manager_.observe(session);
    } catch (const runtime::SessionNotRunningError&) {
        observation.ended = true;  // the manager holds no run for the session
    }
    if (observation.ended_run) {
        ended_runs.push_back(std::move(observation.ended_run));
    }
    if (!observation.last_error.has_value() && !observation.ended) {
        return session;
    }

    if (observation.last_error.has_value()) {
        session.last_error = observation.last_error;
    }
    auto phase = InternalLifecyclePhase::Running;
    if (observation.ended && observation.run_error.has_value()) {
        session.state = domain::SessionState::Error;
        session.last_error = observation.run_error;
        phase = InternalLifecyclePhase::Failed;
    } else if (observation.ended) {
        session.state = domain::SessionState::Stopped;
        phase = InternalLifecyclePhase::Idle;
    }
    session.updated_at = now_utc();
    store_session_locked(session, phase);
    return session;
}

domain::Session SessionService::load_or_throw(const std::string& id) {
    auto session = repository_.get(id);
    if (!session.has_value()) {
        throw NotFoundError("session not found: " + id);
    }
    return *session;
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
