#include "gr4cp/app/session_stream_service.hpp"

#include <chrono>
#include <string>

#include <gtest/gtest.h>

namespace {

constexpr std::string_view kGenericStreamBlockId = "gr::testing::ManagedStreamBlock<float32>";
constexpr std::string_view kGenericSpectrumBlockId = "gr::testing::ManagedSpectrumBlock<float32>";
constexpr std::string_view kGenericMatrixBlockId = "gr::testing::ManagedMatrixBlock<float32>";
constexpr std::string_view kSeriesPayloadFormat = "series-window-json-v1";
constexpr std::string_view kSpectrumPayloadFormat = "dataset-xy-json-v1";
constexpr std::string_view kMatrixPayloadFormat = "waterfall-spectrum-json-v1";

gr4cp::domain::Session make_session(std::string id, std::string grc_content) {
    gr4cp::domain::Session session;
    session.id = std::move(id);
    session.name = session.id;
    session.grc_content = std::move(grc_content);
    session.created_at = std::chrono::system_clock::now();
    session.updated_at = session.created_at;
    return session;
}

std::string authored_stream_graph(std::string_view block_id,
                                  std::string_view instance_name,
                                  std::string_view transport,
                                  std::string_view payload_format,
                                  std::optional<std::string_view> stream_id = std::nullopt) {
    auto stream_id_yaml = stream_id.has_value() ? "        id: \"" + std::string(*stream_id) + "\"\n" : "";
    return "blocks:\n"
           "  - id: \"" +
           std::string(block_id) +
           "\"\n"
           "    parameters:\n"
           "      name: \"" +
           std::string(instance_name) +
           "\"\n"
           "      stream:\n" +
           stream_id_yaml +
           "        transport: \"" + std::string(transport) + "\"\n"
           "        payload_format: \"" +
           std::string(payload_format) +
           "\"\n"
           "      update_ms: 250\n"
           "connections: []\n";
}

void expect_single_stream(const gr4cp::domain::StreamRuntimePlan& plan,
                          std::string_view stream_id,
                          std::string_view block_instance_name,
                          std::string_view transport,
                          std::string_view payload_format,
                          std::string_view path) {
    ASSERT_EQ(plan.streams.size(), 1U);
    const auto& stream = plan.streams.front();
    EXPECT_EQ(stream.stream_id, stream_id);
    EXPECT_EQ(stream.block_instance_name, block_instance_name);
    EXPECT_EQ(stream.transport, transport);
    EXPECT_EQ(stream.payload_format, payload_format);
    EXPECT_EQ(stream.path, path);
    EXPECT_TRUE(stream.ready);
}

std::string unmanaged_graph() {
    return R"(blocks:
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "sink0"
connections: []
)";
}

std::string compatibility_flattened_stream_graph() {
    return R"({
  "graph_name": "legacy_series",
  "source_format": "grc",
  "blocks": [
    {
      "instance_name": "series0",
      "block_type": "gr::testing::ManagedLegacyStreamBlock<float32>",
      "raw_parameters": {
        "transport": "http_poll",
        "payload_format": "series-window-json-v1",
        "endpoint": "http://127.0.0.1:18083/snapshot",
        "update_ms": 250
      }
    }
  ],
  "connections": []
})";
}

std::string broken_managed_graph() {
    return "blocks:\n"
           "  - id: \"" +
           std::string(kGenericStreamBlockId) +
           "\"\n"
           "    parameters:\n"
           "      name: \"series0\"\n"
           "      stream:\n"
           "        transport: \"http_poll\"\n"
           "        payload_format: \"" +
           std::string(kSeriesPayloadFormat) +
           "\"\n"
           "      endpoint: \"http://127.0.0.1:18083/snapshot\"\n"
           "  - id: \"" +
           std::string(kGenericStreamBlockId) +
           "\"\n"
           "    parameters:\n"
           "      stream:\n"
           "        transport: \"http_poll\"\n"
           "        payload_format: \"" +
           std::string(kSeriesPayloadFormat) +
           "\"\n"
           "      endpoint: \"http://127.0.0.1:18084/snapshot\"\n"
           "connections: []\n";
}

std::string unsupported_transport_graph() {
    return authored_stream_graph(kGenericStreamBlockId, "spectrum0", "grpc", kSpectrumPayloadFormat);
}

TEST(SessionStreamServiceTest, AuthoredHttpPollStreamDerivesPlan) {
    gr4cp::app::SessionStreamService service;
    const auto plan = service.derive_plan(make_session(
        "sess_demo",
        authored_stream_graph(kGenericStreamBlockId, "series0", "http_poll", kSeriesPayloadFormat)));

    ASSERT_TRUE(plan.has_value());
    expect_single_stream(*plan,
                         "series0",
                         "series0",
                         "http_poll",
                         kSeriesPayloadFormat,
                         "/sessions/sess_demo/streams/series0/http");
}

TEST(SessionStreamServiceTest, UnsupportedGraphOmitsPlan) {
    gr4cp::app::SessionStreamService service;
    EXPECT_FALSE(service.derive_plan(make_session("sess_demo", unmanaged_graph())).has_value());
}

