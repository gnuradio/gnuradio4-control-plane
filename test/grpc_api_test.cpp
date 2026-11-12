#include "gr4cp/api/grpc_server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/scheduler_catalog_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/catalog/block_catalog_provider.hpp"
#include "gr4cp/catalog/scheduler_catalog_provider.hpp"
#include "gr4cp/runtime/stub_runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"
#include "gr4cp/v1/control_plane.grpc.pb.h"

namespace {

namespace v1 = gr4cp::v1;

// Holds the calls that pass through it while it is closed.
class Gate {
public:
  void close() {
    std::lock_guard lock(mutex_);
    closed_ = true;
  }

  void open() {
    {
      std::lock_guard lock(mutex_);
      closed_ = false;
    }
    changed_.notify_all();
  }

  void pass() {
    std::unique_lock lock(mutex_);
    if (!closed_) {
      return;
    }
    ++waiting_;
    changed_.notify_all();
    changed_.wait(lock, [this]() { return !closed_; });
    --waiting_;
  }

  // Waits until a call is held here. Returns false when none arrives in time.
  bool wait_for_held_call() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::seconds(5),
                             [this]() { return waiting_ > 0; });
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool closed_{false};
  int waiting_{0};
};

// The stub runtime, with a start and a stream plan read that a test can hold or
// fail.
class ControlledRuntimeManager final : public gr4cp::runtime::RuntimeManager {
public:
  void prepare(const gr4cp::domain::Session &session) override {
    stub_.prepare(session);
  }
  void start(const gr4cp::domain::Session &session) override {
    if (fail_start) {
      throw std::runtime_error("start refused");
    }
    start_gate.pass();
    stub_.start(session);
  }
  std::optional<std::string>
  stop(const gr4cp::domain::Session &session) override {
    return stub_.stop(session);
  }
  void destroy(const gr4cp::domain::Session &session) override {
    stub_.destroy(session);
  }
  std::optional<gr4cp::domain::StreamRuntimePlan>
  active_stream_plan(const gr4cp::domain::Session &session) override {
    ++plan_reads;
    plan_gate.pass();
    return stub_.active_stream_plan(session);
  }
  gr4cp::runtime::HttpStreamResponse
  fetch_http_stream(const gr4cp::domain::Session &session,
                    const std::string &stream_id) override {
    return stub_.fetch_http_stream(session, stream_id);
  }
  gr4cp::runtime::WebSocketStreamRoute
  resolve_websocket_stream(const gr4cp::domain::Session &session,
                           const std::string &stream_id) override {
    return stub_.resolve_websocket_stream(session, stream_id);
  }
  void set_block_settings(const gr4cp::domain::Session &session,
                          const std::string &block_unique_name,
                          const gr::property_map &patch,
                          gr4cp::runtime::BlockSettingsMode mode) override {
    stub_.set_block_settings(session, block_unique_name, patch, mode);
  }
  gr::property_map
  get_block_settings(const gr4cp::domain::Session &session,
                     const std::string &block_unique_name) override {
    return stub_.get_block_settings(session, block_unique_name);
  }
  gr4cp::runtime::RunObservation
  observe(const gr4cp::domain::Session &session) override {
    return stub_.observe(session);
  }

  Gate start_gate;
  Gate plan_gate;
  std::atomic<bool> fail_start{false};
  std::atomic<int> plan_reads{0};

private:
  gr4cp::runtime::StubRuntimeManager stub_;
};

