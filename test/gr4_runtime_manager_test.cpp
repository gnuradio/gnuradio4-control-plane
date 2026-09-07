#include "gr4cp/domain/session.hpp"
#include "gr4cp/runtime/gr4_runtime_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

gr4cp::domain::Session make_session(std::string id, std::string yaml) {
    gr4cp::domain::Session session;
    session.id = std::move(id);
    session.name = session.id;
    session.grc_content = std::move(yaml);
    session.state = gr4cp::domain::SessionState::Stopped;
    session.created_at = std::chrono::system_clock::now();
    session.updated_at = session.created_at;
    return session;
}

std::string continuous_graph_yaml() {
    return std::format(R"(blocks:
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    parameters:
      name: "src0"
      sample_rate: 1000.0
      chunk_size: 32
      signal_type: "Sin"
      frequency: 25.0
      amplitude: 1.0
      offset: 0.0
      phase: 0.0
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "sink0"
connections:
  - ["src0", 0, "sink0", 0]
)");
}

std::string legacy_minimal_graph_json() {
    return R"({
  "graph_name": "legacy_null_source_to_sink",
  "source_format": "grc",
  "blocks": [
    {
      "instance_name": "src0",
      "block_type": "gr::blocks::testing::NullSource<float32>",
      "enabled": true,
      "raw_parameters": {}
    },
    {
      "instance_name": "sink0",
      "block_type": "gr::blocks::testing::NullSink<float32>",
      "enabled": true,
      "raw_parameters": {}
    }
  ],
  "connections": [
    {
      "source_block": "src0",
      "source_port_token": "out",
      "dest_block": "sink0",
      "dest_port_token": "in"
    }
  ]
})";
}

std::string studio_inline_graph_yaml() {
    return R"(# gr4-studio inline grc
metadata:
  name: Untitled Graph
  description: ""
blocks:
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: gr__testing__NullSink_float32__5
  - id: "gr::blocks::testing::NullSource<float32>"
    parameters:
      name: gr__testing__NullSource_float32__2
connections:
  - [gr__testing__NullSource_float32__2, out, gr__testing__NullSink_float32__5, in]
)";
}

std::string compatibility_studio_http_series_graph_yaml() {
    return R"(blocks:
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    parameters:
      name: "src0"
      sample_rate: 1000.0
      chunk_size: 32
      signal_type: "Sin"
      frequency: 25.0
      amplitude: 1.0
      offset: 0.0
      phase: 0.0
  - id: "gr::studio::StudioSeriesSink<float32>"
    parameters:
      name: "series0"
      stream:
        transport: "http_poll"
        payload_format: "series-window-json-v1"
      poll_ms: 250
      window_size: 64
      channels: 1
connections:
  - ["src0", "out", "series0", "in#0"]
)";
}

std::string compatibility_studio_http_waterfall_graph_yaml() {
    return R"(blocks:
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    parameters:
      name: "src0"
      sample_rate: 1000.0
      chunk_size: 32
      signal_type: "Sin"
      frequency: 25.0
      amplitude: 1.0
      offset: 0.0
      phase: 0.0
  - id: "gr::studio::StudioWaterfallSink<float32>"
    parameters:
      name: "waterfall0"
      stream:
        transport: "http_poll"
        payload_format: "waterfall-spectrum-json-v1"
      poll_ms: 250
      fft_size: 32
      sample_rate: 1000.0
      time_span: 0.256
connections:
  - ["src0", 0, "waterfall0", 0]
)";
}

class Gr4RuntimeManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        try {
            probe_session_ = make_session("probe", continuous_graph_yaml());
            runtime_.prepare(probe_session_);
            runtime_.destroy(probe_session_);
        } catch (const std::exception& error) {
            GTEST_SKIP() << "GNU Radio 4 runtime unavailable: " << error.what();
        }
    }

    gr4cp::runtime::Gr4RuntimeManager runtime_;
    gr4cp::domain::Session probe_session_;
};

TEST_F(Gr4RuntimeManagerTest, PrepareStartStopAndDestroyManageExecutableGraphLifecycle) {
    auto session = make_session("runtime_lifecycle", continuous_graph_yaml());

    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(250ms);

    runtime_.stop(session);
    runtime_.destroy(session);
    SUCCEED();
}