TEST(SessionStreamServiceTest, AuthoredWebsocketStreamDerivesWsPlan) {
    gr4cp::app::SessionStreamService service;
    const auto plan = service.derive_plan(make_session(
        "sess_demo",
        authored_stream_graph(kGenericStreamBlockId, "series0", "websocket", kSeriesPayloadFormat)));

    ASSERT_TRUE(plan.has_value());
    expect_single_stream(*plan,
                         "series0",
                         "series0",
                         "websocket",
                         kSeriesPayloadFormat,
                         "/sessions/sess_demo/streams/series0/ws");
}

TEST(SessionStreamServiceTest, PayloadFormatIsPreservedForAuthoredWebsocketStream) {
    gr4cp::app::SessionStreamService service;
    const auto plan = service.derive_plan(make_session(
        "sess_demo",
        authored_stream_graph(kGenericSpectrumBlockId, "spectrum0", "websocket", kSpectrumPayloadFormat)));

    ASSERT_TRUE(plan.has_value());
    expect_single_stream(*plan,
                         "spectrum0",
                         "spectrum0",
                         "websocket",
                         kSpectrumPayloadFormat,
                         "/sessions/sess_demo/streams/spectrum0/ws");
}

TEST(SessionStreamServiceTest, DifferentGenericBlockIdsCanAuthorSameHttpContract) {
    gr4cp::app::SessionStreamService service;
    const auto plan = service.derive_plan(make_session(
        "sess_demo",
        authored_stream_graph(kGenericMatrixBlockId, "matrix0", "http_poll", kMatrixPayloadFormat)));

    ASSERT_TRUE(plan.has_value());
    expect_single_stream(*plan,
                         "matrix0",
                         "matrix0",
                         "http_poll",
                         kMatrixPayloadFormat,
                         "/sessions/sess_demo/streams/matrix0/http");
}

TEST(SessionStreamServiceTest, DifferentGenericBlockIdsCanAuthorSameWebsocketContract) {
    gr4cp::app::SessionStreamService service;
    const auto plan = service.derive_plan(make_session(
        "sess_demo",
        authored_stream_graph(kGenericMatrixBlockId, "matrix0", "websocket", kMatrixPayloadFormat)));

    ASSERT_TRUE(plan.has_value());
    expect_single_stream(*plan,
                         "matrix0",
                         "matrix0",
                         "websocket",
                         kMatrixPayloadFormat,
                         "/sessions/sess_demo/streams/matrix0/ws");
}

TEST(SessionStreamServiceTest, CompatibilityFlattenedStreamMetadataStillDerivesPlan) {
    gr4cp::app::SessionStreamService service;
    const auto plan = service.derive_plan(make_session("sess_demo", compatibility_flattened_stream_graph()));

    ASSERT_TRUE(plan.has_value());
    expect_single_stream(*plan,
                         "series0",
                         "series0",
                         "http_poll",
                         kSeriesPayloadFormat,
                         "/sessions/sess_demo/streams/series0/http");
}

TEST(SessionStreamServiceTest, DescriptorDerivationIsStable) {
    gr4cp::app::SessionStreamService service;
    const auto session = make_session(
        "sess_demo",
        authored_stream_graph(kGenericStreamBlockId, "series0", "http_poll", kSeriesPayloadFormat));

    const auto first = service.derive_plan(session);
    const auto second = service.derive_plan(session);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(first->streams.size(), second->streams.size());
    EXPECT_EQ(first->streams.front().stream_id, second->streams.front().stream_id);
    EXPECT_EQ(first->streams.front().path, second->streams.front().path);
}

TEST(SessionStreamServiceTest, BrokenAuthoredStreamBindingSuppressesPlan) {
    gr4cp::app::SessionStreamService service;
    EXPECT_FALSE(service.derive_plan(make_session("sess_demo", broken_managed_graph())).has_value());
}

TEST(SessionStreamServiceTest, ExplicitStreamIdOverridesBlockInstanceName) {
    gr4cp::app::SessionStreamService service;
    const auto plan = service.derive_plan(make_session(
        "sess_demo",
        authored_stream_graph(kGenericStreamBlockId, "sink0", "http_poll", kSeriesPayloadFormat, "public_stream_0")));

    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->streams.size(), 1U);
    EXPECT_EQ(plan->streams.front().stream_id, "public_stream_0");
    EXPECT_EQ(plan->streams.front().block_instance_name, "sink0");
    EXPECT_EQ(plan->streams.front().path, "/sessions/sess_demo/streams/public_stream_0/http");
}

TEST(SessionStreamServiceTest, UnsupportedTransportIsExplicitlyInvalid) {
    const auto error = gr4cp::domain::validate_stream_runtime_contract(
        make_session("sess_demo", unsupported_transport_graph()));

    ASSERT_TRUE(error.has_value());
    EXPECT_NE(error->find("managed stream transport is not supported"), std::string::npos);
}

TEST(SessionStreamServiceTest, SupportedWebsocketTransportIsExplicitlyValid) {
    const auto error = gr4cp::domain::validate_stream_runtime_contract(
        make_session("sess_demo", authored_stream_graph(kGenericMatrixBlockId, "matrix0", "websocket", kMatrixPayloadFormat)));

    EXPECT_FALSE(error.has_value());
}

TEST(SessionStreamServiceTest, HttpSnapshotTransportRemainsExplicitlyValidWhenAuthored) {
    const auto error = gr4cp::domain::validate_stream_runtime_contract(make_session(
        "sess_demo",
        authored_stream_graph(kGenericMatrixBlockId, "matrix0", "http_snapshot", kMatrixPayloadFormat)));

    EXPECT_FALSE(error.has_value());
}

}  // namespace
