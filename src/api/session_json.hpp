#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "gr4cp/domain/session.hpp"
#include "gr4cp/domain/session_stream.hpp"

namespace gr4cp::api {

const std::string& server_instance_id();

nlohmann::json session_to_json(const domain::Session& session,
                               const std::optional<domain::StreamRuntimePlan>& stream_plan = std::nullopt);
nlohmann::json sessions_to_json(const std::vector<domain::Session>& sessions,
                                const std::vector<std::optional<domain::StreamRuntimePlan>>& stream_plans);

}  // namespace gr4cp::api
