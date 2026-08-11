#include "gr4cp/api/http_server.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <iomanip>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/app/session_stream_service.hpp"
#include "gr4cp/catalog/block_catalog_provider.hpp"
#include "gr4cp/catalog/scheduler_catalog_provider.hpp"
#include "gr4cp/runtime/gr4_runtime_manager.hpp"
#include "gr4cp/runtime/runtime_manager.hpp"
#include "gr4cp/runtime/stream_binding_allocator.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

constexpr std::string_view kGenericStreamBlockId = "gr::testing::ManagedStreamBlock<float32>";
constexpr std::string_view kGenericSpectrumBlockId = "gr::testing::ManagedSpectrumBlock<float32>";
constexpr std::string_view kGenericMatrixBlockId = "gr::testing::ManagedMatrixBlock<float32>";
constexpr std::string_view kSeriesPayloadFormat = "series-window-json-v1";
constexpr std::string_view kSpectrumPayloadFormat = "dataset-xy-json-v1";
constexpr std::string_view kMatrixPayloadFormat = "waterfall-spectrum-json-v1";

class TestBlockCatalogProvider final : public gr4cp::catalog::BlockCatalogProvider {
public:
    std::vector<gr4cp::domain::BlockDescriptor> list() const override {
        return {
            {
                .id = "gr::blocks::math::Add<float32, std::plus<float32>>",
                .canonical_type = std::nullopt,
                .name = "Add<float32>",
                .category = "Math",
                .summary = std::string(4096U, 'A'),
                .inputs = {[] {
                    gr4cp::domain::BlockPortDescriptor port;
                    port.name = "in";
                    port.type = "float32";
                    port.cardinality_kind = gr4cp::domain::BlockPortCardinalityKind::Dynamic;
                    port.current_port_count = 2;
                    port.render_port_count = 2;
                    port.min_port_count = 1;
                    port.max_port_count = 32;
                    port.size_parameter = "n_inputs";
                    port.handle_name_template = "in#${index}";
                    return port;
                }()},
                .outputs = {{"out", "float32"}},
                .parameters = {[] {
                    gr4cp::domain::BlockParameterDescriptor parameter("scale", "float", false, 1.0, "Scale factor");
                    parameter.runtime_mutability = "mutable";
                    parameter.value_kind = "scalar";
                    parameter.ui_hint = "advanced";
                    return parameter;
                }()},
            },
            {
                .id = "blocks.math.add_dynamic",
                .canonical_type = std::nullopt,
                .name = "Add",
                .category = "Math",
                .summary = "Adds a dynamic number of float streams",
                .inputs = {[] {
                    gr4cp::domain::BlockPortDescriptor port;
                    port.name = "in";
                    port.type = "float";
                    port.cardinality_kind = gr4cp::domain::BlockPortCardinalityKind::Dynamic;
                    port.current_port_count = 3;
                    port.render_port_count = 3;
                    port.min_port_count = 1;
                    port.max_port_count = 32;
                    port.size_parameter = "n_inputs";
                    port.handle_name_template = "in#${index}";
                    return port;
                }()},
                .outputs = {{"out", "float"}},
                .parameters = {{"n_inputs", "int", false, 3, "Number of inputs"}},
            },
            {
                .id = "blocks.sources.signal_source_f",
                .canonical_type = std::nullopt,
                .name = "Signal Source",
                .category = "Sources",
                .summary = "",
                .inputs = {},
                .outputs = {{"out", "float"}},
                .parameters = {{"frequency", "float", false, 1000.0, ""}},
            },
            {
                .id = "blocks.math.add_ff",
                .canonical_type = std::nullopt,
                .name = "Add",
                .category = "Math",
                .summary = "Adds two float streams",
                .inputs = {{"in0", "float"}, {"in1", "float"}},
                .outputs = {{"out", "float"}},
                .parameters = {[] {
                    gr4cp::domain::BlockParameterDescriptor parameter("scale", "float", false, 1.0, "Scale factor");
                    parameter.runtime_mutability = "mutable";
                    parameter.value_kind = "scalar";
                    parameter.ui_hint = "advanced";
                    return parameter;
                }()},
            },
            {
                .id = "blocks.analog.wfm_rcv",
                .canonical_type = std::nullopt,
                .name = "WFM Receive",
                .category = "Analog",
                .summary = "",
                .inputs = {{"in", "complex"}},
                .outputs = {{"out", "float"}},
                .parameters = {},
            },
        };
    }
};

class TestSchedulerCatalogProvider final : public gr4cp::catalog::SchedulerCatalogProvider {
public:
    std::vector<gr4cp::domain::SchedulerDescriptor> list() const override {
        return {
            gr4cp::domain::SchedulerDescriptor{"gr::scheduler::SimpleSingle"},
            gr4cp::domain::SchedulerDescriptor{"gr::scheduler::SimpleMulti"},
            gr4cp::domain::SchedulerDescriptor{"gr::incubator::scheduler::BlockingBackoff"},
        };
    }
};

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

std::string authored_http_poll_graph() {
    return authored_stream_graph(kGenericStreamBlockId, "series0", "http_poll", kSeriesPayloadFormat);
}

std::string authored_websocket_graph() {
    return authored_stream_graph(kGenericStreamBlockId, "series0", "websocket", kSeriesPayloadFormat);
}

std::string alternate_websocket_graph() {
    return authored_stream_graph(kGenericSpectrumBlockId, "spectrum0", "websocket", kSpectrumPayloadFormat);
}

std::string alternate_http_poll_graph() {
    return authored_stream_graph(kGenericMatrixBlockId, "waterfall0", "http_poll", kMatrixPayloadFormat);
}

std::string matrix_websocket_graph() {
    return authored_stream_graph(kGenericMatrixBlockId, "waterfall0", "websocket", kMatrixPayloadFormat);
}

std::string unmanaged_graph() {
    return R"(blocks:
  - id: "gr::testing::NullSink<float32>"
    parameters:
      name: "sink0"
connections: []
)";
}

std::string partially_broken_managed_graph() {
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

std::string graph_without_stream_metadata() {
    return R"(blocks:
  - id: "gr::testing::NullSink<float32>"
    parameters:
      name: "dataset0"
connections: []
)";
}

std::string unsupported_transport_graph() {
    return authored_stream_graph(kGenericSpectrumBlockId, "spectrum0", "grpc", kSpectrumPayloadFormat);
}

std::string compatibility_studio_http_poll_graph() {
    return R"(blocks:
  - id: "gr::basic::SignalGenerator<float32>"
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
  - ["src0", 0, "series0", 0]
)";
}

void expect_single_stream_descriptor(const nlohmann::json& body,
                                     std::string_view id,
                                     std::string_view block_instance_name,
                                     std::string_view transport,
                                     std::string_view payload_format,
                                     const std::string& path) {
    ASSERT_TRUE(body.contains("streams"));
    ASSERT_TRUE(body["streams"].is_array());
    ASSERT_EQ(body["streams"].size(), 1U);
    const auto& stream = body["streams"][0];
    EXPECT_EQ(stream["id"], id);
    EXPECT_EQ(stream["block_instance_name"], block_instance_name);
    EXPECT_EQ(stream["transport"], transport);
    EXPECT_EQ(stream["payload_format"], payload_format);
    EXPECT_EQ(stream["path"], path);
    EXPECT_EQ(stream["ready"], true);
}

class FakeWebSocketSource {
public:
    explicit FakeWebSocketSource(const gr4cp::domain::InternalStreamBinding& binding)
        : host_(binding.host),
          port_(binding.port),
          path_(binding.path),
          acceptor_(io_context_) {
        beast::error_code error;
        const auto address = asio::ip::make_address(host_, error);
        if (error) {
            throw std::runtime_error("invalid websocket source host");
        }

        acceptor_.open(address.is_v6() ? tcp::v6() : tcp::v4(), error);
        if (error) {
            throw std::runtime_error("failed to open websocket source acceptor");
        }
        acceptor_.set_option(asio::socket_base::reuse_address(true), error);
        if (error) {
            throw std::runtime_error("failed to configure websocket source acceptor");
        }
        acceptor_.bind({address, static_cast<unsigned short>(port_)}, error);
        if (error) {
            throw std::runtime_error("failed to bind websocket source");
        }
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        if (error) {
            throw std::runtime_error("failed to listen websocket source");
        }
        acceptor_.non_blocking(true, error);
        if (error) {
            throw std::runtime_error("failed to set websocket source non-blocking mode");
        }

        thread_ = std::jthread([this]() { run(); });
    }

    ~FakeWebSocketSource() {
        stop();
    }

    void send_text(const std::string& payload) {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(mutex_);
            connection = connection_;
        }
        if (!connection) {
            return;
        }

