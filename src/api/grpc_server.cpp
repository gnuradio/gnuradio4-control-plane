#include "gr4cp/api/grpc_server.hpp"

#include "session_json.hpp"

#include <chrono>
#include <complex>
#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <google/protobuf/empty.pb.h>
#include <google/protobuf/map.h>
#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <nlohmann/json.hpp>

#include "gr4cp/v1/control_plane.grpc.pb.h"

namespace gr4cp::api {

namespace {

using Json = nlohmann::json;
using SettingsMap = google::protobuf::Map<std::string, v1::Value>;

template <typename T> struct is_tensor : std::false_type {};

template <typename T> struct is_tensor<gr::Tensor<T>> : std::true_type {};

template <typename T> struct is_complex : std::false_type {};

template <typename T> struct is_complex<std::complex<T>> : std::true_type {};

// The trailing metadata key that carries the REST API's error code for a failed
// call.
constexpr char kErrorCodeKey[] = "gr4cp-error-code";

struct ErrorDetails {
  grpc::StatusCode status;
  std::string code;
  std::string message;
};

ErrorDetails current_error_details() {
  try {
    throw;
  } catch (const app::ValidationError &error) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "validation_error",
            error.what()};
  } catch (const app::NotFoundError &error) {
    return {grpc::StatusCode::NOT_FOUND, "not_found", error.what()};
  } catch (const app::InvalidStateError &error) {
    return {grpc::StatusCode::FAILED_PRECONDITION, "invalid_state",
            error.what()};
  } catch (const app::RuntimeError &error) {
    return {grpc::StatusCode::INTERNAL, "runtime_error", error.what()};
  } catch (const catalog::SchedulerCatalogLoadError &error) {
    return {grpc::StatusCode::INTERNAL, "scheduler_catalog_error",
            error.what()};
  } catch (const catalog::CatalogLoadError &error) {
    return {grpc::StatusCode::INTERNAL, "catalog_error", error.what()};
  } catch (...) {
    return {grpc::StatusCode::INTERNAL, "internal_error",
            "internal server error"};
  }
}

// Thrown between the steps of a call whose client cancelled it or whose
// deadline passed.
struct CallCancelled {};

void throw_if_cancelled(const grpc::ServerContext &context) {
  if (context.IsCancelled()) {
    throw CallCancelled{};
  }
}

// Runs one call. A call that was cancelled, or whose deadline passed, before
// the server reached it does no work. One that is already in an application
// service runs to its end, since the services have no way to abandon a
// lifecycle step midway, and the client still gets CANCELLED or
// DEADLINE_EXCEEDED at once.
template <typename Action>
grpc::Status execute(grpc::ServerContext *context, Action &&action) {
  try {
    throw_if_cancelled(*context);
    std::forward<Action>(action)();
    return grpc::Status::OK;
  } catch (const CallCancelled &) {
    return {grpc::StatusCode::CANCELLED, "call cancelled"};
  } catch (...) {
    const auto details = current_error_details();
    context->AddTrailingMetadata(kErrorCodeKey, details.code);
    return {details.status, details.message};
  }
}

void set_timestamp(const domain::Timestamp &timestamp,
                   google::protobuf::Timestamp &out) {
  const auto since_epoch = timestamp.time_since_epoch();
  const auto seconds = std::chrono::floor<std::chrono::seconds>(since_epoch);
  out.set_seconds(seconds.count());
  out.set_nanos(static_cast<std::int32_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch -
                                                           seconds)
          .count()));
}

v1::SessionState to_proto(const domain::SessionState state) {
  switch (state) {
  case domain::SessionState::Stopped:
    return v1::SESSION_STATE_STOPPED;
  case domain::SessionState::Running:
    return v1::SESSION_STATE_RUNNING;
  case domain::SessionState::Error:
    return v1::SESSION_STATE_ERROR;
  }
  return v1::SESSION_STATE_UNSPECIFIED;
}

v1::CompletionReason to_proto(const domain::CompletionReason reason) {
  switch (reason) {
  case domain::CompletionReason::Completed:
    return v1::COMPLETION_REASON_COMPLETED;
  case domain::CompletionReason::Stopped:
    return v1::COMPLETION_REASON_STOPPED;
  case domain::CompletionReason::Error:
    return v1::COMPLETION_REASON_ERROR;
  }
  return v1::COMPLETION_REASON_UNSPECIFIED;
}