class TestBlockCatalogProvider final
    : public gr4cp::catalog::BlockCatalogProvider {
public:
  bool fail{false};

  std::vector<gr4cp::domain::BlockDescriptor> list() const override {
    if (fail) {
      throw gr4cp::catalog::CatalogLoadError("catalog unavailable");
    }
    gr4cp::domain::BlockPortDescriptor input("in", "float32");
    input.cardinality_kind = gr4cp::domain::BlockPortCardinalityKind::Dynamic;
    input.current_port_count = 2;
    input.max_port_count = 32;
    input.size_parameter = "n_inputs";

    gr4cp::domain::BlockParameterDescriptor scale("scale", "float", false, 1.5,
                                                  "Scale factor");
    scale.runtime_mutability = "mutable";
    gr4cp::domain::BlockParameterDescriptor window(
        "window", "string", true, std::string("hann"), "Window");
    window.enum_choices = std::vector<std::string>{"hann", "rect"};
    window.enum_labels = {{"hann", "Hann"}, {"rect", "Rectangular"}};
    window.allow_custom_value = false;
    gr4cp::domain::BlockParameterDescriptor count("count", "int", false, 3,
                                                  "Count");
    gr4cp::domain::BlockParameterDescriptor label("label", "string", false,
                                                  nullptr, "Label");

    return {
        {
            .id = "blocks.math.add",
            .canonical_type = std::nullopt,
            .name = "Add",
            .category = "Math",
            .summary = "Adds streams",
            .inputs = {input},
            .outputs = {{"out", "float32"}},
            .parameters = {scale, window, count, label},
        },
        {
            .id = "blocks.sources.null",
            .canonical_type = std::nullopt,
            .name = "Null Source",
            .category = "Sources",
            .summary = "Produces zeros",
            .inputs = {},
            .outputs = {{"out", "float32"}},
            .parameters = {},
        },
    };
  }
};

class TestSchedulerCatalogProvider final
    : public gr4cp::catalog::SchedulerCatalogProvider {
public:
  std::vector<gr4cp::domain::SchedulerDescriptor> list() const override {
    return {
        gr4cp::domain::SchedulerDescriptor{"gr::scheduler::SimpleSingle"},
        gr4cp::domain::SchedulerDescriptor{"gr::scheduler::SimpleMulti"},
    };
  }
};

std::string websocket_stream_graph() {
  return R"(blocks:
  - id: "gr::testing::ManagedStreamBlock<float32>"
    parameters:
      name: "series0"
      stream:
        id: "series"
        transport: "websocket"
        payload_format: "series-window-json-v1"
connections: []
)";
}

class GrpcApiTest : public ::testing::Test {
protected:
  void SetUp() override {
    port = server.start("127.0.0.1", 0);
    ASSERT_GT(port, 0);
    channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                  grpc::InsecureChannelCredentials());
    sessions = v1::SessionService::NewStub(channel);
    settings = v1::BlockSettingsService::NewStub(channel);
    catalog = v1::CatalogService::NewStub(channel);
  }

  void TearDown() override {
    runtime_manager.start_gate.open();
    runtime_manager.plan_gate.open();
    server.stop();
  }

  // The REST error code a failed call carries in its trailing metadata, or an
  // empty string.
  static std::string error_code(const grpc::ClientContext &context) {
    const auto &trailers = context.GetServerTrailingMetadata();
    const auto it = trailers.find("gr4cp-error-code");
    return it == trailers.end()
               ? std::string()
               : std::string(it->second.data(), it->second.size());
  }

  v1::Session create_session(const std::string &name = "demo",
                             const std::string &grc = "flowgraph") {
    v1::CreateSessionRequest request;
    request.set_name(name);
    request.set_grc(grc);
    v1::Session session;
    grpc::ClientContext context;
    const auto status = sessions->CreateSession(&context, request, &session);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return session;
  }

  v1::Session start_session(const std::string &id) {
    v1::StartSessionRequest request;
    request.set_session_id(id);
    v1::Session session;
    grpc::ClientContext context;
    const auto status = sessions->StartSession(&context, request, &session);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return session;
  }

  grpc::Status get_session(const std::string &id, v1::Session &session) {
    v1::GetSessionRequest request;
    request.set_session_id(id);
    grpc::ClientContext context;
    return sessions->GetSession(&context, request, &session);
  }

  grpc::Status update_settings(const v1::UpdateBlockSettingsRequest &request,
                               v1::UpdateBlockSettingsResponse &response) {
    grpc::ClientContext context;
    return settings->UpdateBlockSettings(&context, request, &response);
  }

  gr4cp::storage::InMemorySessionRepository repository;
  ControlledRuntimeManager runtime_manager;
  gr4cp::app::SessionService session_service{repository, runtime_manager};
  gr4cp::app::BlockSettingsService block_settings_service{session_service,
                                                          runtime_manager};
  TestBlockCatalogProvider block_catalog_provider;
  gr4cp::app::BlockCatalogService block_catalog_service{block_catalog_provider};
  TestSchedulerCatalogProvider scheduler_catalog_provider;
  gr4cp::app::SchedulerCatalogService scheduler_catalog_service{
      scheduler_catalog_provider};
  gr4cp::api::GrpcServer server{session_service, block_catalog_service,
                                scheduler_catalog_service,
                                block_settings_service};
  int port{};
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<v1::SessionService::Stub> sessions;
  std::unique_ptr<v1::BlockSettingsService::Stub> settings;
  std::unique_ptr<v1::CatalogService::Stub> catalog;
};