        std::lock_guard write_lock(connection->write_mutex);
        beast::error_code error;
        connection->ws.text(true);
        connection->ws.write(asio::buffer(payload), error);
    }

    bool wait_for_connection(std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this]() { return connection_ != nullptr; });
    }

    void stop() {
        if (stopped_.exchange(true)) {
            return;
        }
        beast::error_code error;
        acceptor_.cancel(error);
        acceptor_.close(error);

        std::shared_ptr<Connection> connection;
        {
            std::lock_guard lock(mutex_);
            connection = connection_;
        }
        if (connection) {
            std::lock_guard write_lock(connection->write_mutex);
            connection->ws.next_layer().socket().shutdown(tcp::socket::shutdown_both, error);
            connection->ws.next_layer().socket().close(error);
        }

        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    struct Connection {
        explicit Connection(tcp::socket socket) : ws(std::move(socket)) {}

        websocket::stream<beast::tcp_stream> ws;
        std::mutex write_mutex;
    };

    void run() {
        while (!stopped_.load()) {
            beast::error_code error;
            tcp::socket socket(io_context_);
            acceptor_.accept(socket, error);
            if (error) {
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                break;
            }

            beast::flat_buffer buffer;
            beast::http::request<beast::http::string_body> request;
            beast::http::read(socket, buffer, request, error);
            if (error || !websocket::is_upgrade(request) || request.target() != path_) {
                socket.shutdown(tcp::socket::shutdown_both, error);
                socket.close(error);
                continue;
            }

            auto connection = std::make_shared<Connection>(std::move(socket));
            connection->ws.accept(request, error);
            if (error) {
                continue;
            }

            {
                std::lock_guard lock(mutex_);
                connection_ = connection;
                condition_.notify_all();
            }

            while (!stopped_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            {
                std::lock_guard lock(mutex_);
                if (connection_ == connection) {
                    connection_.reset();
                }
            }
            condition_.notify_all();
        }
    }

    std::string host_;
    int port_{};
    std::string path_;
    asio::io_context io_context_{1};
    tcp::acceptor acceptor_;
    std::atomic<bool> stopped_{false};
    std::jthread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::shared_ptr<Connection> connection_;
};

class HttpApiTest : public ::testing::Test {
protected:
    class FakeRuntimeManager final : public gr4cp::runtime::RuntimeManager {
    public:
        ~FakeRuntimeManager() override {
            for (auto& [_, source] : websocket_sources) {
                if (source) {
                    source->stop();
                }
            }
        }

        void prepare(const gr4cp::domain::Session& session) override {
            release_streams(session.id);
            if (const auto contract_error = gr4cp::domain::validate_stream_runtime_contract(session); contract_error.has_value()) {
                throw std::runtime_error(*contract_error);
            }
            const auto plan = gr4cp::domain::derive_stream_runtime_plan(session);
            if (!plan.has_value()) {
                return;
            }
            auto& bindings = stream_bindings[session.id];
            for (auto stream_plan : plan->streams) {
                auto internal = stream_plan.transport == "websocket" ? stream_allocator.allocate_websocket()
                                                                      : stream_allocator.allocate_http();
                stream_plan.ready = true;
                bindings.push_back(gr4cp::domain::RuntimeStreamBinding{
                    .plan = stream_plan,
                    .internal = internal,
                    .browser = gr4cp::domain::to_browser_stream_descriptor(stream_plan),
                });
                if (stream_plan.transport == "websocket") {
                    websocket_sources[session.id + "/" + stream_plan.stream_id] =
                        std::make_unique<FakeWebSocketSource>(internal);
                }
            }
        }
        void start(const gr4cp::domain::Session&) override {}
        void stop(const gr4cp::domain::Session& session) override {
            {
                std::lock_guard lock(control_mutex_);
                stop_entered_.insert(session.id);
                control_condition_.notify_all();
            }

            std::unique_lock lock(control_mutex_);
            control_condition_.wait(lock, [this, &session]() { return blocked_stop_sessions_.count(session.id) == 0U; });
            if (stop_exception) {
                std::rethrow_exception(stop_exception);
            }
            release_streams(session.id);
        }
        void destroy(const gr4cp::domain::Session& session) override {
            release_streams(session.id);
            settings.erase(session.id);
        }

        std::optional<gr4cp::domain::StreamRuntimePlan> active_stream_plan(const gr4cp::domain::Session& session) override {
            const auto it = stream_bindings.find(session.id);
            if (it == stream_bindings.end() || it->second.empty()) {
                return std::nullopt;
            }
            gr4cp::domain::StreamRuntimePlan plan;
            for (const auto& binding : it->second) {
                plan.streams.push_back(binding.plan);
            }
            return plan;
        }

        gr4cp::runtime::HttpStreamResponse fetch_http_stream(const gr4cp::domain::Session& session,
                                                             const std::string& stream_id) override {
            const auto it = stream_bindings.find(session.id);
            if (it == stream_bindings.end()) {
                throw std::runtime_error("session stream resources are not available");
            }
            for (const auto& binding : it->second) {
                if (binding.plan.stream_id != stream_id || binding.plan.transport == "websocket") {
                    continue;
                }
                const auto payload_it = stream_payloads.find(session.id + "/" + stream_id);
                return gr4cp::runtime::HttpStreamResponse{
                    .status = 200,
                    .body = payload_it == stream_payloads.end() ? R"({"stream":"ok"})" : payload_it->second,
                    .content_type = "application/json",
                };
            }
            throw std::runtime_error("stream not found in active session resources: " + stream_id);
        }

        gr4cp::runtime::WebSocketStreamRoute resolve_websocket_stream(const gr4cp::domain::Session& session,
                                                                      const std::string& stream_id) override {
            const auto it = stream_bindings.find(session.id);
            if (it == stream_bindings.end()) {
                throw std::runtime_error("session stream resources are not available");
            }
            for (const auto& binding : it->second) {
                if (binding.plan.stream_id == stream_id && binding.plan.transport == "websocket") {
                    return gr4cp::runtime::WebSocketStreamRoute{.internal = binding.internal};
                }
            }
            throw std::runtime_error("stream not found in active session resources: " + stream_id);
        }

        void set_block_settings(const gr4cp::domain::Session& session,
                                const std::string& block_unique_name,
                                const gr::property_map& patch,
                                gr4cp::runtime::BlockSettingsMode mode) override {
            set_calls++;
            last_session_id = session.id;
            last_block = block_unique_name;
            last_mode = mode;
            last_patch = patch;
            if (set_exception) {
                std::rethrow_exception(set_exception);
            }
            auto& block_settings = settings[session.id][block_unique_name];
            for (const auto& [key, value] : patch) {
                block_settings.insert_or_assign(key, value);
            }
        }

        gr::property_map get_block_settings(const gr4cp::domain::Session& session,
                                            const std::string& block_unique_name) override {
            get_calls++;
            last_session_id = session.id;
            last_block = block_unique_name;
            if (get_exception) {
                std::rethrow_exception(get_exception);
            }
            return settings[session.id][block_unique_name];
        }

        int set_calls{};
        int get_calls{};
        std::string last_session_id;
        std::string last_block;
        gr4cp::runtime::BlockSettingsMode last_mode{gr4cp::runtime::BlockSettingsMode::Staged};
        gr::property_map last_patch;
        std::unordered_map<std::string, std::unordered_map<std::string, gr::property_map>> settings;
        std::exception_ptr set_exception;
        std::exception_ptr get_exception;
        std::exception_ptr stop_exception;
        gr4cp::runtime::StreamBindingAllocator stream_allocator;
        std::unordered_map<std::string, std::vector<gr4cp::domain::RuntimeStreamBinding>> stream_bindings;
        std::unordered_map<std::string, std::string> stream_payloads;
        std::unordered_map<std::string, std::unique_ptr<FakeWebSocketSource>> websocket_sources;

        std::optional<int> internal_port_for(const std::string& session_id, const std::string& stream_id) const {
            const auto it = stream_bindings.find(session_id);
            if (it == stream_bindings.end()) {
                return std::nullopt;
            }
            for (const auto& binding : it->second) {
                if (binding.plan.stream_id == stream_id) {
                    return binding.internal.port;
                }
            }
            return std::nullopt;
        }

        bool has_stream_bindings(const std::string& session_id) const {
            const auto it = stream_bindings.find(session_id);
            return it != stream_bindings.end() && !it->second.empty();
        }

        void send_websocket_text(const std::string& session_id, const std::string& stream_id, const std::string& payload) {
            const auto it = websocket_sources.find(session_id + "/" + stream_id);
            if (it != websocket_sources.end() && it->second) {
                it->second->send_text(payload);
            }
        }

        bool wait_for_websocket_connection(const std::string& session_id,
                                           const std::string& stream_id,
                                           std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
            const auto it = websocket_sources.find(session_id + "/" + stream_id);
            return it != websocket_sources.end() && it->second && it->second->wait_for_connection(timeout);
        }

        void block_stop_for(const std::string& session_id) {
            std::lock_guard lock(control_mutex_);
            blocked_stop_sessions_.insert(session_id);
            stop_entered_.erase(session_id);
        }

        void release_stop_for(const std::string& session_id) {
            std::lock_guard lock(control_mutex_);
            blocked_stop_sessions_.erase(session_id);
            control_condition_.notify_all();
        }

        bool wait_for_stop_entry(const std::string& session_id,
                                 std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
            std::unique_lock lock(control_mutex_);
            return control_condition_.wait_for(lock, timeout, [&]() { return stop_entered_.count(session_id) > 0U; });
        }

    private:
        void release_streams(const std::string& session_id) {
            const auto it = stream_bindings.find(session_id);
            if (it == stream_bindings.end()) {
                return;
            }
            for (const auto& binding : it->second) {
                const auto websocket_it = websocket_sources.find(session_id + "/" + binding.plan.stream_id);
                if (websocket_it != websocket_sources.end()) {
                    websocket_it->second->stop();
                    websocket_sources.erase(websocket_it);
                }
                stream_allocator.release(binding.internal);
            }
            stream_bindings.erase(it);
        }

        std::mutex control_mutex_;
        std::condition_variable control_condition_;
        std::unordered_set<std::string> blocked_stop_sessions_;
        std::unordered_set<std::string> stop_entered_;
    };

    static std::string url_encode(std::string_view value) {
        std::ostringstream encoded;
        encoded << std::uppercase << std::hex;
        for (const char ch : value) {
            const auto byte = static_cast<unsigned char>(ch);
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                ch == '_' || ch == '.' || ch == '~') {
                encoded << ch;
            } else {
                encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
            }
        }
        return encoded.str();
    }

    void wait_for_server_ready() {
        httplib::Client probe("127.0.0.1", port);
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (const auto response = probe.Get("/healthz"); response && response->status == 200) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        FAIL() << "server did not become ready";
    }

    void SetUp() override {
        server = std::make_unique<gr4cp::api::HttpServer>(service,
                                                          session_stream_service,
                                                          block_catalog_service,
                                                          scheduler_catalog_service,
                                                          block_settings_service);
        port = server->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        server_thread = std::jthread([this]() { server->listen_after_bind(); });
        wait_for_server_ready();
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    static nlohmann::json parse_json(const httplib::Result& response) {
        EXPECT_TRUE(response);
        return nlohmann::json::parse(response->body);
    }

    static void expect_json_content_type(const httplib::Result& response) {
        ASSERT_TRUE(response);
        EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    }

    nlohmann::json create_session_via_api() {
        const auto response = client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json");
        EXPECT_TRUE(response);
        EXPECT_EQ(response->status, 201);
        return parse_json(response);
    }

    nlohmann::json create_session_with_graph(const std::string& grc) {
        const auto response = client->Post("/sessions",
                                           (R"({"name":"demo","grc":)" + nlohmann::json(grc).dump() + "}").c_str(),
                                           "application/json");
        EXPECT_TRUE(response);
        EXPECT_EQ(response->status, 201);
        return parse_json(response);
    }

    std::string create_running_session() {
        const auto created = create_session_via_api();
        const auto id = created["id"].get<std::string>();
        const auto started = client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json");
        EXPECT_TRUE(started);
        EXPECT_EQ(started->status, 200);
        return id;
    }

    httplib::Result post_with_fresh_client(const std::string& path, const std::string& body = "") {
        httplib::Client local_client("127.0.0.1", port);
        return local_client.Post(path.c_str(), body, "application/json");
    }

    websocket::stream<beast::tcp_stream> connect_websocket(const std::string& path) {
        tcp::resolver resolver(ws_io_context_);
        websocket::stream<beast::tcp_stream> stream(ws_io_context_);
        const auto endpoint = resolver.resolve("127.0.0.1", std::to_string(port));
        beast::get_lowest_layer(stream).connect(endpoint);
        stream.handshake("127.0.0.1:" + std::to_string(port), path);
        return stream;
    }

    static std::string read_websocket_text(websocket::stream<beast::tcp_stream>& stream) {
        beast::flat_buffer buffer;
        stream.read(buffer);
        return beast::buffers_to_string(buffer.data());
    }

    static bool wait_for_websocket_close(websocket::stream<beast::tcp_stream>& stream,
                                         std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
        beast::flat_buffer buffer;
        beast::error_code error;
        beast::get_lowest_layer(stream).expires_after(timeout);
        stream.read(buffer, error);
        beast::get_lowest_layer(stream).expires_never();
        return static_cast<bool>(error);
    }

    std::unique_ptr<gr4cp::api::HttpServer> server;
    gr4cp::storage::InMemorySessionRepository repository;
    FakeRuntimeManager runtime_manager;
    TestBlockCatalogProvider block_catalog_provider;
    TestSchedulerCatalogProvider scheduler_catalog_provider;
    gr4cp::app::SessionService service{repository, runtime_manager};
    gr4cp::app::SessionStreamService session_stream_service;
    gr4cp::app::BlockSettingsService block_settings_service{repository, runtime_manager};
    gr4cp::app::BlockCatalogService block_catalog_service{block_catalog_provider};
    gr4cp::app::SchedulerCatalogService scheduler_catalog_service{scheduler_catalog_provider};
    int port{};
    std::jthread server_thread;
    std::unique_ptr<httplib::Client> client;
    asio::io_context ws_io_context_{1};
};

class RealRuntimeHttpApiTest : public ::testing::Test {
protected:
    void wait_for_server_ready() {
        httplib::Client probe("127.0.0.1", port);
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (const auto response = probe.Get("/healthz"); response && response->status == 200) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        FAIL() << "server did not become ready";
    }

    void SetUp() override {
        try {
            auto probe = gr4cp::domain::Session{};
            probe.id = "probe";
            probe.name = "probe";
            probe.grc_content = unmanaged_graph();
            probe.state = gr4cp::domain::SessionState::Stopped;
            probe.created_at = std::chrono::system_clock::now();
            probe.updated_at = probe.created_at;
            runtime_manager.prepare(probe);
            runtime_manager.destroy(probe);
        } catch (const std::exception& error) {
            GTEST_SKIP() << "GNU Radio 4 runtime unavailable: " << error.what();
        }

        server = std::make_unique<gr4cp::api::HttpServer>(service,
                                                          session_stream_service,
                                                          block_catalog_service,
                                                          scheduler_catalog_service,
                                                          block_settings_service);
        port = server->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        server_thread = std::jthread([this]() { server->listen_after_bind(); });
        wait_for_server_ready();
        client = std::make_unique<httplib::Client>("127.0.0.1", port);
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }

    static nlohmann::json parse_json(const httplib::Result& response) {
        EXPECT_TRUE(response);
        return nlohmann::json::parse(response->body);
    }

    nlohmann::json create_session_with_graph(const std::string& grc) {
        const auto response = client->Post("/sessions",
                                           (R"({"name":"demo","grc":)" + nlohmann::json(grc).dump() + "}").c_str(),
                                           "application/json");
        EXPECT_TRUE(response);
        EXPECT_EQ(response->status, 201);
        return parse_json(response);
    }

    std::unique_ptr<gr4cp::api::HttpServer> server;
    gr4cp::storage::InMemorySessionRepository repository;
    gr4cp::runtime::Gr4RuntimeManager runtime_manager;
    TestBlockCatalogProvider block_catalog_provider;
    TestSchedulerCatalogProvider scheduler_catalog_provider;
    gr4cp::app::SessionService service{repository, runtime_manager};
    gr4cp::app::SessionStreamService session_stream_service;
    gr4cp::app::BlockSettingsService block_settings_service{repository, runtime_manager};
    gr4cp::app::BlockCatalogService block_catalog_service{block_catalog_provider};
    gr4cp::app::SchedulerCatalogService scheduler_catalog_service{scheduler_catalog_provider};
    int port{};
    std::jthread server_thread;
    std::unique_ptr<httplib::Client> client;
};

TEST_F(HttpApiTest, PostSessionsSuccess) {
    const auto response = client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 201);
    expect_json_content_type(response);

    const auto body = parse_json(response);
    EXPECT_TRUE(body["id"].get<std::string>().starts_with("sess_"));
    EXPECT_EQ(body["name"], "demo");
    EXPECT_EQ(body["state"], "stopped");
    EXPECT_TRUE(body["last_error"].is_null());
    EXPECT_TRUE(body.contains("created_at"));
    EXPECT_TRUE(body.contains("updated_at"));
    EXPECT_FALSE(body.contains("grc_content"));
}

