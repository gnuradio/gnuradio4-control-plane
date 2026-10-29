#include "gr4cp/domain/session.hpp"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace gr4cp::domain {

std::string to_string(const SessionState state) {
    switch (state) {
    case SessionState::Stopped:
        return "stopped";
    case SessionState::Running:
        return "running";
    case SessionState::Error:
        return "error";
    }

    throw std::invalid_argument("unknown session state");
}

SessionState session_state_from_string(const std::string_view state) {
    if (state == "stopped") {
        return SessionState::Stopped;
    }
    if (state == "running") {
        return SessionState::Running;
    }
    if (state == "error") {
        return SessionState::Error;
    }

    throw std::invalid_argument("invalid session state: " + std::string(state));
}

std::string to_string(const CompletionReason reason) {
    switch (reason) {
    case CompletionReason::Completed:
        return "completed";
    case CompletionReason::Stopped:
        return "stopped";
    case CompletionReason::Error:
        return "error";
    }

    throw std::invalid_argument("unknown completion reason");
}

std::string format_timestamp_utc(const Timestamp& timestamp) {
    const auto time = std::chrono::system_clock::to_time_t(timestamp);

    std::tm utc_time{};
#if defined(_WIN32)
    gmtime_s(&utc_time, &time);
#else
    gmtime_r(&time, &utc_time);
#endif

    std::ostringstream out;
    out << std::put_time(&utc_time, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

}  // namespace gr4cp::domain