TEST_F(GrpcApiTest, CreateSessionReturnsAStoppedSession) {
  const auto session = create_session();

  EXPECT_TRUE(session.id().starts_with("sess_"));
  EXPECT_FALSE(session.server_instance_id().empty());
  EXPECT_EQ(session.name(), "demo");
  EXPECT_EQ(session.state(), v1::SESSION_STATE_STOPPED);
  EXPECT_FALSE(session.has_last_error());
  EXPECT_FALSE(session.has_execution_id());
  EXPECT_FALSE(session.has_completion_reason());
  EXPECT_FALSE(session.has_scheduler_id());
  EXPECT_GT(session.created_at().seconds(), 0);
  EXPECT_GT(session.updated_at().seconds(), 0);
}

TEST_F(GrpcApiTest, CreateSessionKeepsTheSchedulerId) {
  v1::CreateSessionRequest request;
  request.set_grc("flowgraph");
  request.set_scheduler_id("gr::scheduler::SimpleMulti");
  v1::Session session;
  grpc::ClientContext context;
  ASSERT_TRUE(sessions->CreateSession(&context, request, &session).ok());

  EXPECT_EQ(session.name(), "");
  EXPECT_EQ(session.scheduler_id(), "gr::scheduler::SimpleMulti");
}

TEST_F(GrpcApiTest, CreateSessionWithoutGrcIsInvalidArgument) {
  v1::CreateSessionRequest request;
  request.set_name("demo");
  v1::Session session;
  grpc::ClientContext context;
  const auto status = sessions->CreateSession(&context, request, &session);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_FALSE(status.error_message().empty());
}

TEST_F(GrpcApiTest, ListSessionsReturnsEveryCreatedSession) {
  {
    v1::ListSessionsResponse response;
    grpc::ClientContext context;
    ASSERT_TRUE(sessions->ListSessions(&context, {}, &response).ok());
    EXPECT_EQ(response.sessions_size(), 0);
  }

  const auto first = create_session("first");
  const auto second = create_session("second");

  v1::ListSessionsResponse response;
  grpc::ClientContext context;
  ASSERT_TRUE(sessions->ListSessions(&context, {}, &response).ok());
  ASSERT_EQ(response.sessions_size(), 2);
  std::vector<std::string> ids{response.sessions(0).id(),
                               response.sessions(1).id()};
  EXPECT_NE(std::ranges::find(ids, first.id()), ids.end());
  EXPECT_NE(std::ranges::find(ids, second.id()), ids.end());
}

