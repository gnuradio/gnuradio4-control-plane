#include "gr4cp/api/wasm_api.hpp"

#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace gr4cp::api {

namespace {

using Json = nlohmann::json;

Json stream_to_json(const domain::BrowserStreamDescriptor& stream) {
    return Json{
        {"id", stream.id},
        {"block_instance_name", stream.block_instance_name},
        {"transport", stream.transport},
        {"payload_format", stream.payload_format},
        {"path", stream.path},
        {"ready", stream.ready},
    };
}

Json stream_plan_to_json(const domain::StreamRuntimePlan& plan) {
    auto body = Json::array();
    for (const auto& stream_plan : plan.streams) {
        body.push_back(stream_to_json(domain::to_browser_stream_descriptor(stream_plan)));
    }
    return body;
}

Json block_parameter_default_to_json(const domain::BlockParameterDefault& value) {
    return std::visit([](const auto& item) -> Json { return item; }, value);
}

Json block_port_to_json(const domain::BlockPortDescriptor& port) {
    auto body = Json{
        {"name", port.name},
        {"type", port.type},
        {"cardinality_kind", port.cardinality_kind == domain::BlockPortCardinalityKind::Dynamic ? "dynamic" : "fixed"},
    };

    if (port.current_port_count.has_value()) {
        body["current_port_count"] = *port.current_port_count;
    }
    if (port.render_port_count.has_value()) {
        body["render_port_count"] = *port.render_port_count;
    }
    if (port.min_port_count.has_value()) {
        body["min_port_count"] = *port.min_port_count;
    }
    if (port.max_port_count.has_value()) {
        body["max_port_count"] = *port.max_port_count;
    }
    if (port.size_parameter.has_value()) {
        body["size_parameter"] = *port.size_parameter;
    }
    if (port.handle_name_template.has_value()) {
        body["handle_name_template"] = *port.handle_name_template;
    }

    return body;
}

Json block_parameter_to_json(const domain::BlockParameterDescriptor& parameter) {
    auto body = Json{
        {"name", parameter.name},
        {"type", parameter.type},
        {"required", parameter.required},
        {"default", block_parameter_default_to_json(parameter.default_value)},
        {"summary", parameter.summary},
    };

    if (parameter.runtime_mutability.has_value()) {
        body["runtime_mutability"] = *parameter.runtime_mutability;
    }
    if (parameter.value_kind.has_value()) {
        body["value_kind"] = *parameter.value_kind;
    }
    if (parameter.enum_choices.has_value()) {
        body["enum_choices"] = *parameter.enum_choices;
    }
    if (parameter.enum_type.has_value()) {
        body["enum_type"] = *parameter.enum_type;
    }
    if (!parameter.enum_labels.empty()) {
        body["enum_labels"] = parameter.enum_labels;
    }
    if (parameter.enum_source.has_value()) {
        body["enum_source"] = *parameter.enum_source;
    }
    if (parameter.ui_hint.has_value()) {
        body["ui_hint"] = *parameter.ui_hint;
    }
    if (parameter.allow_custom_value.has_value()) {
        body["allow_custom_value"] = *parameter.allow_custom_value;
    }

    return body;
}

Json block_to_json(const domain::BlockDescriptor& block) {
    auto inputs = Json::array();
    for (const auto& input : block.inputs) {
        inputs.push_back(block_port_to_json(input));
    }

    auto outputs = Json::array();
    for (const auto& output : block.outputs) {
        outputs.push_back(block_port_to_json(output));
    }

    auto parameters = Json::array();
    for (const auto& parameter : block.parameters) {
        parameters.push_back(block_parameter_to_json(parameter));
    }

    return Json{
        {"id", block.id},
        {"name", block.name},
        {"category", block.category},
        {"summary", block.summary},
        {"inputs", std::move(inputs)},
        {"outputs", std::move(outputs)},
        {"parameters", std::move(parameters)},
    };
}

Json blocks_to_json(const std::vector<domain::BlockDescriptor>& blocks) {
    auto body = Json::array();
    for (const auto& block : blocks) {
        body.push_back(block_to_json(block));
    }
    return body;
}

Json scheduler_to_json(const domain::SchedulerDescriptor& scheduler) {
    return Json{{"id", scheduler.id}};
}

Json schedulers_to_json(const std::vector<domain::SchedulerDescriptor>& schedulers) {
    auto body = Json::array();
    for (const auto& scheduler : schedulers) {
        body.push_back(scheduler_to_json(scheduler));
    }
    return body;
}

Json session_to_json(const domain::Session& session,
                     const std::optional<domain::StreamRuntimePlan>& stream_plan = std::nullopt) {
    auto body = Json{
        {"id", session.id},
        {"name", session.name},
        {"state", domain::to_string(session.state)},
        {"created_at", domain::format_timestamp_utc(session.created_at)},
        {"updated_at", domain::format_timestamp_utc(session.updated_at)},
        {"last_error", session.last_error ? Json(*session.last_error) : Json(nullptr)},
    };

    if (session.scheduler_alias.has_value()) {
        body["scheduler_id"] = *session.scheduler_alias;
    }

    if (stream_plan.has_value()) {
        body["streams"] = stream_plan_to_json(*stream_plan);
    }

    return body;
}

Json sessions_to_json(const std::vector<domain::Session>& sessions,
                      const std::vector<std::optional<domain::StreamRuntimePlan>>& stream_plans) {
    auto body = Json::array();
    for (std::size_t index = 0; index < sessions.size(); ++index) {
        const auto& plan = index < stream_plans.size() ? stream_plans[index] : std::optional<domain::StreamRuntimePlan>{};
        body.push_back(session_to_json(sessions[index], plan));
    }
    return body;
}

Json block_settings_update_to_json(const app::BlockSettingsUpdateResult& result) {
    return Json{
        {"session_id", result.session_id},
        {"block", result.block},
        {"applied_via", result.applied_via},
        {"accepted", result.accepted},
    };
}

WasmResponse json_response(const Json& body, const int status = 200) {
    return WasmResponse{status, "application/json", body.dump()};
}

WasmResponse error_response(const int status, const std::string& code, const std::string& message) {
    return json_response(Json{{"error", {{"code", code}, {"message", message}}}}, status);
}

WasmResponse no_content_response() {
    return WasmResponse{204, "", ""};
}

std::string decode_percent_encoded(std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch == '%' && index + 2 < value.size()) {
            const auto hex = value.substr(index + 1, 2);
            const auto byte = static_cast<char>(std::stoi(std::string(hex), nullptr, 16));
            decoded.push_back(byte);
            index += 2;
        } else if (ch == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(ch);
        }
    }
    return decoded;
}