TEST_F(RealRuntimeHttpApiTest, CompatibilityStudioSinkHttpRouteReachesRealRuntimeStream) {
    const auto created = create_session_with_graph(compatibility_studio_http_poll_graph());
    const auto id = created["id"].get<std::string>();

    const auto started = client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json");
    ASSERT_TRUE(started);
    ASSERT_EQ(started->status, 200);

    const auto detail = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(detail);
    ASSERT_EQ(detail->status, 200);
    const auto detail_body = parse_json(detail);
    ASSERT_TRUE(detail_body.contains("streams"));
    ASSERT_EQ(detail_body["streams"].size(), 1U);
    EXPECT_EQ(detail_body["streams"][0]["id"], "series0");
    EXPECT_EQ(detail_body["streams"][0]["transport"], "http_poll");
    EXPECT_EQ(detail_body["streams"][0]["path"], "/sessions/" + id + "/streams/series0/http");

    const auto route = client->Get(("/sessions/" + id + "/streams/series0/http").c_str());
    ASSERT_TRUE(route);
    EXPECT_EQ(route->status, 200);
    EXPECT_EQ(route->get_header_value("Content-Type"), "application/json");
    const auto body = nlohmann::json::parse(route->body);
    EXPECT_EQ(body["sample_type"], "float32");
    EXPECT_EQ(body["layout"], "channels_first");
    EXPECT_TRUE(body["channels"].get<int>() >= 1);
    EXPECT_TRUE(body["samples_per_channel"].get<int>() >= 0);
    ASSERT_TRUE(body["data"].is_array());
    ASSERT_FALSE(body["data"].empty());
}

TEST_F(RealRuntimeHttpApiTest, CompatibilityStudioSinkStopRemovesBrowserRoute) {
    const auto created = create_session_with_graph(compatibility_studio_http_poll_graph());
    const auto id = created["id"].get<std::string>();

    const auto started = client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json");
    ASSERT_TRUE(started);
    ASSERT_EQ(started->status, 200);

    const auto before_stop = client->Get(("/sessions/" + id + "/streams/series0/http").c_str());
    ASSERT_TRUE(before_stop);
    EXPECT_EQ(before_stop->status, 200);

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);

    const auto after_stop = client->Get(("/sessions/" + id + "/streams/series0/http").c_str());
    ASSERT_TRUE(after_stop);
    EXPECT_EQ(after_stop->status, 409);
}

TEST_F(HttpApiTest, GetBlocksSuccess) {
    const auto response = client->Get("/blocks");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);

    const auto body = parse_json(response);
    ASSERT_EQ(body.size(), 5U);
    EXPECT_EQ(body[0]["category"], "Analog");
    EXPECT_EQ(body[0]["name"], "WFM Receive");
    EXPECT_TRUE(body[0]["inputs"].is_array());
    EXPECT_TRUE(body[0]["outputs"].is_array());
    EXPECT_TRUE(body[0]["parameters"].is_array());
}

TEST_F(HttpApiTest, GetBlocksDoesNotAdvertiseContentEncodingForIdentityBody) {
    const httplib::Headers headers{{"Accept-Encoding", "br, gzip, deflate"}};
    const auto response = client->Get("/blocks", headers);

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);
    EXPECT_FALSE(response->has_header("Content-Encoding"));

    const auto body = parse_json(response);
    ASSERT_EQ(body.size(), 5U);
}

TEST_F(HttpApiTest, GetBlocksUsesDeterministicOrdering) {
    const auto response = client->Get("/blocks");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    ASSERT_EQ(body.size(), 5U);

    for (std::size_t index = 1; index < body.size(); ++index) {
        const auto& previous = body[index - 1];
        const auto& current = body[index];
        const auto previous_category = previous.at("category").get<std::string>();
        const auto current_category = current.at("category").get<std::string>();
        const auto previous_name = previous.at("name").get<std::string>();
        const auto current_name = current.at("name").get<std::string>();
        const auto previous_id = previous.at("id").get<std::string>();
        const auto current_id = current.at("id").get<std::string>();

        const bool in_order = previous_category < current_category ||
                              (previous_category == current_category &&
                               (previous_name < current_name ||
                                (previous_name == current_name && previous_id < current_id)));
        EXPECT_TRUE(in_order);
    }
}

TEST_F(HttpApiTest, GetBlockByIdSuccess) {
    const auto response = client->Get("/blocks/blocks.math.add_ff");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);

    const auto body = parse_json(response);
    EXPECT_EQ(body["id"], "blocks.math.add_ff");
    EXPECT_EQ(body["name"], "Add");
    EXPECT_EQ(body["category"], "Math");
    ASSERT_EQ(body["inputs"].size(), 2U);
    ASSERT_EQ(body["parameters"].size(), 1U);
    EXPECT_EQ(body["parameters"][0]["default"], 1.0);
}

TEST_F(HttpApiTest, GetBlockByIdIncludesDynamicCollectionMetadata) {
    const auto response = client->Get("/blocks/blocks.math.add_dynamic");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);

    const auto body = parse_json(response);
    ASSERT_EQ(body["inputs"].size(), 1U);
    const auto& input = body["inputs"][0];
    EXPECT_EQ(input["cardinality_kind"], "dynamic");
    EXPECT_EQ(input["current_port_count"], 3);
    EXPECT_EQ(input["render_port_count"], 3);
    EXPECT_EQ(input["min_port_count"], 1);
    EXPECT_EQ(input["max_port_count"], 32);
    EXPECT_EQ(input["size_parameter"], "n_inputs");
    EXPECT_EQ(input["handle_name_template"], "in#${index}");
}

