#include "gr4cp/api/http_server.hpp"
#include "gr4cp/api/wasm_api.hpp"

#include <regex>
#include <thread>

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gr4cp/runtime/stub_runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

namespace {

using Json = nlohmann::json;

class EmptyBlockCatalogProvider final : public gr4cp::catalog::BlockCatalogProvider {
public:
    std::vector<gr4cp::domain::BlockDescriptor> list() const override { return {}; }
};

class EmptySchedulerCatalogProvider final : public gr4cp::catalog::SchedulerCatalogProvider {
public:
    std::vector<gr4cp::domain::SchedulerDescriptor> list() const override { return {}; }
};

class WasmApiTest : public ::testing::Test {
protected:
    void TearDown() override {
        http_server.stop();
        if (http_thread.joinable()) {
            http_thread.join();
        }
    }

    gr4cp::storage::InMemorySessionRepository repository;
    gr4cp::runtime::StubRuntimeManager runtime;
    EmptyBlockCatalogProvider blocks;
    EmptySchedulerCatalogProvider schedulers;
    gr4cp::app::SessionService sessions{repository, runtime};
    gr4cp::app::SessionStreamService streams;
    gr4cp::app::BlockCatalogService block_catalog{blocks};
    gr4cp::app::SchedulerCatalogService scheduler_catalog{schedulers};
    gr4cp::app::BlockSettingsService settings{repository, runtime};
    gr4cp::api::WasmApi api{sessions, streams, block_catalog, scheduler_catalog, settings};
    gr4cp::api::HttpServer http_server{sessions, streams, block_catalog, scheduler_catalog, settings};
    std::jthread http_thread;
};

TEST_F(WasmApiTest, HealthIdentityIsAStableUuidAcrossDispatchers) {
    const auto health = api.dispatch("GET", "/healthz", "");
    ASSERT_EQ(health.status, 200);
    const auto body = Json::parse(health.body);
    EXPECT_EQ(body.at("ok"), true);
    const auto identity = body.at("server_instance_id").get<std::string>();
    EXPECT_TRUE(std::regex_match(identity, std::regex{
        "[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"}));
    EXPECT_EQ(api.dispatch("GET", "/healthz", "").body, health.body);

    gr4cp::api::WasmApi another{sessions, streams, block_catalog, scheduler_catalog, settings};
    EXPECT_EQ(another.dispatch("GET", "/healthz", "").body, health.body);
}

TEST_F(WasmApiTest, HealthAndEverySessionResponseShareHttpIdentityAndSerialization) {
    const auto port = http_server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    http_thread = std::jthread([this]() { http_server.listen_after_bind(); });
    httplib::Client client("127.0.0.1", port);
    const auto http_health = client.Get("/healthz");
    ASSERT_TRUE(http_health);
    ASSERT_EQ(http_health->status, 200);
    const auto health = Json::parse(api.dispatch("GET", "/healthz", "").body);
    EXPECT_EQ(Json::parse(http_health->body), health);

    const auto created = api.dispatch("POST", "/sessions", R"({"name":"identity-check","grc":"graph"})");
    ASSERT_EQ(created.status, 201);
    auto session = Json::parse(created.body);
    const auto id = session.at("id").get<std::string>();
    const auto path = "/sessions/" + id;
    EXPECT_EQ(session.at("server_instance_id"), health.at("server_instance_id"));

    const auto expect_matching_session = [&]() {
        const auto http_session = client.Get(path);
        ASSERT_TRUE(http_session);
        ASSERT_EQ(http_session->status, 200);
        const auto detail = api.dispatch("GET", path, "");
        ASSERT_EQ(detail.status, 200);
        EXPECT_EQ(Json::parse(detail.body), session);
        EXPECT_EQ(Json::parse(http_session->body), session);
        const auto listed = api.dispatch("GET", "/sessions", "");
        ASSERT_EQ(listed.status, 200);
        EXPECT_EQ(Json::parse(listed.body), Json::array({session}));
    };
    expect_matching_session();

    for (const auto* action : {"start", "stop", "restart"}) {
        const auto response = api.dispatch("POST", path + "/" + action, "");
        ASSERT_EQ(response.status, 200) << response.body;
        session = Json::parse(response.body);
        EXPECT_EQ(session.at("server_instance_id"), health.at("server_instance_id"));
        expect_matching_session();
    }
}

}  // namespace