void set_session(const domain::Session &session,
                 const std::optional<domain::StreamRuntimePlan> &stream_plan,
                 v1::Session &out) {
  out.set_id(session.id);
  out.set_server_instance_id(server_instance_id());
  out.set_name(session.name);
  out.set_state(to_proto(session.state));
  set_timestamp(session.created_at, *out.mutable_created_at());
  set_timestamp(session.updated_at, *out.mutable_updated_at());
  if (session.last_error.has_value()) {
    out.set_last_error(*session.last_error);
  }
  if (session.scheduler_alias.has_value()) {
    out.set_scheduler_id(*session.scheduler_alias);
  }
  if (session.execution_id.has_value()) {
    out.set_execution_id(*session.execution_id);
  }
  if (session.completion_reason.has_value()) {
    out.set_completion_reason(to_proto(*session.completion_reason));
  }
  if (stream_plan.has_value()) {
    for (const auto &stream_plan_entry : stream_plan->streams) {
      const auto stream =
          domain::to_browser_stream_descriptor(stream_plan_entry);
      auto &item = *out.add_streams();
      item.set_id(stream.id);
      item.set_block_instance_name(stream.block_instance_name);
      item.set_transport(stream.transport);
      item.set_payload_format(stream.payload_format);
      item.set_path(stream.path);
      item.set_ready(stream.ready);
    }
  }
}

// Converts a request value to the JSON the settings service takes. An integer
// stays an integer of its signedness. A value with no kind has no REST form, so
// it is rejected.
Json value_to_json(const v1::Value &value) {
  switch (value.kind_case()) {
  case v1::Value::kBoolValue:
    return value.bool_value();
  case v1::Value::kIntValue:
    return value.int_value();
  case v1::Value::kUintValue:
    return value.uint_value();
  case v1::Value::kDoubleValue:
    return value.double_value();
  case v1::Value::kStringValue:
    return value.string_value();
  case v1::Value::kListValue: {
    auto result = Json::array();
    for (const auto &element : value.list_value().values()) {
      result.push_back(value_to_json(element));
    }
    return result;
  }
  case v1::Value::kMapValue: {
    auto result = Json::object();
    for (const auto &[key, field] : value.map_value().fields()) {
      result[key] = value_to_json(field);
    }
    return result;
  }
  case v1::Value::kNullValue:
    return nullptr;
  case v1::Value::KIND_NOT_SET:
    break;
  }
  throw app::ValidationError("setting value must have a kind");
}

Json settings_to_json(const SettingsMap &settings) {
  auto result = Json::object();
  for (const auto &[key, value] : settings) {
    try {
      result[key] = value_to_json(value);
    } catch (const app::ValidationError &error) {
      throw app::ValidationError("setting " + key + ": " + error.what());
    }
  }
  return result;
}

void set_value(const gr::pmt::Value &value, v1::Value &out);

void set_settings(const gr::property_map &settings, SettingsMap &out) {
  for (const auto &[key, item] : settings) {
    set_value(item, out[std::string(key)]);
  }
}

// Converts a scalar, a complex number or a string. A complex number becomes a
// map of its two parts.
template <typename T> void set_scalar(const T &item, v1::Value &out) {
  if constexpr (std::same_as<T, std::monostate>) {
    out.set_null_value(google::protobuf::NULL_VALUE);
  } else if constexpr (std::same_as<T, bool>) {
    out.set_bool_value(item);
  } else if constexpr (is_complex<T>::value) {
    auto &fields = *out.mutable_map_value()->mutable_fields();
    fields["real"].set_double_value(static_cast<double>(item.real()));
    fields["imag"].set_double_value(static_cast<double>(item.imag()));
  } else if constexpr (std::floating_point<T>) {
    out.set_double_value(static_cast<double>(item));
  } else if constexpr (std::signed_integral<T>) {
    out.set_int_value(static_cast<std::int64_t>(item));
  } else if constexpr (std::unsigned_integral<T>) {
    out.set_uint_value(static_cast<std::uint64_t>(item));
  } else {
    out.set_string_value(std::string(item));
  }
}