TEST_F(GrpcApiTest, GetSessionOfAnUnknownIdIsNotFound) {
  v1::Session session;
  const auto status = get_session("sess_missing", session);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST_F(GrpcApiTest, LifecycleCallsMoveTheSessionThroughItsStates) {
  const auto created = create_session();

  const auto started = start_session(created.id());
  EXPECT_EQ(started.state(), v1::SESSION_STATE_RUNNING);
  EXPECT_TRUE(started.has_execution_id());
  EXPECT_EQ(started.server_instance_id(), created.server_instance_id());

  v1::Session restarted;
  {
    v1::RestartSessionRequest request;
    request.set_session_id(created.id());
    grpc::ClientContext context;
    ASSERT_TRUE(sessions->RestartSession(&context, request, &restarted).ok());
  }
  EXPECT_EQ(restarted.state(), v1::SESSION_STATE_RUNNING);
  EXPECT_NE(restarted.execution_id(), started.execution_id());

  v1::Session stopped;
  {
    v1::StopSessionRequest request;
    request.set_session_id(created.id());
    grpc::ClientContext context;
    ASSERT_TRUE(sessions->StopSession(&context, request, &stopped).ok());
  }
  EXPECT_EQ(stopped.state(), v1::SESSION_STATE_STOPPED);
  EXPECT_EQ(stopped.completion_reason(), v1::COMPLETION_REASON_STOPPED);

  {
    v1::DeleteSessionRequest request;
    request.set_session_id(created.id());
    google::protobuf::Empty empty;
    grpc::ClientContext context;
    ASSERT_TRUE(sessions->DeleteSession(&context, request, &empty).ok());
  }
  v1::Session gone;
  EXPECT_EQ(get_session(created.id(), gone).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

TEST_F(GrpcApiTest, StartOfAnUnknownSessionIsNotFound) {
  v1::StartSessionRequest request;
  request.set_session_id("sess_missing");
  v1::Session session;
  grpc::ClientContext context;

  EXPECT_EQ(sessions->StartSession(&context, request, &session).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

TEST_F(GrpcApiTest, GetSessionListsTheStreamsOfARunningSession) {
  const auto created = create_session("streams", websocket_stream_graph());

  v1::Session before;
  ASSERT_TRUE(get_session(created.id(), before).ok());
  EXPECT_EQ(before.streams_size(), 0);

  start_session(created.id());

  v1::Session running;
  ASSERT_TRUE(get_session(created.id(), running).ok());
  ASSERT_EQ(running.streams_size(), 1);
  EXPECT_EQ(running.streams(0).id(), "series");
  EXPECT_EQ(running.streams(0).block_instance_name(), "series0");
  EXPECT_EQ(running.streams(0).transport(), "websocket");
  EXPECT_EQ(running.streams(0).payload_format(), "series-window-json-v1");
  EXPECT_EQ(running.streams(0).path(),
            "/sessions/" + created.id() + "/streams/series/ws");
  EXPECT_TRUE(running.streams(0).ready());
}

TEST_F(GrpcApiTest, SettingsRoundTripKeepsEachValuesType) {
  const auto session = start_session(create_session().id());
  constexpr auto large_unsigned =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  constexpr auto lowest = std::numeric_limits<std::int64_t>::min();

  v1::UpdateBlockSettingsRequest update;
  update.set_session_id(session.id());
  update.set_block_unique_name("block0");
  update.set_mode(v1::BLOCK_SETTINGS_MODE_IMMEDIATE);
  auto &patch = *update.mutable_settings();
  patch["enabled"].set_bool_value(true);
  patch["lowest"].set_int_value(lowest);
  patch["highest"].set_uint_value(large_unsigned);
  patch["gain"].set_double_value(0.25);
  patch["label"].set_string_value("9007199254740993");
  patch["cleared"].set_null_value(google::protobuf::NULL_VALUE);
  patch["taps"].mutable_list_value()->add_values()->set_double_value(1.5);
  patch["taps"].mutable_list_value()->add_values()->set_int_value(2);
  (*patch["nested"].mutable_map_value()->mutable_fields())["depth"]
      .set_int_value(7);
  auto &phasor = *patch["phasor"].mutable_map_value()->mutable_fields();
  phasor["real"].set_double_value(1.0);
  phasor["imag"].set_double_value(-2.0);

  v1::UpdateBlockSettingsResponse updated;
  const auto update_status = update_settings(update, updated);
  ASSERT_TRUE(update_status.ok()) << update_status.error_message();
  EXPECT_EQ(updated.session_id(), session.id());
  EXPECT_EQ(updated.block(), "block0");
  EXPECT_EQ(updated.applied_via(), "settings");
  EXPECT_TRUE(updated.accepted());

  v1::GetBlockSettingsRequest request;
  request.set_session_id(session.id());
  request.set_block_unique_name("block0");
  v1::GetBlockSettingsResponse response;
  grpc::ClientContext context;
  const auto status = settings->GetBlockSettings(&context, request, &response);
  ASSERT_TRUE(status.ok()) << status.error_message();

  const auto &read = response.settings();
  ASSERT_EQ(read.size(), 9U);
  EXPECT_TRUE(read.at("enabled").bool_value());
  ASSERT_EQ(read.at("lowest").kind_case(), v1::Value::kIntValue);
  EXPECT_EQ(read.at("lowest").int_value(), lowest);
  // The settings service holds a JSON integer as a signed 64-bit value.
  ASSERT_EQ(read.at("highest").kind_case(), v1::Value::kIntValue);
  EXPECT_EQ(read.at("highest").int_value(),
            std::numeric_limits<std::int64_t>::max());
  ASSERT_EQ(read.at("gain").kind_case(), v1::Value::kDoubleValue);
  EXPECT_EQ(read.at("gain").double_value(), 0.25);
  ASSERT_EQ(read.at("label").kind_case(), v1::Value::kStringValue);
  EXPECT_EQ(read.at("label").string_value(), "9007199254740993");
  EXPECT_EQ(read.at("cleared").kind_case(), v1::Value::kNullValue);
  ASSERT_EQ(read.at("taps").list_value().values_size(), 2);
  EXPECT_EQ(read.at("taps").list_value().values(0).double_value(), 1.5);
  EXPECT_EQ(read.at("taps").list_value().values(1).int_value(), 2);
  EXPECT_EQ(read.at("nested").map_value().fields().at("depth").int_value(), 7);
  EXPECT_EQ(read.at("phasor").map_value().fields().at("real").double_value(),
            1.0);
  EXPECT_EQ(read.at("phasor").map_value().fields().at("imag").double_value(),
            -2.0);
}

TEST_F(GrpcApiTest, UpdateSettingsDefaultsToStagedMode) {
  const auto session = start_session(create_session().id());

  v1::UpdateBlockSettingsRequest update;
  update.set_session_id(session.id());
  update.set_block_unique_name("block0");
  (*update.mutable_settings())["gain"].set_double_value(2.0);
  v1::UpdateBlockSettingsResponse updated;
  ASSERT_TRUE(update_settings(update, updated).ok());

  EXPECT_EQ(updated.applied_via(), "staged_settings");
}

TEST_F(GrpcApiTest,
       UpdateSettingsWithAnIntegerAboveTheSignedRangeIsInvalidArgument) {
  const auto session = start_session(create_session().id());

  v1::UpdateBlockSettingsRequest update;
  update.set_session_id(session.id());
  update.set_block_unique_name("block0");
  (*update.mutable_settings())["count"].set_uint_value(
      std::numeric_limits<std::uint64_t>::max());
  v1::UpdateBlockSettingsResponse updated;
  const auto status = update_settings(update, updated);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("count"), std::string::npos)
      << status.error_message();
}

TEST_F(GrpcApiTest, UpdateSettingsWithAnUnknownModeIsInvalidArgument) {
  const auto session = start_session(create_session().id());

  v1::UpdateBlockSettingsRequest update;
  update.set_session_id(session.id());
  update.set_block_unique_name("block0");
  update.set_mode(static_cast<v1::BlockSettingsMode>(42));
  v1::UpdateBlockSettingsResponse updated;

  EXPECT_EQ(update_settings(update, updated).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(GrpcApiTest, UpdateSettingsOfAStoppedSessionIsFailedPrecondition) {
  const auto session = create_session();

  v1::UpdateBlockSettingsRequest update;
  update.set_session_id(session.id());
  update.set_block_unique_name("block0");
  (*update.mutable_settings())["gain"].set_double_value(2.0);
  v1::UpdateBlockSettingsResponse updated;

  EXPECT_EQ(update_settings(update, updated).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(GrpcApiTest, SettingsOfAnUnknownSessionAreNotFound) {
  v1::GetBlockSettingsRequest request;
  request.set_session_id("sess_missing");
  request.set_block_unique_name("block0");
  v1::GetBlockSettingsResponse response;
  grpc::ClientContext context;

  EXPECT_EQ(
      settings->GetBlockSettings(&context, request, &response).error_code(),
      grpc::StatusCode::NOT_FOUND);
}

TEST_F(GrpcApiTest, ListBlocksReturnsTheCatalog) {
  v1::ListBlocksResponse response;
  grpc::ClientContext context;
  ASSERT_TRUE(catalog->ListBlocks(&context, {}, &response).ok());

  ASSERT_EQ(response.blocks_size(), 2);
  std::vector<std::string> ids{response.blocks(0).id(),
                               response.blocks(1).id()};
  EXPECT_NE(std::ranges::find(ids, "blocks.math.add"), ids.end());
  EXPECT_NE(std::ranges::find(ids, "blocks.sources.null"), ids.end());
}

TEST_F(GrpcApiTest, GetBlockReturnsPortsAndParameters) {
  v1::GetBlockRequest request;
  request.set_block_id("blocks.math.add");
  v1::Block block;
  grpc::ClientContext context;
  ASSERT_TRUE(catalog->GetBlock(&context, request, &block).ok());

  EXPECT_EQ(block.name(), "Add");
  EXPECT_EQ(block.category(), "Math");
  EXPECT_EQ(block.summary(), "Adds streams");

  ASSERT_EQ(block.inputs_size(), 1);
  const auto &input = block.inputs(0);
  EXPECT_EQ(input.name(), "in");
  EXPECT_EQ(input.type(), "float32");
  EXPECT_EQ(input.cardinality_kind(), v1::BLOCK_PORT_CARDINALITY_KIND_DYNAMIC);
  EXPECT_EQ(input.current_port_count(), 2);
  EXPECT_EQ(input.max_port_count(), 32);
  EXPECT_FALSE(input.has_min_port_count());
  EXPECT_FALSE(input.has_render_port_count());
  EXPECT_EQ(input.size_parameter(), "n_inputs");
  EXPECT_FALSE(input.has_handle_name_template());

  ASSERT_EQ(block.outputs_size(), 1);
  EXPECT_EQ(block.outputs(0).cardinality_kind(),
            v1::BLOCK_PORT_CARDINALITY_KIND_FIXED);
  EXPECT_FALSE(block.outputs(0).has_current_port_count());

  ASSERT_EQ(block.parameters_size(), 4);
  const auto &scale = block.parameters(0);
  EXPECT_EQ(scale.name(), "scale");
  EXPECT_EQ(scale.type(), "float");
  EXPECT_FALSE(scale.required());
  EXPECT_EQ(scale.default_value().double_value(), 1.5);
  EXPECT_EQ(scale.summary(), "Scale factor");
  EXPECT_EQ(scale.runtime_mutability(), "mutable");
  EXPECT_FALSE(scale.has_enum_choices());
  EXPECT_FALSE(scale.has_allow_custom_value());
  EXPECT_FALSE(scale.has_ui_hint());

  const auto &window = block.parameters(1);
  EXPECT_TRUE(window.required());
  EXPECT_EQ(window.default_value().string_value(), "hann");
  ASSERT_TRUE(window.has_enum_choices());
  ASSERT_EQ(window.enum_choices().values_size(), 2);
  EXPECT_EQ(window.enum_choices().values(1), "rect");
  EXPECT_EQ(window.enum_labels().at("rect"), "Rectangular");
  ASSERT_TRUE(window.has_allow_custom_value());
  EXPECT_FALSE(window.allow_custom_value());

  ASSERT_EQ(block.parameters(2).default_value().kind_case(),
            v1::Value::kIntValue);
  EXPECT_EQ(block.parameters(2).default_value().int_value(), 3);
  EXPECT_EQ(block.parameters(3).default_value().kind_case(),
            v1::Value::kNullValue);
}

TEST_F(GrpcApiTest, GetBlockOfAnUnknownIdIsNotFound) {
  v1::GetBlockRequest request;
  request.set_block_id("blocks.missing");
  v1::Block block;
  grpc::ClientContext context;

  EXPECT_EQ(catalog->GetBlock(&context, request, &block).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

TEST_F(GrpcApiTest, SchedulersAreListedAndReadById) {
  v1::ListSchedulersResponse response;
  {
    grpc::ClientContext context;
    ASSERT_TRUE(catalog->ListSchedulers(&context, {}, &response).ok());
  }
  ASSERT_EQ(response.schedulers_size(), 2);

  v1::GetSchedulerRequest request;
  request.set_scheduler_id("gr::scheduler::SimpleMulti");
  v1::Scheduler scheduler;
  {
    grpc::ClientContext context;
    ASSERT_TRUE(catalog->GetScheduler(&context, request, &scheduler).ok());
  }
  EXPECT_EQ(scheduler.id(), "gr::scheduler::SimpleMulti");

  request.set_scheduler_id("gr::scheduler::Missing");
  grpc::ClientContext context;
  EXPECT_EQ(catalog->GetScheduler(&context, request, &scheduler).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

TEST_F(GrpcApiTest, GetHealthReportsTheServerInstanceOfItsSessions) {
  const auto stub = v1::ServerService::NewStub(channel);
  v1::GetHealthResponse health;
  grpc::ClientContext context;
  const auto status = stub->GetHealth(&context, {}, &health);
  ASSERT_TRUE(status.ok()) << status.error_message();

  EXPECT_TRUE(health.ok());
  EXPECT_EQ(health.server_instance_id().size(), 36U);
  EXPECT_EQ(health.server_instance_id(), create_session().server_instance_id());
}

TEST_F(GrpcApiTest, StandardHealthServiceReportsServing) {
  // The health protocol's messages are not generated here: Check takes an empty
  // request, and SERVING is field 1 with value 1.
  grpc::GenericStub generic(channel);
  grpc::ClientContext context;
  grpc::CompletionQueue queue;
  const grpc::Slice empty;
  const grpc::ByteBuffer request(&empty, 1);
  grpc::ByteBuffer response;
  grpc::Status status;
  const auto call = generic.PrepareUnaryCall(
      &context, "/grpc.health.v1.Health/Check", request, &queue);
  call->StartCall();
  call->Finish(&response, &status, nullptr);
  void *tag = nullptr;
  bool finished = false;
  ASSERT_TRUE(queue.Next(&tag, &finished));
  ASSERT_TRUE(finished);
  ASSERT_TRUE(status.ok()) << status.error_message();

  std::vector<grpc::Slice> slices;
  ASSERT_TRUE(response.Dump(&slices).ok());
  std::string bytes;
  for (const auto &slice : slices) {
    bytes.append(reinterpret_cast<const char *>(slice.begin()), slice.size());
  }
  EXPECT_EQ(bytes, std::string("\x08\x01", 2));
}

TEST_F(GrpcApiTest, StopEndsServingAndStartServesAgain) {
  server.stop();
  {
    v1::ListSessionsResponse response;
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::seconds(2));
    EXPECT_FALSE(sessions->ListSessions(&context, {}, &response).ok());
  }

  const auto second_port = server.start("127.0.0.1", 0);
  ASSERT_GT(second_port, 0);
  EXPECT_EQ(server.start("127.0.0.1", 0), 0);

  const auto second_channel =
      grpc::CreateChannel("127.0.0.1:" + std::to_string(second_port),
                          grpc::InsecureChannelCredentials());
  const auto stub = v1::SessionService::NewStub(second_channel);
  v1::ListSessionsResponse response;
  grpc::ClientContext context;
  EXPECT_TRUE(stub->ListSessions(&context, {}, &response).ok());
}

TEST_F(GrpcApiTest, StartOnAPortThatIsAlreadyServedFails) {
  gr4cp::api::GrpcServer second(session_service, block_catalog_service,
                                scheduler_catalog_service,
                                block_settings_service);

  EXPECT_EQ(second.start("127.0.0.1", port), 0);
}

TEST_F(GrpcApiTest, FailedCallsCarryTheRestErrorCode) {
  {
    v1::GetSessionRequest request;
    request.set_session_id("sess_missing");
    v1::Session session;
    grpc::ClientContext context;
    EXPECT_EQ(sessions->GetSession(&context, request, &session).error_code(),
              grpc::StatusCode::NOT_FOUND);
    EXPECT_EQ(error_code(context), "not_found");
  }
  {
    v1::Session session;
    grpc::ClientContext context;
    EXPECT_EQ(sessions->CreateSession(&context, {}, &session).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_EQ(error_code(context), "validation_error");
  }

  const auto created = create_session();
  {
    v1::UpdateBlockSettingsRequest request;
    request.set_session_id(created.id());
    request.set_block_unique_name("block0");
    v1::UpdateBlockSettingsResponse response;
    grpc::ClientContext context;
    EXPECT_EQ(settings->UpdateBlockSettings(&context, request, &response)
                  .error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
    EXPECT_EQ(error_code(context), "invalid_state");
  }
  {
    runtime_manager.fail_start = true;
    v1::StartSessionRequest request;
    request.set_session_id(created.id());
    v1::Session session;
    grpc::ClientContext context;
    const auto status = sessions->StartSession(&context, request, &session);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_NE(status.error_message().find("start refused"), std::string::npos)
        << status.error_message();
    EXPECT_EQ(error_code(context), "runtime_error");
  }
}

TEST_F(GrpcApiTest, CatalogFailureCarriesTheCatalogErrorCode) {
  block_catalog_provider.fail = true;
  v1::ListBlocksResponse response;
  grpc::ClientContext context;
  const auto status = catalog->ListBlocks(&context, {}, &response);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(status.error_message(), "catalog unavailable");
  EXPECT_EQ(error_code(context), "catalog_error");
}

TEST_F(GrpcApiTest, SuccessfulCallsCarryNoErrorCode) {
  v1::ListSessionsResponse response;
  grpc::ClientContext context;
  ASSERT_TRUE(sessions->ListSessions(&context, {}, &response).ok());

  EXPECT_EQ(error_code(context), "");
}

TEST_F(GrpcApiTest, DeadlineEndsTheCallAtOnceAndTheStartCompletesOnTheServer) {
  const auto created = create_session();
  runtime_manager.start_gate.close();

  v1::StartSessionRequest request;
  request.set_session_id(created.id());
  v1::Session session;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::milliseconds(200));
  const auto begin = std::chrono::steady_clock::now();
  const auto status = sessions->StartSession(&context, request, &session);

  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(3));

  ASSERT_TRUE(runtime_manager.start_gate.wait_for_held_call());
  runtime_manager.start_gate.open();

  v1::Session read;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    ASSERT_TRUE(get_session(created.id(), read).ok());
    if (read.state() == v1::SESSION_STATE_RUNNING) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  EXPECT_EQ(read.state(), v1::SESSION_STATE_RUNNING);
}

TEST_F(GrpcApiTest, CancellationEndsACallThatIsHeldInTheRuntime) {
  const auto created = create_session();
  runtime_manager.start_gate.close();

  v1::StartSessionRequest request;
  request.set_session_id(created.id());
  v1::Session session;
  grpc::ClientContext context;
  auto call = std::async(std::launch::async, [&]() {
    return sessions->StartSession(&context, request, &session);
  });
  ASSERT_TRUE(runtime_manager.start_gate.wait_for_held_call());
  context.TryCancel();

  ASSERT_EQ(call.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_EQ(call.get().error_code(), grpc::StatusCode::CANCELLED);
  runtime_manager.start_gate.open();
}

TEST_F(GrpcApiTest, CancelledListSessionsStopsBeforeTheNextSession) {
  start_session(create_session("first").id());
  start_session(create_session("second").id());
  runtime_manager.plan_reads = 0;
  runtime_manager.plan_gate.close();

  v1::ListSessionsResponse response;
  grpc::ClientContext context;
  auto call = std::async(std::launch::async, [&]() {
    return sessions->ListSessions(&context, {}, &response);
  });
  ASSERT_TRUE(runtime_manager.plan_gate.wait_for_held_call());
  context.TryCancel();
  ASSERT_EQ(call.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  EXPECT_EQ(call.get().error_code(), grpc::StatusCode::CANCELLED);

  // Give the cancellation time to reach the server before the held read
  // returns.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  runtime_manager.plan_gate.open();
  server.stop();

  EXPECT_EQ(runtime_manager.plan_reads.load(), 1);
}

} // namespace