TEST_F(Gr4RuntimeManagerTest, PrepareAfterStopReusesPreparedExecutionForRestart) {
    auto session = make_session("runtime_restart", continuous_graph_yaml());

    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(250ms);

    runtime_.stop(session);
    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(250ms);

    runtime_.destroy(session);
}

TEST_F(Gr4RuntimeManagerTest, InvalidGraphProducesActionablePrepareError) {
    auto session = make_session("runtime_invalid_graph", R"(blocks:
  - id: "gr::does::not::Exist"
    parameters:
      name: "bad0"
connections: []
)");

    EXPECT_THROW(
        {
            try {
                runtime_.prepare(session);
            } catch (const std::runtime_error& error) {
                EXPECT_NE(std::string(error.what()).find("prepare"), std::string::npos);
                throw;
            }
        },
        std::runtime_error);
}

TEST_F(Gr4RuntimeManagerTest, LegacyMinimalGraphShapeIsNormalizedBeforePrepare) {
    auto session = make_session("runtime_legacy_graph", legacy_minimal_graph_json());

    EXPECT_NO_THROW(runtime_.prepare(session));
    EXPECT_NO_THROW(runtime_.start(session));
    std::this_thread::sleep_for(100ms);
    EXPECT_NO_THROW(runtime_.stop(session));
    EXPECT_NO_THROW(runtime_.destroy(session));
}

TEST_F(Gr4RuntimeManagerTest, CompatibilityStudioInlineGraphShapeIsNormalizedBeforePrepare) {
    auto session = make_session("runtime_studio_graph", studio_inline_graph_yaml());

    EXPECT_NO_THROW(runtime_.prepare(session));
    EXPECT_NO_THROW(runtime_.start(session));
    std::this_thread::sleep_for(100ms);
    EXPECT_NO_THROW(runtime_.stop(session));
    EXPECT_NO_THROW(runtime_.destroy(session));
}

TEST_F(Gr4RuntimeManagerTest, RunningSessionSupportsBlockSettingsMessageRoundTrip) {
    auto session = make_session("runtime_block_settings", continuous_graph_yaml());

    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(100ms);

    EXPECT_NO_THROW(runtime_.set_block_settings(
        session,
        "src0",
        gr::property_map{{"frequency", 1250.0}, {"amplitude", 0.5}},
        gr4cp::runtime::BlockSettingsMode::Staged));

    const auto settings = runtime_.get_block_settings(session, "src0");
    ASSERT_TRUE(settings.contains("frequency"));
    ASSERT_TRUE(settings.contains("amplitude"));
    EXPECT_EQ(settings.at("frequency").value_or(0.0F), 1250.0F);
    EXPECT_EQ(settings.at("amplitude").value_or(0.0F), 0.5F);

    runtime_.stop(session);
    runtime_.destroy(session);
}

TEST_F(Gr4RuntimeManagerTest, CompatibilityStudioBlockNameResolvesForSettingsRoundTrip) {
    auto session = make_session("runtime_studio_name_settings", R"(blocks:
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    parameters:
      name: "gr__basic__SignalGenerator_float32__1"
      sample_rate: 1000.0
      chunk_size: 32
      signal_type: "Sin"
      frequency: 25.0
      amplitude: 1.0
      offset: 0.0
      phase: 0.0
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "gr__testing__NullSink_float32__1"
connections:
  - ["gr__basic__SignalGenerator_float32__1", 0, "gr__testing__NullSink_float32__1", 0]
)");

    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(100ms);

    EXPECT_NO_THROW(runtime_.set_block_settings(
        session,
        "gr__basic__SignalGenerator_float32__1",
        gr::property_map{{"frequency", 1250.0}, {"amplitude", 0.5}},
        gr4cp::runtime::BlockSettingsMode::Staged));

    const auto settings = runtime_.get_block_settings(session, "gr__basic__SignalGenerator_float32__1");
    ASSERT_TRUE(settings.contains("frequency"));
    ASSERT_TRUE(settings.contains("amplitude"));
    EXPECT_EQ(settings.at("frequency").value_or(0.0F), 1250.0F);
    EXPECT_EQ(settings.at("amplitude").value_or(0.0F), 0.5F);

    runtime_.stop(session);
    runtime_.destroy(session);
}

