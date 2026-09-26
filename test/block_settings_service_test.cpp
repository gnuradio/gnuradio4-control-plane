#include "gr4cp/app/block_settings_service.hpp"

#include <complex>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

#include <gtest/gtest.h>

#include "gr4cp/app/session_service.hpp"
#include "gr4cp/runtime/runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

namespace {

class FakeBlockSettingsRuntimeManager final : public gr4cp::runtime::RuntimeManager {
public:
    void prepare(const gr4cp::domain::Session&) override {}
    void start(const gr4cp::domain::Session&) override {}
    std::optional<std::string> stop(const gr4cp::domain::Session&) override { return std::nullopt; }
    void destroy(const gr4cp::domain::Session&) override {}
    std::optional<gr4cp::domain::StreamRuntimePlan> active_stream_plan(const gr4cp::domain::Session&) override {
        return std::nullopt;
    }
    gr4cp::runtime::HttpStreamResponse fetch_http_stream(const gr4cp::domain::Session&, const std::string&) override {
        return {};
    }
    gr4cp::runtime::WebSocketStreamRoute resolve_websocket_stream(const gr4cp::domain::Session&,
                                                                  const std::string&) override {
        return {};
    }

    void set_block_settings(const gr4cp::domain::Session& session,
                            const std::string& block_unique_name,
                            const gr::property_map& patch,
                            gr4cp::runtime::BlockSettingsMode mode) override {
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

    gr::property_map get_block_settings(const gr4cp::domain::Session& session, const std::string& block_unique_name) override {
        last_session_id = session.id;
        last_block = block_unique_name;
        if (get_exception) {
            std::rethrow_exception(get_exception);
        }
        return settings[session.id][block_unique_name];
    }

    gr4cp::runtime::RunObservation observe(const gr4cp::domain::Session&) override { return {}; }

    std::string last_session_id;
    std::string last_block;
    gr4cp::runtime::BlockSettingsMode last_mode{gr4cp::runtime::BlockSettingsMode::Staged};
    gr::property_map last_patch;
    std::unordered_map<std::string, std::unordered_map<std::string, gr::property_map>> settings;
    std::exception_ptr set_exception;
    std::exception_ptr get_exception;
};

class BlockSettingsServiceTest : public ::testing::Test {
protected:
    gr4cp::domain::Session create_session(gr4cp::domain::SessionState state) {
        auto session = service_api.create("demo", "graph");
        session.state = state;
        repository.update(session);
        return *repository.get(session.id);
    }