void set_value(const gr::pmt::Value &value, v1::Value &out) {
  gr::pmt::ValueVisitor([&out](const auto &item) {
    using T = std::decay_t<decltype(item)>;
    if constexpr (is_tensor<T>::value) {
      auto &list = *out.mutable_list_value();
      for (const auto &element : item) {
        if constexpr (std::same_as<typename T::value_type, gr::pmt::Value>) {
          set_value(element, *list.add_values());
        } else {
          set_scalar(static_cast<typename T::value_type>(element),
                     *list.add_values());
        }
      }
    } else if constexpr (std::same_as<T, gr::property_map>) {
      set_settings(item, *out.mutable_map_value()->mutable_fields());
    } else {
      set_scalar(item, out);
    }
  }).visit(value);
}

runtime::BlockSettingsMode to_settings_mode(const v1::BlockSettingsMode mode) {
  switch (mode) {
  case v1::BLOCK_SETTINGS_MODE_UNSPECIFIED:
  case v1::BLOCK_SETTINGS_MODE_STAGED:
    return runtime::BlockSettingsMode::Staged;
  case v1::BLOCK_SETTINGS_MODE_IMMEDIATE:
    return runtime::BlockSettingsMode::Immediate;
  default:
    break;
  }
  throw app::ValidationError("mode must be staged or immediate");
}

void set_port(const domain::BlockPortDescriptor &port, v1::BlockPort &out) {
  out.set_name(port.name);
  out.set_type(port.type);
  out.set_cardinality_kind(port.cardinality_kind ==
                                   domain::BlockPortCardinalityKind::Dynamic
                               ? v1::BLOCK_PORT_CARDINALITY_KIND_DYNAMIC
                               : v1::BLOCK_PORT_CARDINALITY_KIND_FIXED);
  if (port.current_port_count.has_value()) {
    out.set_current_port_count(*port.current_port_count);
  }
  if (port.render_port_count.has_value()) {
    out.set_render_port_count(*port.render_port_count);
  }
  if (port.min_port_count.has_value()) {
    out.set_min_port_count(*port.min_port_count);
  }
  if (port.max_port_count.has_value()) {
    out.set_max_port_count(*port.max_port_count);
  }
  if (port.size_parameter.has_value()) {
    out.set_size_parameter(*port.size_parameter);
  }
  if (port.handle_name_template.has_value()) {
    out.set_handle_name_template(*port.handle_name_template);
  }
}

void set_parameter(const domain::BlockParameterDescriptor &parameter,
                   v1::BlockParameter &out) {
  out.set_name(parameter.name);
  out.set_type(parameter.type);
  out.set_required(parameter.required);
  std::visit(
      [&out](const auto &item) {
        auto &value = *out.mutable_default_value();
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, std::nullptr_t>) {
          value.set_null_value(google::protobuf::NULL_VALUE);
        } else {
          set_scalar(item, value);
        }
      },
      parameter.default_value);
  out.set_summary(parameter.summary);
  if (parameter.runtime_mutability.has_value()) {
    out.set_runtime_mutability(*parameter.runtime_mutability);
  }
  if (parameter.value_kind.has_value()) {
    out.set_value_kind(*parameter.value_kind);
  }
  if (parameter.enum_choices.has_value()) {
    auto &choices = *out.mutable_enum_choices();
    for (const auto &choice : *parameter.enum_choices) {
      choices.add_values(choice);
    }
  }
  if (parameter.enum_type.has_value()) {
    out.set_enum_type(*parameter.enum_type);
  }
  for (const auto &[value, label] : parameter.enum_labels) {
    (*out.mutable_enum_labels())[value] = label;
  }
  if (parameter.enum_source.has_value()) {
    out.set_enum_source(*parameter.enum_source);
  }
  if (parameter.ui_hint.has_value()) {
    out.set_ui_hint(*parameter.ui_hint);
  }
  if (parameter.allow_custom_value.has_value()) {
    out.set_allow_custom_value(*parameter.allow_custom_value);
  }
}

void set_block(const domain::BlockDescriptor &block, v1::Block &out) {
  out.set_id(block.id);
  out.set_name(block.name);
  out.set_category(block.category);
  out.set_summary(block.summary);
  for (const auto &input : block.inputs) {
    set_port(input, *out.add_inputs());
  }
  for (const auto &output : block.outputs) {
    set_port(output, *out.add_outputs());
  }
  for (const auto &parameter : block.parameters) {
    set_parameter(parameter, *out.add_parameters());
  }
}