// Query parameters of the dispatched request target, already percent-decoded.
class QueryParams {
public:
    explicit QueryParams(std::string_view query) {
        std::size_t start = 0;
        while (start <= query.size() && !query.empty()) {
            const auto end = query.find('&', start);
            const auto token = query.substr(start, end == std::string_view::npos ? query.size() - start : end - start);
            if (!token.empty()) {
                const auto separator = token.find('=');
                if (separator == std::string_view::npos) {
                    params_.emplace_back(decode_percent_encoded(token), std::string{});
                } else {
                    params_.emplace_back(decode_percent_encoded(token.substr(0, separator)),
                                         decode_percent_encoded(token.substr(separator + 1)));
                }
            }
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
    }

    std::string value(std::string_view name) const {
        for (const auto& [key, value] : params_) {
            if (key == name) {
                return value;
            }
        }
        return {};
    }

private:
    std::vector<std::pair<std::string, std::string>> params_;
};

Json parse_json_body(const std::string& body) {
    try {
        return Json::parse(body);
    } catch (const Json::parse_error&) {
        throw app::ValidationError("request body must be valid JSON");
    }
}

runtime::BlockSettingsMode parse_settings_mode(const QueryParams& params) {
    const auto mode = params.value("mode");
    if (mode.empty() || mode == "staged") {
        return runtime::BlockSettingsMode::Staged;
    }
    if (mode == "immediate") {
        return runtime::BlockSettingsMode::Immediate;
    }
    throw app::ValidationError("mode must be 'staged' or 'immediate'");
}

std::string get_required_grc(const Json& body) {
    const auto it = body.find("grc");
    if (it == body.end() || !it->is_string() || it->get_ref<const std::string&>().empty()) {
        throw app::ValidationError("grc must be a non-empty string");
    }
    return it->get<std::string>();
}

std::string get_optional_name(const Json& body) {
    const auto it = body.find("name");
    if (it == body.end() || it->is_null()) {
        return "";
    }
    if (!it->is_string()) {
        throw app::ValidationError("name must be a string");
    }
    return it->get<std::string>();
}

std::optional<std::string> get_optional_scheduler_id(const Json& body) {
    const auto it = body.find("scheduler_id");
    if (it == body.end() || it->is_null()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        throw app::ValidationError("scheduler_id must be a string");
    }
    auto scheduler_id = it->get<std::string>();
    if (scheduler_id.empty()) {
        return std::nullopt;
    }
    return scheduler_id;
}

WasmResponse current_error_response() {
    try {
        throw;
    } catch (const app::ValidationError& error) {
        return error_response(400, "validation_error", error.what());
    } catch (const app::NotFoundError& error) {
        return error_response(404, "not_found", error.what());
    } catch (const app::InvalidStateError& error) {
        return error_response(409, "invalid_state", error.what());
    } catch (const app::RuntimeError& error) {
        return error_response(500, "runtime_error", error.what());
    } catch (const app::TimeoutError& error) {
        return error_response(504, "timeout", error.what());
    } catch (const catalog::SchedulerCatalogLoadError& error) {
        return error_response(500, "scheduler_catalog_error", error.what());
    } catch (const catalog::CatalogLoadError& error) {
        return error_response(500, "catalog_error", error.what());
    } catch (const std::exception&) {
        return error_response(500, "internal_error", "internal server error");
    } catch (...) {
        return error_response(500, "internal_error", "internal server error");
    }
}

// Route patterns mirror the cpp-httplib table in http_server.cpp; keep the two in sync when
// adding or changing endpoints. Order matters: the more specific /sessions/... routes have to
// be tried before the bare /sessions/(id) ones.
const std::regex& block_id_route() {
    static const std::regex pattern{R"(/blocks/(.+))"};
    return pattern;
}

const std::regex& scheduler_id_route() {
    static const std::regex pattern{R"(/schedulers/(.+))"};
    return pattern;
}

const std::regex& session_http_stream_route() {
    static const std::regex pattern{R"(/sessions/([A-Za-z0-9_]+)/streams/(.+)/http)"};
    return pattern;
}

const std::regex& session_ws_stream_route() {
    static const std::regex pattern{R"(/sessions/([A-Za-z0-9_]+)/streams/(.+)/ws)"};
    return pattern;
}

const std::regex& session_settings_route() {
    static const std::regex pattern{R"(/sessions/([A-Za-z0-9_]+)/blocks/(.+)/settings)"};
    return pattern;
}

const std::regex& session_start_route() {
    static const std::regex pattern{R"(/sessions/([A-Za-z0-9_]+)/start)"};
    return pattern;
}

const std::regex& session_stop_route() {
    static const std::regex pattern{R"(/sessions/([A-Za-z0-9_]+)/stop)"};
    return pattern;
}

const std::regex& session_restart_route() {
    static const std::regex pattern{R"(/sessions/([A-Za-z0-9_]+)/restart)"};
    return pattern;
}

const std::regex& session_id_route() {
    static const std::regex pattern{R"(/sessions/([A-Za-z0-9_]+))"};
    return pattern;
}

}  // namespace