    gr4cp::storage::InMemorySessionRepository repository;
    FakeBlockSettingsRuntimeManager runtime;
    gr4cp::app::SessionService service_api{repository, runtime};
    gr4cp::app::BlockSettingsService service{service_api, runtime};
};

TEST_F(BlockSettingsServiceTest, UpdateUsesRuntimePatchAndStagedModeByDefault) {
    const auto session = create_session(gr4cp::domain::SessionState::Running);

    const auto result = service.update(
        session.id,
        "sig0",
        nlohmann::json{{"frequency", 1250.0}, {"nested", {{"enabled", true}}}},
        gr4cp::runtime::BlockSettingsMode::Staged);

    EXPECT_EQ(result.session_id, session.id);
    EXPECT_EQ(result.block, "sig0");
    EXPECT_EQ(result.applied_via, "staged_settings");
    EXPECT_TRUE(result.accepted);
    EXPECT_EQ(runtime.last_mode, gr4cp::runtime::BlockSettingsMode::Staged);
    ASSERT_TRUE(runtime.last_patch.contains("frequency"));
    EXPECT_EQ(runtime.last_patch.at("frequency").value_or(0.0), 1250.0);
    ASSERT_TRUE(runtime.last_patch.contains("nested"));
    const auto* nested = runtime.last_patch.at("nested").get_if<gr::property_map>();
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->at("enabled").value_or(false), true);
}

TEST_F(BlockSettingsServiceTest, GetReturnsJsonObjectFromRuntimeSettings) {
    const auto session = create_session(gr4cp::domain::SessionState::Running);
    runtime.settings[session.id]["sig0"] = gr::property_map{
        {"frequency", 1250.0},
        {"nested", gr::property_map{{"enabled", true}}},
    };

    const auto result = service.get(session.id, "sig0");

    EXPECT_EQ(result["frequency"], 1250.0);
    EXPECT_EQ(result["nested"]["enabled"], true);
}

TEST_F(BlockSettingsServiceTest, UpdateCarriesArraysElementByElementAndComplexParts) {
    const auto session = create_session(gr4cp::domain::SessionState::Running);

    service.update(session.id,
                   "sig0",
                   nlohmann::json{{"values", nlohmann::json::array({1, 2.5, "three"})},
                                  {"correction", {{"real", 1.0}, {"imag", -2}}}},
                   gr4cp::runtime::BlockSettingsMode::Staged);

    const auto* values = runtime.last_patch.at("values").get_if<gr::Tensor<gr::pmt::Value>>();
    ASSERT_NE(values, nullptr);
    ASSERT_EQ(values->size(), 3U);
    EXPECT_EQ((*values)[0].value_or(std::int64_t{0}), 1);
    EXPECT_EQ((*values)[1].value_or(0.0), 2.5);
    EXPECT_EQ((*values)[2].value_or(std::string()), "three");
    const auto* correction = runtime.last_patch.at("correction").get_if<std::complex<double>>();
    ASSERT_NE(correction, nullptr);
    EXPECT_EQ(*correction, std::complex<double>(1.0, -2.0));
}

TEST_F(BlockSettingsServiceTest, GetReturnsTensorsAsArraysAndComplexAsParts) {
    const auto session = create_session(gr4cp::domain::SessionState::Running);
    gr::Tensor<gr::pmt::Value> mixed;
    mixed.push_back(gr::pmt::Value(std::int64_t{7}));
    mixed.push_back(gr::pmt::Value(std::string("seven")));
    runtime.settings[session.id]["sig0"] = gr::property_map{
        {"gains", gr::Tensor<float>(std::pmr::vector<float>{0.5F, 2.0F})},
        {"enabled", gr::Tensor<bool>(std::pmr::vector<bool>{true, false})},
        {"taps", gr::Tensor<std::complex<double>>(std::pmr::vector<std::complex<double>>{{1.0, -1.0}})},
        {"mixed", mixed},
        {"correction", std::complex<float>(0.5F, 0.25F)},
    };

    const auto result = service.get(session.id, "sig0");

    EXPECT_EQ(result["gains"], nlohmann::json::parse("[0.5, 2.0]"));
    EXPECT_EQ(result["enabled"], nlohmann::json::parse("[true, false]"));
    EXPECT_EQ(result["taps"], nlohmann::json::parse(R"([{"real": 1.0, "imag": -1.0}])"));
    EXPECT_EQ(result["mixed"], nlohmann::json::parse(R"([7, "seven"])"));
    EXPECT_EQ(result["correction"], nlohmann::json::parse(R"({"real": 0.5, "imag": 0.25})"));
}

TEST_F(BlockSettingsServiceTest, UpdateFailsWhenSessionNotRunning) {
    const auto session = create_session(gr4cp::domain::SessionState::Stopped);

    EXPECT_THROW(service.update(session.id, "sig0", nlohmann::json::object(), gr4cp::runtime::BlockSettingsMode::Staged),
                 gr4cp::app::InvalidStateError);
}

TEST_F(BlockSettingsServiceTest, RunEndingByItselfMapsToInvalidState) {
    const auto session = create_session(gr4cp::domain::SessionState::Running);
    runtime.set_exception = std::make_exception_ptr(gr4cp::runtime::SessionNotRunningError("session runtime is not running"));
    runtime.get_exception = runtime.set_exception;

    EXPECT_THROW(service.update(session.id, "sig0", nlohmann::json::object(), gr4cp::runtime::BlockSettingsMode::Staged),
                 gr4cp::app::InvalidStateError);
    EXPECT_THROW(service.get(session.id, "sig0"), gr4cp::app::InvalidStateError);
}

TEST_F(BlockSettingsServiceTest, SettingsRefusalMapsToValidationError) {
    const auto session = create_session(gr4cp::domain::SessionState::Running);
    runtime.set_exception = std::make_exception_ptr(gr4cp::runtime::SettingsRefusedError("block sig0 has no setting named gain"));

    try {
        service.update(session.id, "sig0", nlohmann::json{{"gain", 1.0}}, gr4cp::runtime::BlockSettingsMode::Immediate);
        FAIL() << "expected ValidationError";
    } catch (const gr4cp::app::ValidationError& error) {
        EXPECT_STREQ(error.what(), "block sig0 has no setting named gain");
    }
}

TEST_F(BlockSettingsServiceTest, RuntimeUnknownBlockMapsToNotFound) {
    const auto session = create_session(gr4cp::domain::SessionState::Running);
    runtime.set_exception = std::make_exception_ptr(gr4cp::runtime::BlockNotFoundError("block not found in running session: missing"));

    EXPECT_THROW(service.update(session.id, "missing", nlohmann::json::object(), gr4cp::runtime::BlockSettingsMode::Staged),
                 gr4cp::app::NotFoundError);
}

}  // namespace