class ServerRpc final : public v1::ServerService::Service {
public:
  grpc::Status GetHealth(grpc::ServerContext *context,
                         const v1::GetHealthRequest *,
                         v1::GetHealthResponse *response) override {
    return execute(context, [&]() {
      response->set_ok(true);
      response->set_server_instance_id(server_instance_id());
    });
  }

  grpc::Status GetVersion(grpc::ServerContext *context,
                          const v1::GetVersionRequest *,
                          v1::GetVersionResponse *response) override {
    return execute(context, [&]() {
      response->set_version(server_version());
      response->set_server_instance_id(server_instance_id());
    });
  }
};

class SessionRpc final : public v1::SessionService::Service {
public:
  explicit SessionRpc(app::SessionService &sessions) : sessions_(sessions) {}

  grpc::Status CreateSession(grpc::ServerContext *context,
                             const v1::CreateSessionRequest *request,
                             v1::Session *response) override {
    return execute(context, [&]() {
      std::optional<std::string> scheduler_id;
      if (request->has_scheduler_id() && !request->scheduler_id().empty()) {
        scheduler_id = request->scheduler_id();
      }
      set_session(sessions_.create(request->name(), request->grc(),
                                   std::move(scheduler_id)),
                  std::nullopt, *response);
    });
  }

  grpc::Status ListSessions(grpc::ServerContext *context,
                            const v1::ListSessionsRequest *,
                            v1::ListSessionsResponse *response) override {
    return execute(context, [&]() {
      for (const auto &session : sessions_.list()) {
        // Reading a running session's streams observes its run, which may wait.
        throw_if_cancelled(*context);
        set_session(session, sessions_.active_stream_plan(session.id),
                    *response->add_sessions());
      }
    });
  }

  grpc::Status GetSession(grpc::ServerContext *context,
                          const v1::GetSessionRequest *request,
                          v1::Session *response) override {
    return execute(context, [&]() {
      const auto session = sessions_.get(request->session_id());
      set_session(session, sessions_.active_stream_plan(session.id), *response);
    });
  }

  grpc::Status DeleteSession(grpc::ServerContext *context,
                             const v1::DeleteSessionRequest *request,
                             google::protobuf::Empty *) override {
    return execute(context, [&]() { sessions_.remove(request->session_id()); });
  }

  grpc::Status StartSession(grpc::ServerContext *context,
                            const v1::StartSessionRequest *request,
                            v1::Session *response) override {
    return execute(context, [&]() {
      set_session(sessions_.start(request->session_id()), std::nullopt,
                  *response);
    });
  }

  grpc::Status StopSession(grpc::ServerContext *context,
                           const v1::StopSessionRequest *request,
                           v1::Session *response) override {
    return execute(context, [&]() {
      set_session(sessions_.stop(request->session_id()), std::nullopt,
                  *response);
    });
  }

  grpc::Status RestartSession(grpc::ServerContext *context,
                              const v1::RestartSessionRequest *request,
                              v1::Session *response) override {
    return execute(context, [&]() {
      set_session(sessions_.restart(request->session_id()), std::nullopt,
                  *response);
    });
  }

private:
  app::SessionService &sessions_;
};

class BlockSettingsRpc final : public v1::BlockSettingsService::Service {
public:
  explicit BlockSettingsRpc(app::BlockSettingsService &settings)
      : settings_(settings) {}

  grpc::Status
  GetBlockSettings(grpc::ServerContext *context,
                   const v1::GetBlockSettingsRequest *request,
                   v1::GetBlockSettingsResponse *response) override {
    return execute(context, [&]() {
      set_settings(settings_.get_properties(request->session_id(),
                                            request->block_unique_name()),
                   *response->mutable_settings());
    });
  }

  grpc::Status
  UpdateBlockSettings(grpc::ServerContext *context,
                      const v1::UpdateBlockSettingsRequest *request,
                      v1::UpdateBlockSettingsResponse *response) override {
    return execute(context, [&]() {
      const auto result =
          settings_.update(request->session_id(), request->block_unique_name(),
                           settings_to_json(request->settings()),
                           to_settings_mode(request->mode()));
      response->set_session_id(result.session_id);
      response->set_block(result.block);
      response->set_applied_via(result.applied_via);
      response->set_accepted(result.accepted);
    });
  }

private:
  app::BlockSettingsService &settings_;
};

