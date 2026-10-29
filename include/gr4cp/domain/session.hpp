#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace gr4cp::domain {

enum class SessionState {
    Stopped,
    Running,
    Error,
};

// How a session's last run ended: by itself without an error, by a stop, or with an error.
enum class CompletionReason {
    Completed,
    Stopped,
    Error,
};

using Timestamp = std::chrono::system_clock::time_point;

struct Session {
    std::string id;
    std::string name;
    std::string grc_content;
    std::optional<std::string> scheduler_alias;
    SessionState state{SessionState::Stopped};
    std::optional<std::string> last_error;
    std::optional<CompletionReason> completion_reason;  // set once a run has ended, cleared when a run starts
    Timestamp created_at;
    Timestamp updated_at;
};

std::string to_string(SessionState state);
SessionState session_state_from_string(std::string_view state);
std::string to_string(CompletionReason reason);
std::string format_timestamp_utc(const Timestamp& timestamp);

}  // namespace gr4cp::domain