TEST_F(Gr4RuntimeManagerTest, RunningSessionAlsoResolvesInternalRuntimeIdentifierForSettingsRoundTrip) {
    auto session = make_session("runtime_internal_id_settings", continuous_graph_yaml());

    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(100ms);

    const auto authored = runtime_.get_block_settings(session, "src0");
    ASSERT_TRUE(authored.contains("unique_name"));
    std::string runtime_identifier;
    gr::pmt::ValueVisitor([&runtime_identifier](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, std::pmr::string> || std::same_as<T, std::string> || std::same_as<T, std::string_view>) {
            runtime_identifier = std::string(item);
        }
    }).visit(authored.at("unique_name"));
    ASSERT_FALSE(runtime_identifier.empty());

    EXPECT_NO_THROW(runtime_.set_block_settings(
        session,
        runtime_identifier,
        gr::property_map{{"frequency", 900.0}},
        gr4cp::runtime::BlockSettingsMode::Staged));

    const auto settings = runtime_.get_block_settings(session, runtime_identifier);
    ASSERT_TRUE(settings.contains("frequency"));
    EXPECT_EQ(settings.at("frequency").value_or(0.0F), 900.0F);

    runtime_.stop(session);
    runtime_.destroy(session);
}

TEST_F(Gr4RuntimeManagerTest, MissingBlockSettingsLookupIncludesAvailableRuntimeBlockNames) {
    auto session = make_session("runtime_block_lookup_debug", continuous_graph_yaml());

    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(100ms);

    try {
        (void)runtime_.get_block_settings(session, "missing_block");
        FAIL() << "expected BlockNotFoundError";
    } catch (const gr4cp::runtime::BlockNotFoundError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("missing_block"), std::string::npos);
        EXPECT_NE(message.find("available runtime blocks"), std::string::npos);
        EXPECT_NE(message.find("src0"), std::string::npos);
        EXPECT_NE(message.find("sink0"), std::string::npos);
    }

    runtime_.stop(session);
    runtime_.destroy(session);
}

TEST_F(Gr4RuntimeManagerTest, CompatibilityStudioSeriesSinkHttpStreamCanBeFetchedFromRuntimeBinding) {
    auto session = make_session("runtime_managed_http_series", compatibility_studio_http_series_graph_yaml());

    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(250ms);

    const auto plan = runtime_.active_stream_plan(session);
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->streams.size(), 1U);
    EXPECT_EQ(plan->streams.front().stream_id, "series0");
    EXPECT_EQ(plan->streams.front().transport, "http_poll");
    EXPECT_EQ(plan->streams.front().path, "/sessions/runtime_managed_http_series/streams/series0/http");

    const auto stream = runtime_.fetch_http_stream(session, "series0");
    EXPECT_EQ(stream.status, 200);
    EXPECT_EQ(stream.content_type, "application/json");
    EXPECT_NE(stream.body.find("\"sample_type\":\"float32\""), std::string::npos);
    EXPECT_NE(stream.body.find("\"layout\":\"channels_first\""), std::string::npos);
    EXPECT_NE(stream.body.find("\"data\":["), std::string::npos);

    runtime_.stop(session);
    runtime_.destroy(session);
}

TEST_F(Gr4RuntimeManagerTest, CompatibilityStudioWaterfallSinkHttpStreamCanBeFetchedFromRuntimeBinding) {
    auto session = make_session("runtime_managed_http_waterfall", compatibility_studio_http_waterfall_graph_yaml());

    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(250ms);

    const auto plan = runtime_.active_stream_plan(session);
    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->streams.size(), 1U);
    EXPECT_EQ(plan->streams.front().stream_id, "waterfall0");
    EXPECT_EQ(plan->streams.front().transport, "http_poll");
    EXPECT_EQ(plan->streams.front().payload_format, "waterfall-spectrum-json-v1");
    EXPECT_EQ(plan->streams.front().path, "/sessions/runtime_managed_http_waterfall/streams/waterfall0/http");

    const auto stream = runtime_.fetch_http_stream(session, "waterfall0");
    EXPECT_EQ(stream.status, 200);
    EXPECT_EQ(stream.content_type, "application/json");
    EXPECT_NE(stream.body.find("\"payload_format\":\"waterfall-spectrum-json-v1\""), std::string::npos);
    EXPECT_NE(stream.body.find("\"layout\":\"waterfall_matrix\""), std::string::npos);

    runtime_.stop(session);
    runtime_.destroy(session);
}

}  // namespace