class CatalogRpc final : public v1::CatalogService::Service {
public:
  CatalogRpc(app::BlockCatalogService &blocks,
             app::SchedulerCatalogService &schedulers)
      : blocks_(blocks), schedulers_(schedulers) {}

  grpc::Status ListBlocks(grpc::ServerContext *context,
                          const v1::ListBlocksRequest *,
                          v1::ListBlocksResponse *response) override {
    return execute(context, [&]() {
      for (const auto &block : blocks_.list()) {
        set_block(block, *response->add_blocks());
      }
    });
  }

  grpc::Status GetBlock(grpc::ServerContext *context,
                        const v1::GetBlockRequest *request,
                        v1::Block *response) override {
    return execute(context, [&]() {
      set_block(blocks_.get(request->block_id()), *response);
    });
  }

  grpc::Status ListSchedulers(grpc::ServerContext *context,
                              const v1::ListSchedulersRequest *,
                              v1::ListSchedulersResponse *response) override {
    return execute(context, [&]() {
      for (const auto &scheduler : schedulers_.list()) {
        response->add_schedulers()->set_id(scheduler.id);
      }
    });
  }

  grpc::Status GetScheduler(grpc::ServerContext *context,
                            const v1::GetSchedulerRequest *request,
                            v1::Scheduler *response) override {
    return execute(context, [&]() {
      response->set_id(schedulers_.get(request->scheduler_id()).id);
    });
  }

private:
  app::BlockCatalogService &blocks_;
  app::SchedulerCatalogService &schedulers_;
};

} // namespace

struct GrpcServer::Impl {
  Impl(app::SessionService &session_service,
       app::BlockCatalogService &block_catalog_service,
       app::SchedulerCatalogService &scheduler_catalog_service,
       app::BlockSettingsService &block_settings_service)
      : sessions(session_service), settings(block_settings_service),
        catalog(block_catalog_service, scheduler_catalog_service) {}

  ServerRpc server_info;
  SessionRpc sessions;
  BlockSettingsRpc settings;
  CatalogRpc catalog;
  std::mutex mutex;
  std::unique_ptr<grpc::Server> server;
};

GrpcServer::GrpcServer(app::SessionService &session_service,
                       app::BlockCatalogService &block_catalog_service,
                       app::SchedulerCatalogService &scheduler_catalog_service,
                       app::BlockSettingsService &block_settings_service)
    : impl_(std::make_unique<Impl>(session_service, block_catalog_service,
                                   scheduler_catalog_service,
                                   block_settings_service)) {}

GrpcServer::~GrpcServer() { stop(); }

int GrpcServer::start(const std::string &host, const int port) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->server) {
    return 0;
  }

  grpc::EnableDefaultHealthCheckService(true);

  // An IPv6 literal takes brackets in a gRPC address.
  const auto address =
      (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" +
      std::to_string(port);
  int selected_port = 0;
  grpc::ServerBuilder builder;
  // gRPC sets SO_REUSEPORT by default, which lets a second server bind a port
  // that is already served.
  builder.AddChannelArgument(GRPC_ARG_ALLOW_REUSEPORT, 0);
  builder.AddListeningPort(address, grpc::InsecureServerCredentials(),
                           &selected_port);
  builder.RegisterService(&impl_->server_info);
  builder.RegisterService(&impl_->sessions);
  builder.RegisterService(&impl_->settings);
  builder.RegisterService(&impl_->catalog);
  auto server = builder.BuildAndStart();
  if (!server || selected_port == 0) {
    if (server) {
      server->Shutdown();
    }
    return 0;
  }
  impl_->server = std::move(server);
  return selected_port;
}

void GrpcServer::stop() {
  std::unique_ptr<grpc::Server> server;
  {
    std::lock_guard lock(impl_->mutex);
    server = std::move(impl_->server);
  }
  if (server) {
    // Every RPC is unary. The deadline bounds a call that is still in an
    // application service.
    server->Shutdown(std::chrono::system_clock::now() +
                     std::chrono::seconds(5));
    server->Wait();
  }
}

} // namespace gr4cp::api