TEST_F(HttpApiTest, GetBlockByIdIncludesExtendedParameterMetadataWhenAvailable) {
    const auto response = client->Get("/blocks/blocks.math.add_ff");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);

    const auto body = parse_json(response);
    ASSERT_EQ(body["parameters"].size(), 1U);
    const auto& parameter = body["parameters"][0];
    ASSERT_TRUE(parameter.contains("summary"));
    ASSERT_TRUE(parameter.contains("runtime_mutability"));
    ASSERT_TRUE(parameter.contains("value_kind"));
    ASSERT_TRUE(parameter.contains("ui_hint"));
    EXPECT_EQ(parameter["summary"], "Scale factor");
    EXPECT_EQ(parameter["runtime_mutability"], "mutable");
    EXPECT_EQ(parameter["value_kind"], "scalar");
    EXPECT_EQ(parameter["ui_hint"], "advanced");
    EXPECT_FALSE(parameter.contains("enum_labels"));
    EXPECT_FALSE(parameter.contains("enum_source"));
    EXPECT_FALSE(parameter.contains("allow_custom_value"));
}

TEST_F(HttpApiTest, GetBlockByIdOmitsUnknownExtendedParameterMetadata) {
    const auto response = client->Get("/blocks/blocks.sources.signal_source_f");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);

    const auto body = parse_json(response);
    ASSERT_EQ(body["parameters"].size(), 1U);
    const auto& parameter = body["parameters"][0];
    EXPECT_FALSE(parameter.contains("runtime_mutability"));
    EXPECT_FALSE(parameter.contains("value_kind"));
    EXPECT_FALSE(parameter.contains("enum_labels"));
    EXPECT_FALSE(parameter.contains("enum_source"));
    EXPECT_FALSE(parameter.contains("ui_hint"));
    EXPECT_FALSE(parameter.contains("allow_custom_value"));
}

TEST_F(HttpApiTest, GetBlockByEncodedIdSuccess) {
    const auto id = "gr::blocks::math::Add<float32, std::plus<float32>>";
    const auto response = client->Get(("/blocks/" + url_encode(id)).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);

    const auto body = parse_json(response);
    EXPECT_EQ(body["id"], id);
    ASSERT_EQ(body["inputs"].size(), 1U);
    ASSERT_TRUE(body["inputs"][0].contains("cardinality_kind"));
    ASSERT_TRUE(body["inputs"][0].contains("current_port_count"));
    EXPECT_EQ(body["inputs"][0]["cardinality_kind"], "dynamic");
    EXPECT_EQ(body["inputs"][0]["current_port_count"], 2);
    ASSERT_EQ(body["parameters"].size(), 1U);
}

TEST_F(HttpApiTest, ListedBlockIdsRoundTripIntoDetails) {
    const auto listed = client->Get("/blocks");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->status, 200);
    const auto blocks = parse_json(listed);
    ASSERT_FALSE(blocks.empty());

    for (const auto& block : blocks) {
        const auto id = block["id"].get<std::string>();
        const auto detail = client->Get(("/blocks/" + url_encode(id)).c_str());
        ASSERT_TRUE(detail) << id;
        EXPECT_EQ(detail->status, 200) << id;
    }
}

TEST_F(HttpApiTest, GetBlockByIdNotFoundReturns404) {
    const auto response = client->Get("/blocks/blocks.missing");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["error"]["code"], "not_found");
    EXPECT_FALSE(body["error"]["message"].get<std::string>().empty());
}

TEST_F(HttpApiTest, PostSessionsWithMissingGrcReturnsBadRequest) {
    const auto response = client->Post("/sessions", R"({"name":"demo"})", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    expect_json_content_type(response);

    const auto body = parse_json(response);
    EXPECT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"]["code"], "validation_error");
    EXPECT_FALSE(body["error"]["message"].get<std::string>().empty());
}

TEST_F(HttpApiTest, PostSessionsWithMalformedJsonReturnsBadRequest) {
    const auto response = client->Post("/sessions", R"({"name":"demo","grc":)", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    expect_json_content_type(response);

    const auto body = parse_json(response);
    EXPECT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"]["code"], "validation_error");
    EXPECT_EQ(body["error"]["message"], "request body must be valid JSON");
}

TEST_F(HttpApiTest, PostSessionsWithoutNameUsesEmptyString) {
    const auto response = client->Post("/sessions", R"({"grc":"flowgraph"})", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 201);
    expect_json_content_type(response);
    EXPECT_EQ(parse_json(response)["name"], "");
}

TEST_F(HttpApiTest, GetSessionsWhenEmpty) {
    const auto response = client->Get("/sessions");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);
    EXPECT_EQ(parse_json(response), nlohmann::json::array());
}

TEST_F(HttpApiTest, GetSessionsAfterCreate) {
    const auto created = client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json");
    ASSERT_TRUE(created);

    const auto response = client->Get("/sessions");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);

    const auto body = parse_json(response);
    ASSERT_EQ(body.size(), 1U);
    EXPECT_EQ(body[0]["name"], "demo");
    EXPECT_EQ(body[0]["state"], "stopped");
}

TEST_F(HttpApiTest, GetSessionByIdSuccess) {
    const auto created = parse_json(client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json"));
    const auto response = client->Get(("/sessions/" + created["id"].get<std::string>()).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["id"], created["id"]);
    EXPECT_EQ(body["state"], "stopped");
    EXPECT_TRUE(body["last_error"].is_null());
}

TEST_F(HttpApiTest, AuthoredHttpStreamAppearsOnGetSession) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get(("/sessions/" + id).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    expect_single_stream_descriptor(
        body, "series0", "series0", "http_poll", "series-window-json-v1", "/sessions/" + id + "/streams/series0/http");
}

TEST_F(HttpApiTest, AuthoredHttpStreamAppearsInSessionsListWithStreamsMetadata) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get("/sessions");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    ASSERT_EQ(body.size(), 1U);
    ASSERT_TRUE(body[0].contains("streams"));
    ASSERT_EQ(body[0]["streams"].size(), 1U);
    EXPECT_EQ(body[0]["streams"][0]["id"], "series0");
    EXPECT_EQ(body[0]["streams"][0]["transport"], "http_poll");
}

TEST_F(HttpApiTest, UnsupportedGraphOmitsStreamsMetadata) {
    const auto created = create_session_with_graph(unmanaged_graph());
    const auto id = created["id"].get<std::string>();

    const auto detail = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(detail);
    EXPECT_FALSE(parse_json(detail).contains("streams"));

    const auto listed = client->Get("/sessions");
    ASSERT_TRUE(listed);
    ASSERT_EQ(parse_json(listed).size(), 1U);
    EXPECT_FALSE(parse_json(listed)[0].contains("streams"));
}

TEST_F(HttpApiTest, CompatibilityFlattenedStreamMetadataStillLoadsAndDerivesStreams) {
    const auto created = create_session_with_graph(compatibility_flattened_stream_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get(("/sessions/" + id).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    ASSERT_TRUE(body.contains("streams"));
    ASSERT_EQ(body["streams"].size(), 1U);
    EXPECT_EQ(body["streams"][0]["id"], "series0");
}

TEST_F(HttpApiTest, StreamDescriptorDerivationIsStableAcrossReads) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto first = parse_json(client->Get(("/sessions/" + id).c_str()));
    const auto second = parse_json(client->Get(("/sessions/" + id).c_str()));

    ASSERT_TRUE(first.contains("streams"));
    ASSERT_TRUE(second.contains("streams"));
    EXPECT_EQ(first["streams"], second["streams"]);
}

TEST_F(HttpApiTest, PartialAuthoredStreamMetadataOmitsStreamsInsteadOfEmittingPartialContract) {
    const auto created = create_session_with_graph(partially_broken_managed_graph());
    const auto id = created["id"].get<std::string>();

    const auto response = client->Get(("/sessions/" + id).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_FALSE(parse_json(response).contains("streams"));
}

TEST_F(HttpApiTest, GraphWithoutStreamMetadataStaysOnLegacyNoStreamsPath) {
    const auto created = create_session_with_graph(graph_without_stream_metadata());
    const auto id = created["id"].get<std::string>();

    const auto started = client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json");
    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 200);

    const auto detail = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(detail);
    EXPECT_FALSE(parse_json(detail).contains("streams"));
}

TEST_F(HttpApiTest, UnsupportedAuthoredTransportFailsExplicitlyOnStart) {
    const auto created = create_session_with_graph(unsupported_transport_graph());
    const auto id = created["id"].get<std::string>();

    const auto started = client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json");

    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 500);
    const auto error = parse_json(started);
    EXPECT_EQ(error["error"]["code"], "runtime_error");
    EXPECT_NE(error["error"]["message"].get<std::string>().find("managed stream transport is not supported"), std::string::npos);

    const auto detail = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(detail);
    const auto body = parse_json(detail);
    EXPECT_EQ(body["state"], "error");
    EXPECT_FALSE(body.contains("streams"));
}

TEST_F(HttpApiTest, AuthoredStreamPlanningDoesNotChangeLifecycleBehavior) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();

    const auto started = client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json");
    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 200);
    EXPECT_EQ(parse_json(started)["state"], "running");

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    EXPECT_EQ(stopped->status, 200);
    EXPECT_EQ(parse_json(stopped)["state"], "stopped");
}

TEST_F(HttpApiTest, AuthoredStreamsAreOmittedUntilSessionIsRunning) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();

    const auto response = client->Get(("/sessions/" + id).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_FALSE(parse_json(response).contains("streams"));
}

TEST_F(HttpApiTest, BrowserFacingHttpRouteProxiesActiveSessionStream) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    runtime_manager.stream_payloads[id + "/series0"] = R"({"samples":[1,2,3]})";
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get(("/sessions/" + id + "/streams/series0/http").c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, R"({"samples":[1,2,3]})");
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
}

