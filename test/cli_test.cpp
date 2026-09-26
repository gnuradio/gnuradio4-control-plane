#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gr4cp/api/http_server.hpp"
#include "gr4cp/app/block_catalog_service.hpp"
#include "gr4cp/app/block_settings_service.hpp"
#include "gr4cp/app/session_service.hpp"
#include "gr4cp/app/session_stream_service.hpp"
#include "gr4cp/catalog/block_catalog_provider.hpp"
#include "gr4cp/catalog/scheduler_catalog_provider.hpp"
#include "gr4cp/runtime/stub_runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

namespace {

#ifndef GR4CP_CLI_EXECUTABLE
#define GR4CP_CLI_EXECUTABLE "./gr4cp-cli"
#endif

class EmptyBlockCatalogProvider final : public gr4cp::catalog::BlockCatalogProvider {
public:
    std::vector<gr4cp::domain::BlockDescriptor> list() const override { return {}; }
};

class EmptySchedulerCatalogProvider final : public gr4cp::catalog::SchedulerCatalogProvider {
public:
    std::vector<gr4cp::domain::SchedulerDescriptor> list() const override { return {}; }
};

class CliTest : public ::testing::Test {
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
        server = std::make_unique<gr4cp::api::HttpServer>(service,
                                                          session_stream_service,
                                                          block_catalog_service,
                                                          scheduler_catalog_service,
                                                          block_settings_service);
        port = server->bind_to_any_port("127.0.0.1");
        ASSERT_GT(port, 0);
        server_thread = std::jthread([this]() { server->listen_after_bind(); });
        wait_for_server_ready();
        url = "http://127.0.0.1:" + std::to_string(port);
        temp_dir = std::filesystem::temp_directory_path() / "gr4cp-cli-test";
        std::filesystem::create_directories(temp_dir);
    }

    void TearDown() override {
        if (server) {
            server->stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
        std::error_code error;
        std::filesystem::remove_all(temp_dir, error);
    }

    std::filesystem::path write_graph_file(const std::string& name, const std::string& content) {
        const auto path = temp_dir / name;
        std::ofstream output(path);
        output << content;
        return path;
    }

    static std::string shell_quote(const std::string& value) {
        std::string quoted = "'";
        for (const char ch : value) {
            if (ch == '\'') {
                quoted += "'\\''";
            } else {
                quoted += ch;
            }
        }
        quoted += "'";
        return quoted;
    }

    std::pair<int, std::string> run_cli(const std::string& arguments) {
        const std::string command = std::string(GR4CP_CLI_EXECUTABLE) + " " + arguments + " 2>&1";
        FILE* pipe = popen(command.c_str(), "r");
        if (pipe == nullptr) {
            return {1, "failed to launch CLI"};
        }

        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }

        const int status = pclose(pipe);
        if (WIFEXITED(status)) {
            return {WEXITSTATUS(status), output};
        }
        return {1, output};
    }

    std::unique_ptr<gr4cp::api::HttpServer> server;
    gr4cp::storage::InMemorySessionRepository repository;
    gr4cp::runtime::StubRuntimeManager runtime_manager;
    EmptyBlockCatalogProvider block_catalog_provider;
    EmptySchedulerCatalogProvider scheduler_catalog_provider;
    gr4cp::app::SessionService service{repository, runtime_manager};
    gr4cp::app::SessionStreamService session_stream_service;
    gr4cp::app::BlockSettingsService block_settings_service{service, runtime_manager};
    gr4cp::app::BlockCatalogService block_catalog_service{block_catalog_provider};
    gr4cp::app::SchedulerCatalogService scheduler_catalog_service{scheduler_catalog_provider};
    int port{};
    std::string url;
    std::jthread server_thread;
    std::filesystem::path temp_dir;
};

TEST_F(CliTest, CreateCommandReadsFileAndCreatesSession) {
    const auto file = write_graph_file("demo.grc", "graph-body");
    const auto [exit_code, output] = run_cli(
        "sessions create --file " + shell_quote(file.string()) + " --name demo --url " + shell_quote(url));

    EXPECT_EQ(exit_code, 0);
    const auto body = nlohmann::json::parse(output);
    EXPECT_EQ(body["name"], "demo");
    EXPECT_EQ(body["state"], "stopped");
}

TEST_F(CliTest, ListCommandReturnsSessions) {
    const auto created = service.create("demo", "graph");
    const auto [exit_code, output] = run_cli("sessions list --url " + shell_quote(url));

    EXPECT_EQ(exit_code, 0);
    const auto body = nlohmann::json::parse(output);
    ASSERT_EQ(body.size(), 1U);
    EXPECT_EQ(body[0]["id"], created.id);
}

TEST_F(CliTest, GetStartStopRestartDeleteFlow) {
    const auto created = service.create("demo", "graph");

    {
        const auto [exit_code, output] = run_cli("sessions get " + created.id + " --url " + shell_quote(url));
        EXPECT_EQ(exit_code, 0);
        EXPECT_EQ(nlohmann::json::parse(output)["state"], "stopped");
    }

    {
        const auto [exit_code, output] = run_cli("sessions start " + created.id + " --url " + shell_quote(url));
        EXPECT_EQ(exit_code, 0);
        EXPECT_EQ(nlohmann::json::parse(output)["state"], "running");
    }

    {
        const auto [exit_code, output] = run_cli("sessions stop " + created.id + " --url " + shell_quote(url));
        EXPECT_EQ(exit_code, 0);
        EXPECT_EQ(nlohmann::json::parse(output)["state"], "stopped");
    }

    {
        const auto [exit_code, output] = run_cli("sessions restart " + created.id + " --url " + shell_quote(url));
        EXPECT_EQ(exit_code, 0);
        EXPECT_EQ(nlohmann::json::parse(output)["state"], "running");
    }

    {
        const auto [exit_code, output] = run_cli("sessions delete " + created.id + " --url " + shell_quote(url));
        EXPECT_EQ(exit_code, 0);
        EXPECT_EQ(output, "deleted " + created.id + "\n");
    }
}

TEST_F(CliTest, CreateWithMissingFileFails) {
    const auto [exit_code, output] =
        run_cli("sessions create --file /no/such/file.grc --url " + shell_quote(url));

    EXPECT_EQ(exit_code, 1);
    EXPECT_NE(output.find("failed to read file"), std::string::npos);
}

TEST_F(CliTest, GetMissingSessionReturnsServerErrorMessage) {
    const auto [exit_code, output] = run_cli("sessions get sess_missing --url " + shell_quote(url));

    EXPECT_EQ(exit_code, 1);
    EXPECT_NE(output.find("not_found"), std::string::npos);
}

TEST_F(CliTest, StartRunningSessionReturnsConflictMessage) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    const auto [exit_code, output] =
        run_cli("sessions start " + created.id + " --url " + shell_quote(url));

    EXPECT_EQ(exit_code, 1);
    EXPECT_NE(output.find("invalid_state"), std::string::npos);
}

TEST_F(CliTest, CreateWithoutFileArgumentFails) {
    const auto [exit_code, output] = run_cli("sessions create --url " + shell_quote(url));

    EXPECT_EQ(exit_code, 1);
    EXPECT_NE(output.find("create requires --file"), std::string::npos);
}

}  // namespace
