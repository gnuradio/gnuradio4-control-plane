#include "gr4cp/api/http_server.hpp"

#include <sys/wait.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/app/session_stream_service.hpp"
#include "gr4cp/catalog/block_catalog_provider.hpp"
#include "gr4cp/catalog/scheduler_catalog_provider.hpp"
#include "gr4cp/runtime/stub_runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

namespace {

// Runs the server executable with `arguments` and returns its exit status and standard output.
std::pair<int, std::string> run_server(const std::string& arguments) {
    const std::string command = std::string(GR4CP_SERVER_EXECUTABLE) + " " + arguments + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return {-1, "failed to launch the server"};
    }
    std::string output;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const int status = pclose(pipe);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, output};
}

class EmptyBlockCatalogProvider final : public gr4cp::catalog::BlockCatalogProvider {
public:
    std::vector<gr4cp::domain::BlockDescriptor> list() const override { return {}; }
};

class EmptySchedulerCatalogProvider final : public gr4cp::catalog::SchedulerCatalogProvider {
public:
    std::vector<gr4cp::domain::SchedulerDescriptor> list() const override { return {}; }
};

TEST(HealthzSmokeTest, ReturnsOkJson) {
    gr4cp::storage::InMemorySessionRepository repository;
    gr4cp::runtime::StubRuntimeManager runtime_manager;
    EmptyBlockCatalogProvider block_catalog_provider;
    EmptySchedulerCatalogProvider scheduler_catalog_provider;
    gr4cp::app::SessionService session_service(repository, runtime_manager);
    gr4cp::app::SessionStreamService session_stream_service;
    gr4cp::app::BlockSettingsService block_settings_service(repository, runtime_manager);
    gr4cp::app::BlockCatalogService block_catalog_service(block_catalog_provider);
    gr4cp::app::SchedulerCatalogService scheduler_catalog_service(scheduler_catalog_provider);

    gr4cp::api::HttpServer server(session_service,
                                  session_stream_service,
                                  block_catalog_service,
                                  scheduler_catalog_service,
                                  block_settings_service);

    const auto port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);

    std::jthread server_thread([&server]() { server.listen_after_bind(); });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    httplib::Client client("127.0.0.1", port);
    const auto response = client.Get("/healthz");

    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->get_header_value("Content-Type"), "application/json");
    const auto body = nlohmann::json::parse(response->body);
    EXPECT_EQ(body, nlohmann::json({{"ok", true}}));

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

TEST(ServerExecutableTest, HelpPrintsUsageAndExitsZero) {
    const auto [status, output] = run_server("--help");
    EXPECT_EQ(status, 0) << output;
    EXPECT_TRUE(output.starts_with("Usage: gr4cp_server [--help]")) << output;
    EXPECT_NE(output.find("GR4CP_PORT"), std::string::npos) << output;
    EXPECT_NE(output.find("GNURADIO4_PLUGIN_DIRECTORIES"), std::string::npos) << output;
}

TEST(ServerExecutableTest, UnknownArgumentExitsWithUsageError) {
    const auto [status, output] = run_server("--no-such-option");
    EXPECT_EQ(status, 2) << output;
    EXPECT_TRUE(output.empty()) << output;
}

}  // namespace
