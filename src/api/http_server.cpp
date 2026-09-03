#include "gr4cp/api/http_server.hpp"

#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace gr4cp::api {

namespace {

using Json = nlohmann::json;
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

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

Json session_to_json(const domain::Session& session, const std::optional<domain::StreamRuntimePlan>& stream_plan = std::nullopt) {
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

void set_json_response(httplib::Response& response, const Json& body, const int status = 200) {
    response.status = status;
    response.set_content(body.dump(), "application/json");
}

void set_error_response(httplib::Response& response,
                        const int status,
                        const std::string& code,
                        const std::string& message) {
    set_json_response(response, Json{{"error", {{"code", code}, {"message", message}}}}, status);
}

void set_no_content_response(httplib::Response& response) {
    response.status = 204;
    response.body.clear();
}

void set_http_stream_response(httplib::Response& response, const runtime::HttpStreamResponse& stream_response) {
    response.status = stream_response.status;
    response.set_content(stream_response.body, stream_response.content_type);
}

bool is_websocket_upgrade_request(const httplib::Request& request) {
    const auto upgrade = request.get_header_value("Upgrade");
    const auto connection = request.get_header_value("Connection");
    return upgrade == "websocket" &&
           (connection.find("Upgrade") != std::string::npos || connection.find("upgrade") != std::string::npos);
}

Json parse_json_body(const httplib::Request& request) {
    try {
        return Json::parse(request.body);
    } catch (const Json::parse_error&) {
        throw app::ValidationError("request body must be valid JSON");
    }
}

app::BlockSettingsService::Json parse_settings_body(const httplib::Request& request) {
    return parse_json_body(request);
}

runtime::BlockSettingsMode parse_settings_mode(const httplib::Request& request) {
    const auto mode = request.get_param_value("mode");
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

struct ErrorDetails {
    int status;
    std::string code;
    std::string message;
};

ErrorDetails current_error_details() {
    try {
        throw;
    } catch (const app::ValidationError& error) {
        return {400, "validation_error", error.what()};
    } catch (const app::NotFoundError& error) {
        return {404, "not_found", error.what()};
    } catch (const app::InvalidStateError& error) {
        return {409, "invalid_state", error.what()};
    } catch (const app::RuntimeError& error) {
        return {500, "runtime_error", error.what()};
    } catch (const app::TimeoutError& error) {
        return {504, "timeout", error.what()};
    } catch (const catalog::SchedulerCatalogLoadError& error) {
        return {500, "scheduler_catalog_error", error.what()};
    } catch (const catalog::CatalogLoadError& error) {
        return {500, "catalog_error", error.what()};
    } catch (const std::exception&) {
        return {500, "internal_error", "internal server error"};
    } catch (...) {
        return {500, "internal_error", "internal server error"};
    }
}

void handle_exception(httplib::Response& response) {
    const auto details = current_error_details();
    set_error_response(response, details.status, details.code, details.message);
}

template <typename Action>
void execute(httplib::Response& response, Action&& action) {
    try {
        std::forward<Action>(action)();
    } catch (...) {
        handle_exception(response);
    }
}

bool proxy_debug_enabled() {
    if (const char* env = std::getenv("GR4CP_PROXY_DEBUG"); env != nullptr) {
        return *env != '\0' && std::string_view(env) != "0";
    }
    return false;
}

void proxy_debug(std::string_view message) {
    if (!proxy_debug_enabled()) {
        return;
    }
    std::clog << "[gr4cp proxy] " << message << '\n';
}

std::string proxy_error_message(const httplib::Result& result) {
    if (result) {
        return "success";
    }
    return httplib::to_string(result.error());
}

}  // namespace

void register_routes(httplib::Server& server,
                     app::SessionService& session_service,
                     app::SessionStreamService&,
                     app::BlockCatalogService& block_catalog_service,
                     app::SchedulerCatalogService& scheduler_catalog_service,
                     app::BlockSettingsService& block_settings_service) {
    server.Get("/healthz", [](const httplib::Request&, httplib::Response& response) {
        set_json_response(response, Json{{"ok", true}});
    });

    server.Get("/blocks", [&block_catalog_service](const httplib::Request&, httplib::Response& response) {
        execute(response, [&]() {
            set_json_response(response, blocks_to_json(block_catalog_service.list()));
        });
    });

    server.Get(R"(/blocks/(.+))",
               [&block_catalog_service](const httplib::Request& request, httplib::Response& response) {
                   execute(response, [&]() {
                       set_json_response(response,
                                         block_to_json(block_catalog_service.get(
                                             decode_percent_encoded(request.matches[1].str()))));
                   });
               });

    server.Get("/schedulers", [&scheduler_catalog_service](const httplib::Request&, httplib::Response& response) {
        execute(response, [&]() {
            set_json_response(response, schedulers_to_json(scheduler_catalog_service.list()));
        });
    });

    server.Get(R"(/schedulers/(.+))",
               [&scheduler_catalog_service](const httplib::Request& request, httplib::Response& response) {
                   execute(response, [&]() {
                       set_json_response(response,
                                         scheduler_to_json(scheduler_catalog_service.get(
                                             decode_percent_encoded(request.matches[1].str()))));
                   });
               });

    server.Post("/sessions", [&session_service](const httplib::Request& request, httplib::Response& response) {
        execute(response, [&]() {
            const auto body = parse_json_body(request);
            const auto session = session_service.create(get_optional_name(body),
                                                        get_required_grc(body),
                                                        get_optional_scheduler_id(body));
            set_json_response(response, session_to_json(session), 201);
        });
    });

    server.Get("/sessions", [&session_service](const httplib::Request&, httplib::Response& response) {
        execute(response, [&]() {
            const auto sessions = session_service.list();
            std::vector<std::optional<domain::StreamRuntimePlan>> stream_plans;
            stream_plans.reserve(sessions.size());
            for (const auto& session : sessions) {
                stream_plans.push_back(session_service.active_stream_plan(session.id));
            }
            set_json_response(response, sessions_to_json(sessions, stream_plans));
        });
    });

    server.Get(R"(/sessions/([A-Za-z0-9_]+)/streams/(.+)/http)",
               [&session_service](const httplib::Request& request, httplib::Response& response) {
                   execute(response, [&]() {
                       set_http_stream_response(
                           response,
                           session_service.fetch_http_stream(request.matches[1].str(),
                                                             decode_percent_encoded(request.matches[2].str())));
                   });
               });

    server.Get(R"(/sessions/([A-Za-z0-9_]+)/streams/(.+)/ws)",
               [&session_service](const httplib::Request& request, httplib::Response& response) {
                   execute(response, [&]() {
                       (void)session_service.resolve_websocket_stream(request.matches[1].str(),
                                                                     decode_percent_encoded(request.matches[2].str()));
                       if (!is_websocket_upgrade_request(request)) {
                           response.status = 426;
                           response.set_header("Upgrade", "websocket");
                           response.set_header("Connection", "Upgrade");
                           response.set_content("", "text/plain");
                           return;
                       }
                       set_error_response(response,
                                          501,
                                          "not_implemented",
                                          "websocket proxy upgrade is not implemented on the current server stack");
                   });
               });

    server.Get(R"(/sessions/([A-Za-z0-9_]+))",
               [&session_service](const httplib::Request& request, httplib::Response& response) {
                   execute(response, [&]() {
                       const auto session = session_service.get(request.matches[1].str());
                       set_json_response(response, session_to_json(session, session_service.active_stream_plan(session.id)));
                   });
               });

    server.Delete(R"(/sessions/([A-Za-z0-9_]+))",
                  [&session_service](const httplib::Request& request, httplib::Response& response) {
                      execute(response, [&]() {
                          session_service.remove(request.matches[1].str());
                          set_no_content_response(response);
                      });
                  });

    server.Post(R"(/sessions/([A-Za-z0-9_]+)/start)",
                [&session_service](const httplib::Request& request, httplib::Response& response) {
                    execute(response, [&]() {
                        set_json_response(response,
                                          session_to_json(session_service.start(request.matches[1].str())));
                    });
                });

    server.Post(R"(/sessions/([A-Za-z0-9_]+)/stop)",
                [&session_service](const httplib::Request& request, httplib::Response& response) {
                    execute(response, [&]() {
                        set_json_response(response,
                                          session_to_json(session_service.stop(request.matches[1].str())));
                    });
                });

    server.Post(R"(/sessions/([A-Za-z0-9_]+)/restart)",
                [&session_service](const httplib::Request& request, httplib::Response& response) {
                    execute(response, [&]() {
                        set_json_response(response,
                                          session_to_json(session_service.restart(request.matches[1].str())));
                    });
                });

    server.Post(R"(/sessions/([A-Za-z0-9_]+)/blocks/(.+)/settings)",
                [&block_settings_service](const httplib::Request& request, httplib::Response& response) {
                    execute(response, [&]() {
                        const auto session_id = request.matches[1].str();
                        const auto block_unique_name = decode_percent_encoded(request.matches[2].str());
                        const auto result = block_settings_service.update(
                            session_id,
                            block_unique_name,
                            parse_settings_body(request),
                            parse_settings_mode(request));
                        set_json_response(response, block_settings_update_to_json(result));
                    });
                });

    server.Get(R"(/sessions/([A-Za-z0-9_]+)/blocks/(.+)/settings)",
               [&block_settings_service](const httplib::Request& request, httplib::Response& response) {
                   execute(response, [&]() {
                       const auto session_id = request.matches[1].str();
                       const auto block_unique_name = decode_percent_encoded(request.matches[2].str());
                       set_json_response(response,
                                         Json{{"settings", block_settings_service.get(session_id, block_unique_name)}});
                   });
               });
}

std::optional<std::pair<std::string, std::string>> parse_websocket_stream_target(std::string_view target) {
    const auto route = target.substr(0, target.find('?'));
    constexpr std::string_view prefix = "/sessions/";
    constexpr std::string_view infix = "/streams/";
    constexpr std::string_view suffix = "/ws";

    if (!route.starts_with(prefix) || !route.ends_with(suffix)) {
        return std::nullopt;
    }

    const auto infix_pos = route.find(infix, prefix.size());
    if (infix_pos == std::string_view::npos || infix_pos <= prefix.size()) {
        return std::nullopt;
    }

    const auto session_id = route.substr(prefix.size(), infix_pos - prefix.size());
    const auto stream_begin = infix_pos + infix.size();
    const auto stream_end = route.size() - suffix.size();
    if (stream_begin >= stream_end || session_id.find('/') != std::string_view::npos) {
        return std::nullopt;
    }

    return std::pair{std::string(session_id), decode_percent_encoded(route.substr(stream_begin, stream_end - stream_begin))};
}

httplib::Headers to_httplib_headers(const http::fields& fields) {
    httplib::Headers headers;
    for (const auto& field : fields) {
        const auto name = std::string(field.name_string());
        const auto header = field.name();
        if (header == http::field::host || header == http::field::connection || header == http::field::upgrade ||
            header == http::field::content_length || header == http::field::accept_encoding) {
            continue;
        }
        headers.emplace(name, std::string(field.value()));
    }
    return headers;
}

http::response<http::string_body> make_beast_response(const httplib::Response& upstream, const unsigned version) {
    http::response<http::string_body> response;
    response.version(version);
    response.result(static_cast<http::status>(upstream.status));
    response.body() = upstream.body;
    for (const auto& [name, value] : upstream.headers) {
        if (name == "Content-Length" || name == "Transfer-Encoding" || name == "Connection") {
            continue;
        }
        response.set(name, value);
    }
    response.keep_alive(false);
    response.prepare_payload();
    return response;
}

void write_response_and_close(tcp::socket& socket, http::response<http::string_body>& response) {
    beast::error_code error;
    http::write(socket, response, error);
    socket.shutdown(tcp::socket::shutdown_both, error);
    socket.close(error);
}

void write_json_error(tcp::socket& socket, const unsigned version, const ErrorDetails& details) {
    http::response<http::string_body> response{static_cast<http::status>(details.status), version};
    response.set(http::field::content_type, "application/json");
    response.body() = Json{{"error", {{"code", details.code}, {"message", details.message}}}}.dump();
    response.keep_alive(false);
    response.prepare_payload();
    write_response_and_close(socket, response);
}

std::optional<tcp::endpoint> endpoint_for_host(const std::string& host, const unsigned short port) {
    if (host == "0.0.0.0") {
        return tcp::endpoint{tcp::v4(), port};
    }
    if (host == "::") {
        return tcp::endpoint{tcp::v6(), port};
    }

    beast::error_code error;
    const auto address = asio::ip::make_address(host, error);
    if (error) {
        return std::nullopt;
    }
    return tcp::endpoint{address, port};
}

class WebSocketBridge : public std::enable_shared_from_this<WebSocketBridge> {
public:
    WebSocketBridge(websocket::stream<beast::tcp_stream>&& browser,
                    websocket::stream<beast::tcp_stream>&& internal)
        : browser_(std::move(browser)), internal_(std::move(internal)) {}

    void run() {
        auto self = shared_from_this();
        asio::post(browser_.get_executor(), [self]() { self->start(); });

        std::unique_lock lock(done_mutex_);
        done_condition_.wait(lock, [this]() { return done_; });
    }

    void close() {
        auto self = shared_from_this();
        asio::post(browser_.get_executor(), [self]() { self->begin_close(); });
    }

private:
    void start() {
        if (closing_) {
            finish_if_idle();
            return;
        }
        read_browser();
        read_internal();
    }

    void read_browser() {
        if (closing_) {
            return;
        }
        ++pending_operations_;
        browser_.async_read(browser_buffer_, [self = shared_from_this()](const beast::error_code& error, std::size_t) {
            --self->pending_operations_;
            if (error) {
                self->begin_close();
                self->finish_if_idle();
                return;
            }

            self->internal_.text(self->browser_.got_text());
            ++self->pending_operations_;
            self->internal_.async_write(
                self->browser_buffer_.data(),
                [self](const beast::error_code& write_error, std::size_t) {
                    --self->pending_operations_;
                    if (write_error) {
                        self->begin_close();
                        self->finish_if_idle();
                        return;
                    }
                    self->browser_buffer_.consume(self->browser_buffer_.size());
                    self->read_browser();
                });
        });
    }

    void read_internal() {
        if (closing_) {
            return;
        }
        ++pending_operations_;
        internal_.async_read(internal_buffer_, [self = shared_from_this()](const beast::error_code& error, std::size_t) {
            --self->pending_operations_;
            if (error) {
                self->begin_close();
                self->finish_if_idle();
                return;
            }

            self->browser_.text(self->internal_.got_text());
            ++self->pending_operations_;
            self->browser_.async_write(
                self->internal_buffer_.data(),
                [self](const beast::error_code& write_error, std::size_t) {
                    --self->pending_operations_;
                    if (write_error) {
                        self->begin_close();
                        self->finish_if_idle();
                        return;
                    }
                    self->internal_buffer_.consume(self->internal_buffer_.size());
                    self->read_internal();
                });
        });
    }

    void begin_close() {
        if (closing_) {
            finish_if_idle();
            return;
        }
        closing_ = true;

        beast::error_code error;
        browser_.next_layer().socket().shutdown(tcp::socket::shutdown_both, error);
        browser_.next_layer().socket().close(error);
        internal_.next_layer().socket().shutdown(tcp::socket::shutdown_both, error);
        internal_.next_layer().socket().close(error);
        finish_if_idle();
    }

    void finish_if_idle() {
        if (!closing_ || pending_operations_ != 0U || done_) {
            return;
        }
        {
            std::lock_guard lock(done_mutex_);
            done_ = true;
        }
        done_condition_.notify_all();
    }

    websocket::stream<beast::tcp_stream> browser_;
    websocket::stream<beast::tcp_stream> internal_;
    beast::flat_buffer browser_buffer_;
    beast::flat_buffer internal_buffer_;
    std::size_t pending_operations_{};
    bool closing_{};
    std::mutex done_mutex_;
    std::condition_variable done_condition_;
    bool done_{};
};

struct gr4cp::api::HttpServer::Impl {
    Impl(app::SessionService& session_service_ref,
         app::SessionStreamService& session_stream_service,
         app::BlockCatalogService& block_catalog_service,
         app::SchedulerCatalogService& scheduler_catalog_service,
         app::BlockSettingsService& block_settings_service)
        : session_service(session_service_ref) {
        register_routes(internal_server,
                        session_service_ref,
                        session_stream_service,
                        block_catalog_service,
                        scheduler_catalog_service,
                        block_settings_service);
    }

    ~Impl() {
        stop();
    }

    int bind_to_any_port(const std::string& host) {
        ensure_internal_bound();
        const auto endpoint = endpoint_for_host(host, 0);
        if (!endpoint.has_value()) {
            return -1;
        }

        beast::error_code error;
        acceptor_ = std::make_unique<tcp::acceptor>(io_context_);
        acceptor_->open(endpoint->protocol(), error);
        if (error) {
            return -1;
        }
        acceptor_->set_option(asio::socket_base::reuse_address(true), error);
        if (error) {
            return -1;
        }
        acceptor_->bind(*endpoint, error);
        if (error) {
            return -1;
        }
        acceptor_->listen(asio::socket_base::max_listen_connections, error);
        if (error) {
            return -1;
        }
        acceptor_->non_blocking(true, error);
        if (error) {
            return -1;
        }
        return static_cast<int>(acceptor_->local_endpoint().port());
    }

    bool listen(const std::string& host, const int port) {
        ensure_internal_bound();
        const auto endpoint = endpoint_for_host(host, static_cast<unsigned short>(port));
        if (!endpoint.has_value()) {
            return false;
        }

        beast::error_code error;
        acceptor_ = std::make_unique<tcp::acceptor>(io_context_);
        acceptor_->open(endpoint->protocol(), error);
        if (error) {
            return false;
        }
        acceptor_->set_option(asio::socket_base::reuse_address(true), error);
        if (error) {
            return false;
        }
        acceptor_->bind(*endpoint, error);
        if (error) {
            return false;
        }
        acceptor_->listen(asio::socket_base::max_listen_connections, error);
        if (error) {
            return false;
        }
        acceptor_->non_blocking(true, error);
        if (error) {
            return false;
        }
        return listen_after_bind();
    }

    bool listen_after_bind() {
        ensure_internal_bound();
        start_internal_server();
        start_websocket_io();
        stopping_.store(false);

        while (!stopping_.load()) {
            beast::error_code error;
            tcp::socket socket(asio::make_strand(io_context_));
            acceptor_->accept(socket, error);
            if (error) {
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                if (stopping_.load() || error == asio::error::operation_aborted || error == asio::error::bad_descriptor) {
                    break;
                }
                continue;
            }

            std::lock_guard lock(connection_mutex_);
            connection_threads_.emplace_back([this, socket = std::move(socket)]() mutable {
                try {
                    handle_connection(std::move(socket));
                } catch (const std::exception& exception) {
                    std::cerr << "gr4cp_server: unhandled exception in HTTP connection worker: " << exception.what() << '\n';
                } catch (...) {
                    std::cerr << "gr4cp_server: unknown exception in HTTP connection worker\n";
                }
            });
        }

        cleanup_threads();
        return true;
    }

    void stop() {
        if (stopping_.exchange(true)) {
            return;
        }

        internal_server.stop();

        if (acceptor_) {
            beast::error_code error;
            acceptor_->cancel(error);
            acceptor_->close(error);
        }

        std::vector<std::shared_ptr<WebSocketBridge>> bridges;
        {
            std::lock_guard lock(bridge_mutex_);
            bridges.assign(active_bridges_.begin(), active_bridges_.end());
        }
        for (const auto& bridge : bridges) {
            bridge->close();
        }

        cleanup_threads();
    }

    app::SessionService& session_service;
    httplib::Server internal_server;
    int internal_port_{0};

private:
    void ensure_internal_bound() {
        if (internal_port_ == 0) {
            internal_port_ = internal_server.bind_to_any_port("127.0.0.1");
        }
    }

    void start_internal_server() {
        if (!internal_thread_.joinable()) {
            internal_thread_ = std::jthread([this]() { internal_server.listen_after_bind(); });
            wait_for_internal_server_ready();
        }
    }

    void start_websocket_io() {
        io_context_.restart();
        websocket_work_guard_.emplace(io_context_.get_executor());
        websocket_io_thread_ = std::jthread([this]() { io_context_.run(); });
    }

    void wait_for_internal_server_ready() {
        httplib::Client client("127.0.0.1", internal_port_);
        client.set_connection_timeout(0, 200000);
        client.set_read_timeout(0, 200000);
        for (int attempt = 0; attempt < 50; ++attempt) {
            const auto response = client.Get("/healthz");
            if (response && response->status == 200) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void stop_internal_server() {
        if (internal_thread_.joinable()) {
            internal_thread_.join();
        }
    }

    void stop_websocket_io() {
        websocket_work_guard_.reset();
        if (websocket_io_thread_.joinable()) {
            websocket_io_thread_.join();
        }
    }

    void cleanup_threads() {
        std::lock_guard lock(cleanup_mutex_);
        join_connection_threads();
        stop_internal_server();
        stop_websocket_io();
    }

    void join_connection_threads() {
        std::vector<std::jthread> threads;
        {
            std::lock_guard lock(connection_mutex_);
            threads.swap(connection_threads_);
        }
        threads.clear();
    }

    void handle_connection(tcp::socket socket) {
        beast::flat_buffer buffer;
        http::request<http::string_body> request;
        beast::error_code error;
        http::read(socket, buffer, request, error);
        if (error) {
            return;
        }

        const auto websocket_route = websocket::is_upgrade(request) ? parse_websocket_stream_target(request.target()) : std::nullopt;
        if (websocket_route.has_value()) {
            handle_websocket_upgrade(std::move(socket), std::move(request), websocket_route->first, websocket_route->second);
            return;
        }

        proxy_http_request(std::move(socket), request);
    }

    void proxy_http_request(tcp::socket socket, const http::request<http::string_body>& request) {
        static constexpr auto kInternalRouteReadTimeout = std::chrono::seconds(45);

        httplib::Client client("127.0.0.1", internal_port_);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(kInternalRouteReadTimeout.count(), 0);
        client.set_keep_alive(false);
        // The public Beast endpoint owns content negotiation. If cpp-httplib
        // advertises compression for this internal hop, it transparently
        // decompresses the body but leaves Content-Encoding on the response.
        // Forwarding that stale header makes browsers decode the JSON twice.
        client.set_decompress(false);
        auto headers = to_httplib_headers(request.base());
        headers.emplace("Accept-Encoding", "identity");
        const auto path = std::string(request.target());
        proxy_debug("public_path=" + path + " internal_route=http://127.0.0.1:" + std::to_string(internal_port_) + path);

        httplib::Result upstream;
        switch (request.method()) {
            case http::verb::get:
                upstream = client.Get(path, headers);
                break;
            case http::verb::post: {
                auto content_type = std::string(request[http::field::content_type]);
                if (content_type.empty()) {
                    content_type = "application/json";
                }
                upstream = client.Post(path, headers, request.body(), std::string(content_type));
                break;
            }
            case http::verb::delete_:
                upstream = client.Delete(path, headers);
                break;
            default: {
                http::response<http::string_body> response{http::status::method_not_allowed, request.version()};
                response.set(http::field::content_type, "text/plain");
                response.body() = "method not allowed";
                response.keep_alive(false);
                response.prepare_payload();
                write_response_and_close(socket, response);
                return;
            }
        }

        if (!upstream) {
            const auto error = proxy_error_message(upstream);
            proxy_debug("internal_route_failed path=" + path + " error=" + error);
            http::response<http::string_body> response{http::status::bad_gateway, request.version()};
            response.set(http::field::content_type, "application/json");
            response.body() =
                Json{{"error", {{"code", "bad_gateway"}, {"message", "failed to reach internal HTTP route: " + error}}}}.dump();
            response.keep_alive(false);
            response.prepare_payload();
            write_response_and_close(socket, response);
            return;
        }

        proxy_debug("internal_route_status path=" + path + " status=" + std::to_string(upstream->status));

        auto response = make_beast_response(*upstream, request.version());
        write_response_and_close(socket, response);
    }

    void handle_websocket_upgrade(tcp::socket socket,
                                  http::request<http::string_body> request,
                                  const std::string& session_id,
                                  const std::string& stream_id) {
        runtime::WebSocketStreamRoute route;
        try {
            route = session_service.resolve_websocket_stream(session_id, stream_id);
        } catch (...) {
            write_json_error(socket, request.version(), current_error_details());
            return;
        }

        websocket::stream<beast::tcp_stream> internal(socket.get_executor());
        try {
            tcp::resolver resolver(io_context_);
            const auto endpoint = resolver.resolve(route.internal.host, std::to_string(route.internal.port));
            beast::get_lowest_layer(internal).connect(endpoint);
            internal.handshake(route.internal.host + ":" + std::to_string(route.internal.port), route.internal.path);
        } catch (const std::exception& error) {
            write_json_error(socket,
                             request.version(),
                             ErrorDetails{500, "runtime_error", std::string("failed to attach internal websocket stream: ") + error.what()});
            return;
        }

        websocket::stream<beast::tcp_stream> browser(std::move(socket));
        try {
            browser.accept(request);
        } catch (...) {
            beast::error_code error;
            internal.next_layer().socket().shutdown(tcp::socket::shutdown_both, error);
            internal.next_layer().socket().close(error);
            return;
        }

        auto bridge = std::make_shared<WebSocketBridge>(std::move(browser), std::move(internal));
        {
            std::lock_guard lock(bridge_mutex_);
            active_bridges_.insert(bridge);
        }
        bridge->run();
        {
            std::lock_guard lock(bridge_mutex_);
            active_bridges_.erase(bridge);
        }
    }

    asio::io_context io_context_{1};
    std::unique_ptr<tcp::acceptor> acceptor_;
    std::atomic<bool> stopping_{true};
    std::jthread internal_thread_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> websocket_work_guard_;
    std::jthread websocket_io_thread_;
    std::mutex connection_mutex_;
    std::vector<std::jthread> connection_threads_;
    std::mutex bridge_mutex_;
    std::set<std::shared_ptr<WebSocketBridge>> active_bridges_;
    std::mutex cleanup_mutex_;
};

gr4cp::api::HttpServer::HttpServer(app::SessionService& session_service,
                                   app::SessionStreamService& session_stream_service,
                                   app::BlockCatalogService& block_catalog_service,
                                   app::SchedulerCatalogService& scheduler_catalog_service,
                                   app::BlockSettingsService& block_settings_service)
    : impl_(std::make_unique<Impl>(session_service,
                                   session_stream_service,
                                   block_catalog_service,
                                   scheduler_catalog_service,
                                   block_settings_service)) {}

gr4cp::api::HttpServer::~HttpServer() = default;

int gr4cp::api::HttpServer::bind_to_any_port(const std::string& host) {
    return impl_->bind_to_any_port(host);
}

bool gr4cp::api::HttpServer::listen_after_bind() {
    return impl_->listen_after_bind();
}

bool gr4cp::api::HttpServer::listen(const std::string& host, const int port) {
    return impl_->listen(host, port);
}

void gr4cp::api::HttpServer::stop() {
    impl_->stop();
}

}  // namespace gr4cp::api