WasmApi::WasmApi(app::SessionService& session_service,
                 app::SessionStreamService& session_stream_service,
                 app::BlockCatalogService& block_catalog_service,
                 app::SchedulerCatalogService& scheduler_catalog_service,
                 app::BlockSettingsService& block_settings_service)
    : session_service_(session_service),
      session_stream_service_(session_stream_service),
      block_catalog_service_(block_catalog_service),
      scheduler_catalog_service_(scheduler_catalog_service),
      block_settings_service_(block_settings_service) {}

WasmResponse WasmApi::dispatch(const std::string& method, const std::string& target, const std::string& body) const {
    (void)session_stream_service_;

    const auto query_start = target.find('?');
    const auto path = query_start == std::string::npos ? target : target.substr(0, query_start);
    const QueryParams params(query_start == std::string::npos ? std::string_view{}
                                                              : std::string_view(target).substr(query_start + 1));

    try {
        std::smatch matches;

        if (method == "GET") {
            if (path == "/healthz") {
                return json_response(Json{{"ok", true}});
            }
            if (path == "/blocks") {
                return json_response(blocks_to_json(block_catalog_service_.list()));
            }
            if (std::regex_match(path, matches, block_id_route())) {
                return json_response(block_to_json(block_catalog_service_.get(decode_percent_encoded(matches[1].str()))));
            }
            if (path == "/schedulers") {
                return json_response(schedulers_to_json(scheduler_catalog_service_.list()));
            }
            if (std::regex_match(path, matches, scheduler_id_route())) {
                return json_response(
                    scheduler_to_json(scheduler_catalog_service_.get(decode_percent_encoded(matches[1].str()))));
            }
            if (path == "/sessions") {
                const auto sessions = session_service_.list();
                std::vector<std::optional<domain::StreamRuntimePlan>> stream_plans;
                stream_plans.reserve(sessions.size());
                for (const auto& session : sessions) {
                    stream_plans.push_back(session_service_.active_stream_plan(session.id));
                }
                return json_response(sessions_to_json(sessions, stream_plans));
            }
            if (std::regex_match(path, matches, session_http_stream_route())) {
                const auto stream_response =
                    session_service_.fetch_http_stream(matches[1].str(), decode_percent_encoded(matches[2].str()));
                return WasmResponse{stream_response.status, stream_response.content_type, stream_response.body};
            }
            if (std::regex_match(path, matches, session_ws_stream_route())) {
                const auto route = session_service_.resolve_websocket_stream(matches[1].str(),
                                                                             decode_percent_encoded(matches[2].str()));
                return json_response(Json{
                    {"transport", "wasm_in_process"},
                    {"host", route.internal.host},
                    {"port", route.internal.port},
                    {"path", route.internal.path},
                    {"endpoint", route.internal.endpoint},
                });
            }
            if (std::regex_match(path, matches, session_settings_route())) {
                return json_response(Json{{"settings",
                                           block_settings_service_.get(matches[1].str(),
                                                                       decode_percent_encoded(matches[2].str()))}});
            }
            if (std::regex_match(path, matches, session_id_route())) {
                const auto session = session_service_.get(matches[1].str());
                return json_response(session_to_json(session, session_service_.active_stream_plan(session.id)));
            }
        } else if (method == "POST") {
            if (path == "/sessions") {
                const auto request_body = parse_json_body(body);
                const auto session = session_service_.create(get_optional_name(request_body),
                                                             get_required_grc(request_body),
                                                             get_optional_scheduler_id(request_body));
                return json_response(session_to_json(session), 201);
            }
            if (std::regex_match(path, matches, session_start_route())) {
                return json_response(session_to_json(session_service_.start(matches[1].str())));
            }
            if (std::regex_match(path, matches, session_stop_route())) {
                return json_response(session_to_json(session_service_.stop(matches[1].str())));
            }
            if (std::regex_match(path, matches, session_restart_route())) {
                return json_response(session_to_json(session_service_.restart(matches[1].str())));
            }
            if (std::regex_match(path, matches, session_settings_route())) {
                const auto result = block_settings_service_.update(matches[1].str(),
                                                                   decode_percent_encoded(matches[2].str()),
                                                                   parse_json_body(body),
                                                                   parse_settings_mode(params));
                return json_response(block_settings_update_to_json(result));
            }
        } else if (method == "DELETE") {
            if (std::regex_match(path, matches, session_id_route())) {
                session_service_.remove(matches[1].str());
                return no_content_response();
            }
        }
    } catch (...) {
        return current_error_response();
    }

    return error_response(404, "not_found", "no route matches " + method + " " + path);
}

}  // namespace gr4cp::api