TEST_F(HttpApiTest, AuthoredHttpStreamDescriptorUsesHttpRouteShapeForDifferentPayloadContract) {
    const auto created = create_session_with_graph(alternate_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get(("/sessions/" + id).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    expect_single_stream_descriptor(body,
                                    "waterfall0",
                                    "waterfall0",
                                    "http_poll",
                                    "waterfall-spectrum-json-v1",
                                    "/sessions/" + id + "/streams/waterfall0/http");
}

TEST_F(HttpApiTest, AuthoredWebsocketStreamDescriptorUsesWsRouteShape) {
    const auto created = create_session_with_graph(authored_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get(("/sessions/" + id).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    expect_single_stream_descriptor(
        body, "series0", "series0", "websocket", "series-window-json-v1", "/sessions/" + id + "/streams/series0/ws");
}

TEST_F(HttpApiTest, AuthoredWebsocketStreamDescriptorPreservesPayloadFormatAcrossBlockIds) {
    const auto created = create_session_with_graph(alternate_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get(("/sessions/" + id).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    expect_single_stream_descriptor(
        body, "spectrum0", "spectrum0", "websocket", "dataset-xy-json-v1", "/sessions/" + id + "/streams/spectrum0/ws");
}

TEST_F(HttpApiTest, AuthoredWebsocketStreamDescriptorSupportsAlternatePayloadContracts) {
    const auto created = create_session_with_graph(matrix_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get(("/sessions/" + id).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    expect_single_stream_descriptor(body,
                                    "waterfall0",
                                    "waterfall0",
                                    "websocket",
                                    "waterfall-spectrum-json-v1",
                                    "/sessions/" + id + "/streams/waterfall0/ws");
}

TEST_F(HttpApiTest, BrowserFacingHttpRouteProxiesAlternatePayloadContractVerbatim) {
    const auto created = create_session_with_graph(alternate_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    runtime_manager.stream_payloads[id + "/waterfall0"] =
        R"({"payload_format":"waterfall-spectrum-json-v1","layout":"waterfall_matrix","rows":1,"cols":2,"data":[[1.0,2.0]]})";
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Get(("/sessions/" + id + "/streams/waterfall0/http").c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body,
              R"({"payload_format":"waterfall-spectrum-json-v1","layout":"waterfall_matrix","rows":1,"cols":2,"data":[[1.0,2.0]]})");
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
}

TEST_F(HttpApiTest, BrowserFacingWebsocketRouteUpgradesAndProxiesFrames) {
    const auto created = create_session_with_graph(authored_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    auto stream = connect_websocket("/sessions/" + id + "/streams/series0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "series0"));

    runtime_manager.send_websocket_text(id, "series0", R"({"samples":[1,2,3]})");

    EXPECT_EQ(read_websocket_text(stream), R"({"samples":[1,2,3]})");
    ASSERT_TRUE(runtime_manager.internal_port_for(id, "series0").has_value());
}

TEST_F(HttpApiTest, BrowserFacingWebsocketRouteProxiesAlternatePayloadFramesVerbatim) {
    const auto created = create_session_with_graph(alternate_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    auto stream = connect_websocket("/sessions/" + id + "/streams/spectrum0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "spectrum0"));

    runtime_manager.send_websocket_text(id, "spectrum0", R"({"bins":[[0.1,-42.0],[0.2,-41.5]]})");

    EXPECT_EQ(read_websocket_text(stream), R"({"bins":[[0.1,-42.0],[0.2,-41.5]]})");
    ASSERT_TRUE(runtime_manager.internal_port_for(id, "spectrum0").has_value());
}

TEST_F(HttpApiTest, BrowserFacingWebsocketRouteAlsoProxiesMatrixPayloadFramesVerbatim) {
    const auto created = create_session_with_graph(matrix_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    auto stream = connect_websocket("/sessions/" + id + "/streams/waterfall0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "waterfall0"));

    runtime_manager.send_websocket_text(
        id,
        "waterfall0",
        R"({"payload_format":"waterfall-spectrum-json-v1","layout":"waterfall_matrix","rows":1,"cols":2,"data":[[1.0,2.0]]})");

    EXPECT_EQ(read_websocket_text(stream),
              R"({"payload_format":"waterfall-spectrum-json-v1","layout":"waterfall_matrix","rows":1,"cols":2,"data":[[1.0,2.0]]})");
    ASSERT_TRUE(runtime_manager.internal_port_for(id, "waterfall0").has_value());
}

TEST_F(HttpApiTest, TwoAuthoredHttpStreamSessionsRunConcurrentlyWithDistinctBindingsAndRoutes) {
    const auto first = create_session_with_graph(authored_http_poll_graph());
    const auto second = create_session_with_graph(authored_http_poll_graph());
    const auto first_id = first["id"].get<std::string>();
    const auto second_id = second["id"].get<std::string>();
    runtime_manager.stream_payloads[first_id + "/series0"] = R"({"session":"first"})";
    runtime_manager.stream_payloads[second_id + "/series0"] = R"({"session":"second"})";

    ASSERT_TRUE(client->Post(("/sessions/" + first_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(client->Post(("/sessions/" + second_id + "/start").c_str(), "", "application/json"));

    ASSERT_TRUE(runtime_manager.internal_port_for(first_id, "series0").has_value());
    ASSERT_TRUE(runtime_manager.internal_port_for(second_id, "series0").has_value());
    EXPECT_NE(*runtime_manager.internal_port_for(first_id, "series0"),
              *runtime_manager.internal_port_for(second_id, "series0"));

    const auto first_route = client->Get(("/sessions/" + first_id + "/streams/series0/http").c_str());
    const auto second_route = client->Get(("/sessions/" + second_id + "/streams/series0/http").c_str());
    ASSERT_TRUE(first_route);
    ASSERT_TRUE(second_route);
    EXPECT_EQ(first_route->body, R"({"session":"first"})");
    EXPECT_EQ(second_route->body, R"({"session":"second"})");
}

TEST_F(HttpApiTest, StoppingAuthoredHttpStreamSessionRemovesBrowserStreamAvailability) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto before_stop = client->Get(("/sessions/" + id + "/streams/series0/http").c_str());
    ASSERT_TRUE(before_stop);
    EXPECT_EQ(before_stop->status, 200);

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);

    const auto after_stop = client->Get(("/sessions/" + id + "/streams/series0/http").c_str());
    ASSERT_TRUE(after_stop);
    EXPECT_EQ(after_stop->status, 409);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(id));
}

TEST_F(HttpApiTest, StoppingAlternateHttpPayloadSessionRemovesBrowserStreamAvailability) {
    const auto created = create_session_with_graph(alternate_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto before_stop = client->Get(("/sessions/" + id + "/streams/waterfall0/http").c_str());
    ASSERT_TRUE(before_stop);
    EXPECT_EQ(before_stop->status, 200);

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);

    const auto after_stop = client->Get(("/sessions/" + id + "/streams/waterfall0/http").c_str());
    ASSERT_TRUE(after_stop);
    EXPECT_EQ(after_stop->status, 409);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(id));
}

TEST_F(HttpApiTest, StoppingMatrixWebsocketStreamSessionRemovesBrowserStreamAvailability) {
    const auto created = create_session_with_graph(matrix_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto before_stop = client->Get(("/sessions/" + id + "/streams/waterfall0/ws").c_str());
    ASSERT_TRUE(before_stop);
    EXPECT_EQ(before_stop->status, 426);

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);

    const auto after_stop = client->Get(("/sessions/" + id + "/streams/waterfall0/ws").c_str());
    ASSERT_TRUE(after_stop);
    EXPECT_EQ(after_stop->status, 409);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(id));
}

TEST_F(HttpApiTest, StoppingAuthoredWebsocketStreamSessionRemovesBrowserStreamAvailability) {
    const auto created = create_session_with_graph(authored_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto before_stop = client->Get(("/sessions/" + id + "/streams/series0/ws").c_str());
    ASSERT_TRUE(before_stop);
    EXPECT_EQ(before_stop->status, 426);

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);

    const auto after_stop = client->Get(("/sessions/" + id + "/streams/series0/ws").c_str());
    ASSERT_TRUE(after_stop);
    EXPECT_EQ(after_stop->status, 409);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(id));
}

TEST_F(HttpApiTest, NonRunningAuthoredWebsocketStreamOmitsStreamsAndRejectsWebsocketRoute) {
    const auto created = create_session_with_graph(alternate_websocket_graph());
    const auto id = created["id"].get<std::string>();

    const auto detail = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(detail);
    EXPECT_FALSE(parse_json(detail).contains("streams"));

    const auto route = client->Get(("/sessions/" + id + "/streams/spectrum0/ws").c_str());
    ASSERT_TRUE(route);
    EXPECT_EQ(route->status, 409);
    EXPECT_EQ(parse_json(route)["error"]["code"], "invalid_state");
}

TEST_F(HttpApiTest, RestartInvalidatesOldWebsocketBindingAndNewRouteUsesNewGeneration) {
    const auto created = create_session_with_graph(authored_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    const auto first_port = runtime_manager.internal_port_for(id, "series0");
    ASSERT_TRUE(first_port.has_value());

    auto old_stream = connect_websocket("/sessions/" + id + "/streams/series0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "series0"));
    runtime_manager.send_websocket_text(id, "series0", "generation-1");
    EXPECT_EQ(read_websocket_text(old_stream), "generation-1");

    const auto restarted = client->Post(("/sessions/" + id + "/restart").c_str(), "", "application/json");
    ASSERT_TRUE(restarted);
    ASSERT_EQ(restarted->status, 200);
    EXPECT_TRUE(wait_for_websocket_close(old_stream));

    const auto second_port = runtime_manager.internal_port_for(id, "series0");
    ASSERT_TRUE(second_port.has_value());
    EXPECT_NE(*first_port, *second_port);

    auto new_stream = connect_websocket("/sessions/" + id + "/streams/series0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "series0"));
    runtime_manager.send_websocket_text(id, "series0", "generation-2");
    EXPECT_EQ(read_websocket_text(new_stream), "generation-2");
}

TEST_F(HttpApiTest, RestartInvalidatesOldAlternateWebsocketBindingAndNewRouteUsesNewGeneration) {
    const auto created = create_session_with_graph(alternate_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    const auto first_port = runtime_manager.internal_port_for(id, "spectrum0");
    ASSERT_TRUE(first_port.has_value());

    auto old_stream = connect_websocket("/sessions/" + id + "/streams/spectrum0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "spectrum0"));
    runtime_manager.send_websocket_text(id, "spectrum0", "generation-1");
    EXPECT_EQ(read_websocket_text(old_stream), "generation-1");

    const auto restarted = client->Post(("/sessions/" + id + "/restart").c_str(), "", "application/json");
    ASSERT_TRUE(restarted);
    ASSERT_EQ(restarted->status, 200);
    EXPECT_TRUE(wait_for_websocket_close(old_stream));

    const auto second_port = runtime_manager.internal_port_for(id, "spectrum0");
    ASSERT_TRUE(second_port.has_value());
    EXPECT_NE(*first_port, *second_port);

    auto new_stream = connect_websocket("/sessions/" + id + "/streams/spectrum0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "spectrum0"));
    runtime_manager.send_websocket_text(id, "spectrum0", "generation-2");
    EXPECT_EQ(read_websocket_text(new_stream), "generation-2");
}

TEST_F(HttpApiTest, RestartInvalidatesOldMatrixWebsocketBindingAndNewRouteUsesNewGeneration) {
    const auto created = create_session_with_graph(matrix_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    const auto first_port = runtime_manager.internal_port_for(id, "waterfall0");
    ASSERT_TRUE(first_port.has_value());

    auto old_stream = connect_websocket("/sessions/" + id + "/streams/waterfall0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "waterfall0"));
    runtime_manager.send_websocket_text(id, "waterfall0", "generation-1");
    EXPECT_EQ(read_websocket_text(old_stream), "generation-1");

    const auto restarted = client->Post(("/sessions/" + id + "/restart").c_str(), "", "application/json");
    ASSERT_TRUE(restarted);
    ASSERT_EQ(restarted->status, 200);
    EXPECT_TRUE(wait_for_websocket_close(old_stream));

    const auto second_port = runtime_manager.internal_port_for(id, "waterfall0");
    ASSERT_TRUE(second_port.has_value());
    EXPECT_NE(*first_port, *second_port);

    auto new_stream = connect_websocket("/sessions/" + id + "/streams/waterfall0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "waterfall0"));
    runtime_manager.send_websocket_text(id, "waterfall0", "generation-2");
    EXPECT_EQ(read_websocket_text(new_stream), "generation-2");
}

TEST_F(HttpApiTest, StopClosesExistingWebsocketConnectionsDeterministically) {
    const auto created = create_session_with_graph(authored_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    auto stream = connect_websocket("/sessions/" + id + "/streams/series0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "series0"));

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);
    EXPECT_TRUE(wait_for_websocket_close(stream));
}

TEST_F(HttpApiTest, StopClosesExistingAlternateWebsocketConnectionsDeterministically) {
    const auto created = create_session_with_graph(alternate_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    auto stream = connect_websocket("/sessions/" + id + "/streams/spectrum0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "spectrum0"));

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);
    EXPECT_TRUE(wait_for_websocket_close(stream));
}

TEST_F(HttpApiTest, StopClosesExistingMatrixWebsocketConnectionsDeterministically) {
    const auto created = create_session_with_graph(matrix_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    auto stream = connect_websocket("/sessions/" + id + "/streams/waterfall0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(id, "waterfall0"));

    const auto stopped = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);
    EXPECT_TRUE(wait_for_websocket_close(stream));
}

TEST_F(HttpApiTest, RestartKeepsBrowserRouteShapeAndAllocatesFreshBinding) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    const auto first_port = runtime_manager.internal_port_for(id, "series0");
    ASSERT_TRUE(first_port.has_value());

    const auto restarted = client->Post(("/sessions/" + id + "/restart").c_str(), "", "application/json");
    ASSERT_TRUE(restarted);
    EXPECT_EQ(restarted->status, 200);

    const auto body = parse_json(client->Get(("/sessions/" + id).c_str()));
    ASSERT_TRUE(body.contains("streams"));
    ASSERT_EQ(body["streams"].size(), 1U);
    EXPECT_EQ(body["streams"][0]["path"], "/sessions/" + id + "/streams/series0/http");
    ASSERT_TRUE(runtime_manager.internal_port_for(id, "series0").has_value());
    EXPECT_TRUE(runtime_manager.has_stream_bindings(id));
}

TEST_F(HttpApiTest, RestartMatrixWebsocketRouteKeepsBrowserRouteShapeAndAllocatesFreshBinding) {
    const auto created = create_session_with_graph(matrix_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    const auto first_port = runtime_manager.internal_port_for(id, "waterfall0");
    ASSERT_TRUE(first_port.has_value());

    const auto restarted = client->Post(("/sessions/" + id + "/restart").c_str(), "", "application/json");
    ASSERT_TRUE(restarted);
    EXPECT_EQ(restarted->status, 200);

    const auto body = parse_json(client->Get(("/sessions/" + id).c_str()));
    ASSERT_TRUE(body.contains("streams"));
    ASSERT_EQ(body["streams"].size(), 1U);
    EXPECT_EQ(body["streams"][0]["path"], "/sessions/" + id + "/streams/waterfall0/ws");
    const auto second_port = runtime_manager.internal_port_for(id, "waterfall0");
    ASSERT_TRUE(second_port.has_value());
    EXPECT_NE(*first_port, *second_port);
}

TEST_F(HttpApiTest, RestartWebsocketRouteKeepsBrowserRouteShapeAndAllocatesFreshBinding) {
    const auto created = create_session_with_graph(authored_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    const auto first_port = runtime_manager.internal_port_for(id, "series0");
    ASSERT_TRUE(first_port.has_value());

    const auto restarted = client->Post(("/sessions/" + id + "/restart").c_str(), "", "application/json");
    ASSERT_TRUE(restarted);
    EXPECT_EQ(restarted->status, 200);

    const auto body = parse_json(client->Get(("/sessions/" + id).c_str()));
    ASSERT_TRUE(body.contains("streams"));
    ASSERT_EQ(body["streams"].size(), 1U);
    EXPECT_EQ(body["streams"][0]["path"], "/sessions/" + id + "/streams/series0/ws");
    const auto second_port = runtime_manager.internal_port_for(id, "series0");
    ASSERT_TRUE(second_port.has_value());
    EXPECT_NE(*first_port, *second_port);
}

TEST_F(HttpApiTest, StopFailureLeavesHttpStreamBindingsUnreleasedAndSessionErrorIsExplicit) {
    const auto created = create_session_with_graph(authored_http_poll_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    runtime_manager.stop_exception =
        std::make_exception_ptr(std::runtime_error("teardown timed out after 2000 ms"));

    const auto response = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_TRUE(runtime_manager.has_stream_bindings(id));
    const auto session_response = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(session_response);
    EXPECT_EQ(parse_json(session_response)["state"], "error");
}

TEST_F(HttpApiTest, StopFailureLeavesWebsocketStreamBindingsUnreleasedAndSessionErrorIsExplicit) {
    const auto created = create_session_with_graph(authored_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    runtime_manager.stop_exception =
        std::make_exception_ptr(std::runtime_error("teardown timed out after 2000 ms"));

    const auto response = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_TRUE(runtime_manager.has_stream_bindings(id));
    const auto session_response = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(session_response);
    EXPECT_EQ(parse_json(session_response)["state"], "error");
}

TEST_F(HttpApiTest, StopFailureLeavesAlternateWebsocketBindingsUnreleasedAndSessionErrorIsExplicit) {
    const auto created = create_session_with_graph(alternate_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    runtime_manager.stop_exception =
        std::make_exception_ptr(std::runtime_error("teardown timed out after 2000 ms"));

    const auto response = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_TRUE(runtime_manager.has_stream_bindings(id));
    const auto session_response = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(session_response);
    EXPECT_EQ(parse_json(session_response)["state"], "error");
}

TEST_F(HttpApiTest, StopFailureLeavesMatrixWebsocketBindingsUnreleasedAndSessionErrorIsExplicit) {
    const auto created = create_session_with_graph(matrix_websocket_graph());
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));
    runtime_manager.stop_exception =
        std::make_exception_ptr(std::runtime_error("teardown timed out after 2000 ms"));

    const auto response = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_TRUE(runtime_manager.has_stream_bindings(id));
    const auto session_response = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(session_response);
    EXPECT_EQ(parse_json(session_response)["state"], "error");
}

TEST_F(HttpApiTest, HttpBindingReleasedBeforeStartingNextGraphWithSameAuthoredContract) {
    const auto first = create_session_with_graph(authored_http_poll_graph());
    const auto first_id = first["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + first_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(runtime_manager.internal_port_for(first_id, "series0").has_value());

    const auto stopped = client->Post(("/sessions/" + first_id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(first_id));

    const auto second = create_session_with_graph(authored_http_poll_graph());
    const auto second_id = second["id"].get<std::string>();
    const auto started = client->Post(("/sessions/" + second_id + "/start").c_str(), "", "application/json");

    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 200);
    EXPECT_TRUE(runtime_manager.internal_port_for(second_id, "series0").has_value());
    const auto route = client->Get(("/sessions/" + second_id + "/streams/series0/http").c_str());
    ASSERT_TRUE(route);
    EXPECT_EQ(route->status, 200);
}

TEST_F(HttpApiTest, AlternateHttpBindingReleasedBeforeStartingNextGraphWithSameAuthoredContract) {
    const auto first = create_session_with_graph(alternate_http_poll_graph());
    const auto first_id = first["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + first_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(runtime_manager.internal_port_for(first_id, "waterfall0").has_value());

    const auto stopped = client->Post(("/sessions/" + first_id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(first_id));

    const auto second = create_session_with_graph(alternate_http_poll_graph());
    const auto second_id = second["id"].get<std::string>();
    const auto started = client->Post(("/sessions/" + second_id + "/start").c_str(), "", "application/json");

    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 200);
    EXPECT_TRUE(runtime_manager.internal_port_for(second_id, "waterfall0").has_value());
    const auto route = client->Get(("/sessions/" + second_id + "/streams/waterfall0/http").c_str());
    ASSERT_TRUE(route);
    EXPECT_EQ(route->status, 200);
}

TEST_F(HttpApiTest, MatrixWebsocketBindingReleasedBeforeStartingNextGraphWithSameAuthoredContract) {
    const auto first = create_session_with_graph(matrix_websocket_graph());
    const auto first_id = first["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + first_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(runtime_manager.internal_port_for(first_id, "waterfall0").has_value());

    const auto stopped = client->Post(("/sessions/" + first_id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(first_id));

    const auto second = create_session_with_graph(matrix_websocket_graph());
    const auto second_id = second["id"].get<std::string>();
    const auto started = client->Post(("/sessions/" + second_id + "/start").c_str(), "", "application/json");

    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 200);
    EXPECT_TRUE(runtime_manager.internal_port_for(second_id, "waterfall0").has_value());
    auto route = connect_websocket("/sessions/" + second_id + "/streams/waterfall0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(second_id, "waterfall0"));
    beast::error_code error;
    route.close(websocket::close_code::normal, error);
    EXPECT_FALSE(error);
}

TEST_F(HttpApiTest, WebsocketBindingReleasedBeforeStartingNextGraphWithSameAuthoredContract) {
    const auto first = create_session_with_graph(authored_websocket_graph());
    const auto first_id = first["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + first_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(runtime_manager.internal_port_for(first_id, "series0").has_value());

    const auto stopped = client->Post(("/sessions/" + first_id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(first_id));

    const auto second = create_session_with_graph(authored_websocket_graph());
    const auto second_id = second["id"].get<std::string>();
    const auto started = client->Post(("/sessions/" + second_id + "/start").c_str(), "", "application/json");

    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 200);
    EXPECT_TRUE(runtime_manager.internal_port_for(second_id, "series0").has_value());
    const auto route = client->Get(("/sessions/" + second_id + "/streams/series0/ws").c_str());
    ASSERT_TRUE(route);
    EXPECT_EQ(route->status, 426);
}

TEST_F(HttpApiTest, AlternateWebsocketBindingReleasedBeforeStartingNextGraphWithSameAuthoredContract) {
    const auto first = create_session_with_graph(alternate_websocket_graph());
    const auto first_id = first["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + first_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(runtime_manager.internal_port_for(first_id, "spectrum0").has_value());

    const auto stopped = client->Post(("/sessions/" + first_id + "/stop").c_str(), "", "application/json");
    ASSERT_TRUE(stopped);
    ASSERT_EQ(stopped->status, 200);
    EXPECT_FALSE(runtime_manager.has_stream_bindings(first_id));

    const auto second = create_session_with_graph(alternate_websocket_graph());
    const auto second_id = second["id"].get<std::string>();
    const auto started = client->Post(("/sessions/" + second_id + "/start").c_str(), "", "application/json");

    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 200);
    EXPECT_TRUE(runtime_manager.internal_port_for(second_id, "spectrum0").has_value());
    auto route = connect_websocket("/sessions/" + second_id + "/streams/spectrum0/ws");
    ASSERT_TRUE(runtime_manager.wait_for_websocket_connection(second_id, "spectrum0"));
    beast::error_code error;
    route.close(websocket::close_code::normal, error);
    EXPECT_FALSE(error);
}

TEST_F(HttpApiTest, RunningUnsupportedGraphKeepsLegacyNoStreamBehavior) {
    const auto created = create_session_with_graph(unmanaged_graph());
    const auto id = created["id"].get<std::string>();

    const auto started = client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json");
    ASSERT_TRUE(started);
    EXPECT_EQ(started->status, 200);

    const auto detail = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(detail);
    EXPECT_FALSE(parse_json(detail).contains("streams"));
    const auto route = client->Get(("/sessions/" + id + "/streams/series0/http").c_str());
    ASSERT_TRUE(route);
    EXPECT_EQ(route->status, 500);
}

TEST_F(HttpApiTest, GetSessionByIdNotFoundReturns404) {
    const auto response = client->Get("/sessions/sess_missing");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["error"]["code"], "not_found");
    EXPECT_FALSE(body["error"]["message"].get<std::string>().empty());
}

TEST_F(HttpApiTest, StartSessionSuccess) {
    const auto created = parse_json(client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json"));
    const auto response = client->Post(("/sessions/" + created["id"].get<std::string>() + "/start").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["state"], "running");
    EXPECT_TRUE(body["last_error"].is_null());
}

TEST_F(HttpApiTest, StartSessionWhenAlreadyRunningReturnsConflict) {
    const auto created = parse_json(client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json"));
    const auto path = "/sessions/" + created["id"].get<std::string>() + "/start";
    const auto first = client->Post(path.c_str(), "", "application/json");
    ASSERT_TRUE(first);

    const auto response = client->Post(path.c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 409);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["error"]["code"], "invalid_state");
    EXPECT_FALSE(body["error"]["message"].get<std::string>().empty());
}

TEST_F(HttpApiTest, StopSessionSuccess) {
    const auto created = parse_json(client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json"));
    const auto id = created["id"].get<std::string>();
    ASSERT_TRUE(client->Post(("/sessions/" + id + "/start").c_str(), "", "application/json"));

    const auto response = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["state"], "stopped");
    EXPECT_TRUE(body["last_error"].is_null());
}

TEST_F(HttpApiTest, StopSessionFromStoppedIsNoOpSuccess) {
    const auto created = parse_json(client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json"));
    const auto response = client->Post(("/sessions/" + created["id"].get<std::string>() + "/stop").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["state"], "stopped");
    EXPECT_TRUE(body["last_error"].is_null());
}

TEST_F(HttpApiTest, GetSessionsRemainsResponsiveDuringSlowStop) {
    const auto id = create_running_session();
    runtime_manager.block_stop_for(id);

    auto stop_future = std::async(std::launch::async, [this, id]() {
        return post_with_fresh_client("/sessions/" + id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(id));

    const auto started = std::chrono::steady_clock::now();
    const auto response = client->Get("/sessions");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_LT(elapsed, std::chrono::milliseconds(300));

    runtime_manager.release_stop_for(id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, SlowAlternateHttpStopDoesNotBlockUnrelatedReadsOrAuthoredHttpOperations) {
    const auto waterfall_created = create_session_with_graph(alternate_http_poll_graph());
    const auto http_created = create_session_with_graph(authored_http_poll_graph());
    const auto waterfall_id = waterfall_created["id"].get<std::string>();
    const auto http_id = http_created["id"].get<std::string>();
    runtime_manager.stream_payloads[http_id + "/series0"] = R"({"session":"http"})";

    ASSERT_TRUE(client->Post(("/sessions/" + waterfall_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(client->Post(("/sessions/" + http_id + "/start").c_str(), "", "application/json"));

    runtime_manager.block_stop_for(waterfall_id);
    auto stop_future = std::async(std::launch::async, [this, waterfall_id]() {
        return post_with_fresh_client("/sessions/" + waterfall_id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(waterfall_id));

    const auto listed = client->Get("/sessions");
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->status, 200);

    const auto detail = client->Get(("/sessions/" + http_id).c_str());
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->status, 200);

    const auto http_route = client->Get(("/sessions/" + http_id + "/streams/series0/http").c_str());
    ASSERT_TRUE(http_route);
    EXPECT_EQ(http_route->status, 200);
    EXPECT_EQ(http_route->body, R"({"session":"http"})");

    runtime_manager.release_stop_for(waterfall_id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, SlowWebsocketStopDoesNotBlockUnrelatedReadsOrAuthoredHttpOperations) {
    const auto ws_created = create_session_with_graph(authored_websocket_graph());
    const auto http_created = create_session_with_graph(authored_http_poll_graph());
    const auto ws_id = ws_created["id"].get<std::string>();
    const auto http_id = http_created["id"].get<std::string>();
    runtime_manager.stream_payloads[http_id + "/series0"] = R"({"session":"http"})";

    ASSERT_TRUE(client->Post(("/sessions/" + ws_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(client->Post(("/sessions/" + http_id + "/start").c_str(), "", "application/json"));

    runtime_manager.block_stop_for(ws_id);
    auto stop_future = std::async(std::launch::async, [this, ws_id]() {
        return post_with_fresh_client("/sessions/" + ws_id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(ws_id));

    const auto listed = client->Get("/sessions");
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->status, 200);

    const auto detail = client->Get(("/sessions/" + http_id).c_str());
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->status, 200);

    const auto http_route = client->Get(("/sessions/" + http_id + "/streams/series0/http").c_str());
    ASSERT_TRUE(http_route);
    EXPECT_EQ(http_route->status, 200);
    EXPECT_EQ(http_route->body, R"({"session":"http"})");

    runtime_manager.release_stop_for(ws_id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, SlowAlternateWebsocketStopDoesNotBlockUnrelatedReadsOrAuthoredHttpOperations) {
    const auto ws_created = create_session_with_graph(alternate_websocket_graph());
    const auto http_created = create_session_with_graph(authored_http_poll_graph());
    const auto ws_id = ws_created["id"].get<std::string>();
    const auto http_id = http_created["id"].get<std::string>();
    runtime_manager.stream_payloads[http_id + "/series0"] = R"({"session":"http"})";

    ASSERT_TRUE(client->Post(("/sessions/" + ws_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(client->Post(("/sessions/" + http_id + "/start").c_str(), "", "application/json"));

    runtime_manager.block_stop_for(ws_id);
    auto stop_future = std::async(std::launch::async, [this, ws_id]() {
        return post_with_fresh_client("/sessions/" + ws_id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(ws_id));

    const auto listed = client->Get("/sessions");
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->status, 200);

    const auto detail = client->Get(("/sessions/" + http_id).c_str());
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->status, 200);

    const auto http_route = client->Get(("/sessions/" + http_id + "/streams/series0/http").c_str());
    ASSERT_TRUE(http_route);
    EXPECT_EQ(http_route->status, 200);
    EXPECT_EQ(http_route->body, R"({"session":"http"})");

    runtime_manager.release_stop_for(ws_id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, SlowMatrixWebsocketStopDoesNotBlockUnrelatedReadsOrAuthoredHttpOperations) {
    const auto waterfall_created = create_session_with_graph(matrix_websocket_graph());
    const auto http_created = create_session_with_graph(authored_http_poll_graph());
    const auto waterfall_id = waterfall_created["id"].get<std::string>();
    const auto http_id = http_created["id"].get<std::string>();
    runtime_manager.stream_payloads[http_id + "/series0"] = R"({"session":"http"})";

    ASSERT_TRUE(client->Post(("/sessions/" + waterfall_id + "/start").c_str(), "", "application/json"));
    ASSERT_TRUE(client->Post(("/sessions/" + http_id + "/start").c_str(), "", "application/json"));

    runtime_manager.block_stop_for(waterfall_id);
    auto stop_future = std::async(std::launch::async, [this, waterfall_id]() {
        return post_with_fresh_client("/sessions/" + waterfall_id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(waterfall_id));

    const auto listed = client->Get("/sessions");
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed->status, 200);

    const auto detail = client->Get(("/sessions/" + http_id).c_str());
    ASSERT_TRUE(detail);
    EXPECT_EQ(detail->status, 200);

    const auto http_route = client->Get(("/sessions/" + http_id + "/streams/series0/http").c_str());
    ASSERT_TRUE(http_route);
    EXPECT_EQ(http_route->status, 200);
    EXPECT_EQ(http_route->body, R"({"session":"http"})");

    runtime_manager.release_stop_for(waterfall_id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, GetSessionByIdRemainsResponsiveDuringSlowStop) {
    const auto id = create_running_session();
    runtime_manager.block_stop_for(id);

    auto stop_future = std::async(std::launch::async, [this, id]() {
        return post_with_fresh_client("/sessions/" + id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(id));

    const auto started = std::chrono::steady_clock::now();
    const auto response = client->Get(("/sessions/" + id).c_str());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_LT(elapsed, std::chrono::milliseconds(300));

    runtime_manager.release_stop_for(id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, RestartDuringSlowStopIsRejectedDeterministically) {
    const auto id = create_running_session();
    runtime_manager.block_stop_for(id);

    auto stop_future = std::async(std::launch::async, [this, id]() {
        return post_with_fresh_client("/sessions/" + id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(id));

    const auto response = client->Post(("/sessions/" + id + "/restart").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 409);
    EXPECT_EQ(parse_json(response)["error"]["code"], "invalid_state");

    runtime_manager.release_stop_for(id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, RemoveDuringSlowStopDoesNotDeleteSessionPrematurely) {
    const auto id = create_running_session();
    runtime_manager.block_stop_for(id);

    auto stop_future = std::async(std::launch::async, [this, id]() {
        return post_with_fresh_client("/sessions/" + id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(id));

    const auto remove_response = client->Delete(("/sessions/" + id).c_str());

    ASSERT_TRUE(remove_response);
    EXPECT_EQ(remove_response->status, 409);
    EXPECT_EQ(parse_json(remove_response)["error"]["code"], "invalid_state");

    const auto session_response = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(session_response);
    EXPECT_EQ(session_response->status, 200);

    runtime_manager.release_stop_for(id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, StopFailureMarksSessionAsError) {
    const auto id = create_running_session();
    runtime_manager.stop_exception =
        std::make_exception_ptr(std::runtime_error("teardown timed out after 2000 ms"));

    const auto response = client->Post(("/sessions/" + id + "/stop").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_EQ(parse_json(response)["error"]["code"], "runtime_error");

    const auto session_response = client->Get(("/sessions/" + id).c_str());
    ASSERT_TRUE(session_response);
    const auto body = parse_json(session_response);
    EXPECT_EQ(body["state"], "error");
    ASSERT_FALSE(body["last_error"].is_null());
    EXPECT_NE(body["last_error"].get<std::string>().find("teardown timed out"), std::string::npos);
}

TEST_F(HttpApiTest, SlowStopDoesNotBlockAnotherSessionsLifecycleOperation) {
    const auto slow_id = create_running_session();
    const auto other_created = create_session_via_api();
    const auto other_id = other_created["id"].get<std::string>();
    runtime_manager.block_stop_for(slow_id);

    auto stop_future = std::async(std::launch::async, [this, slow_id]() {
        return post_with_fresh_client("/sessions/" + slow_id + "/stop");
    });
    ASSERT_TRUE(runtime_manager.wait_for_stop_entry(slow_id));

    const auto started = std::chrono::steady_clock::now();
    const auto start_response = client->Post(("/sessions/" + other_id + "/start").c_str(), "", "application/json");
    const auto elapsed = std::chrono::steady_clock::now() - started;

    ASSERT_TRUE(start_response);
    EXPECT_EQ(start_response->status, 200);
    EXPECT_LT(elapsed, std::chrono::milliseconds(300));

    runtime_manager.release_stop_for(slow_id);
    const auto stop_response = stop_future.get();
    ASSERT_TRUE(stop_response);
    EXPECT_EQ(stop_response->status, 200);
}

TEST_F(HttpApiTest, RestartSessionSuccess) {
    const auto created = parse_json(client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json"));
    const auto response =
        client->Post(("/sessions/" + created["id"].get<std::string>() + "/restart").c_str(), "", "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["state"], "running");
    EXPECT_TRUE(body["last_error"].is_null());
}

TEST_F(HttpApiTest, DeleteSessionSuccessReturnsNoContent) {
    const auto created = parse_json(client->Post("/sessions", R"({"name":"demo","grc":"flowgraph"})", "application/json"));
    const auto response = client->Delete(("/sessions/" + created["id"].get<std::string>()).c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 204);
    EXPECT_TRUE(response->body.empty());

    const auto missing = client->Get(("/sessions/" + created["id"].get<std::string>()).c_str());
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
    expect_json_content_type(missing);
}

TEST_F(HttpApiTest, DeleteSessionNotFoundReturns404) {
    const auto response = client->Delete("/sessions/sess_missing");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    expect_json_content_type(response);
    const auto body = parse_json(response);
    EXPECT_EQ(body["error"]["code"], "not_found");
    EXPECT_FALSE(body["error"]["message"].get<std::string>().empty());
}

TEST_F(HttpApiTest, PostBlockSettingsUsesStagedModeByDefault) {
    const auto id = create_running_session();

    const auto response = client->Post(("/sessions/" + id + "/blocks/sig0/settings").c_str(),
                                       R"({"frequency":1250.0,"amplitude":0.5})",
                                       "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    EXPECT_EQ(body["session_id"], id);
    EXPECT_EQ(body["block"], "sig0");
    EXPECT_EQ(body["applied_via"], "staged_settings");
    EXPECT_EQ(body["accepted"], true);
    EXPECT_EQ(runtime_manager.last_mode, gr4cp::runtime::BlockSettingsMode::Staged);
    EXPECT_EQ(runtime_manager.set_calls, 1);
    ASSERT_TRUE(runtime_manager.last_patch.contains("frequency"));
    EXPECT_EQ(runtime_manager.last_patch.at("frequency").value_or(0.0), 1250.0);
}

TEST_F(HttpApiTest, PostBlockSettingsImmediateModeUsesImmediateEndpoint) {
    const auto id = create_running_session();

    const auto response = client->Post(("/sessions/" + id + "/blocks/sig0/settings?mode=immediate").c_str(),
                                       R"({"enabled":true})",
                                       "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(parse_json(response)["applied_via"], "settings");
    EXPECT_EQ(runtime_manager.last_mode, gr4cp::runtime::BlockSettingsMode::Immediate);
}

TEST_F(HttpApiTest, GetBlockSettingsReturnsCurrentSettings) {
    const auto id = create_running_session();
    runtime_manager.settings[id]["sig0"] = gr::property_map{
        {"frequency", 1250.0},
        {"amplitude", 0.5},
        {"nested", gr::property_map{{"enabled", true}}},
    };

    const auto response = client->Get(("/sessions/" + id + "/blocks/sig0/settings").c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const auto body = parse_json(response);
    EXPECT_EQ(body["settings"]["frequency"], 1250.0);
    EXPECT_EQ(body["settings"]["amplitude"], 0.5);
    EXPECT_EQ(body["settings"]["nested"]["enabled"], true);
}

TEST_F(HttpApiTest, PostBlockSettingsOnStoppedSessionReturnsConflict) {
    const auto created = create_session_via_api();

    const auto response = client->Post(("/sessions/" + created["id"].get<std::string>() + "/blocks/sig0/settings").c_str(),
                                       R"({"frequency":1250.0})",
                                       "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 409);
    EXPECT_EQ(parse_json(response)["error"]["code"], "invalid_state");
}

TEST_F(HttpApiTest, GetBlockSettingsOnStoppedSessionReturnsConflict) {
    const auto created = create_session_via_api();

    const auto response = client->Get(("/sessions/" + created["id"].get<std::string>() + "/blocks/sig0/settings").c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 409);
    EXPECT_EQ(parse_json(response)["error"]["code"], "invalid_state");
}

TEST_F(HttpApiTest, BlockSettingsUnknownSessionReturnsNotFound) {
    const auto response = client->Get("/sessions/sess_missing/blocks/sig0/settings");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_EQ(parse_json(response)["error"]["code"], "not_found");
}

TEST_F(HttpApiTest, BlockSettingsUnknownBlockReturnsNotFound) {
    const auto id = create_running_session();
    runtime_manager.set_exception =
        std::make_exception_ptr(gr4cp::runtime::BlockNotFoundError("block not found in running session: missing"));

    const auto response = client->Post(("/sessions/" + id + "/blocks/missing/settings").c_str(),
                                       R"({"frequency":1250.0})",
                                       "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 404);
    EXPECT_EQ(parse_json(response)["error"]["code"], "not_found");
}

TEST_F(HttpApiTest, BlockSettingsInvalidJsonBodyReturnsValidationError) {
    const auto id = create_running_session();

    const auto response = client->Post(("/sessions/" + id + "/blocks/sig0/settings").c_str(),
                                       R"({"frequency":)",
                                       "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_EQ(parse_json(response)["error"]["code"], "validation_error");
}

TEST_F(HttpApiTest, BlockSettingsUnsupportedArrayReturnsValidationError) {
    const auto id = create_running_session();

    const auto response = client->Post(("/sessions/" + id + "/blocks/sig0/settings").c_str(),
                                       R"({"values":[1,2,3]})",
                                       "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 400);
    EXPECT_EQ(parse_json(response)["error"]["code"], "validation_error");
}

TEST_F(HttpApiTest, BlockSettingsRuntimeFailureReturnsRuntimeError) {
    const auto id = create_running_session();
    runtime_manager.set_exception = std::make_exception_ptr(std::runtime_error("runtime manager unavailable"));

    const auto response = client->Post(("/sessions/" + id + "/blocks/sig0/settings").c_str(),
                                       R"({"frequency":1250.0})",
                                       "application/json");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 500);
    EXPECT_EQ(parse_json(response)["error"]["code"], "runtime_error");
}

TEST_F(HttpApiTest, BlockSettingsReplyTimeoutReturnsGatewayTimeout) {
    const auto id = create_running_session();
    runtime_manager.get_exception =
        std::make_exception_ptr(gr4cp::runtime::ReplyTimeoutError("timed out waiting for runtime reply from block sig0"));

    const auto response = client->Get(("/sessions/" + id + "/blocks/sig0/settings").c_str());

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 504);
    EXPECT_EQ(parse_json(response)["error"]["code"], "timeout");
}

TEST_F(HttpApiTest, BlockSettingsNestedObjectRoundTrips) {
    const auto id = create_running_session();

    const auto post_response = client->Post(("/sessions/" + id + "/blocks/sig0/settings").c_str(),
                                            R"({"nested":{"gain":3.0,"enabled":true}})",
                                            "application/json");
    ASSERT_TRUE(post_response);
    ASSERT_EQ(post_response->status, 200);

    const auto get_response = client->Get(("/sessions/" + id + "/blocks/sig0/settings").c_str());
    ASSERT_TRUE(get_response);
    ASSERT_EQ(get_response->status, 200);
    const auto body = parse_json(get_response);
    EXPECT_EQ(body["settings"]["nested"]["gain"], 3.0);
    EXPECT_EQ(body["settings"]["nested"]["enabled"], true);
}

}  // namespace
