#include "gr4cp/api/http_server.hpp"

#include <chrono>
#include <thread>
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
    EXPECT_EQ(body.at("ok"), true);
    ASSERT_TRUE(body.at("server_instance_id").is_string());
    EXPECT_FALSE(body.at("server_instance_id").get<std::string>().empty());
    const auto again = client.Get("/healthz");
    ASSERT_TRUE(again);
    EXPECT_EQ(nlohmann::json::parse(again->body), body);
    const auto session = client.Post("/sessions", R"({"name":"identity-check","grc":"graph"})", "application/json");
    ASSERT_TRUE(session);
    ASSERT_EQ(session->status, 201);
    EXPECT_EQ(nlohmann::json::parse(session->body).at("server_instance_id"), body.at("server_instance_id"));

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

}  // namespace
