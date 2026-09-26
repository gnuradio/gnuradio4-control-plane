#include "gr4cp/app/session_service.hpp"
#include "gr4cp/catalog/gr4_block_catalog_provider.hpp"
#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"
#include "gr4cp/domain/session.hpp"
#include "gr4cp/runtime/gr4_runtime_manager.hpp"
#include "gr4cp/runtime/plugin_loader.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"
#include "test_blocks.hpp"
#include "test_plugin_loader.hpp"

#include <gnuradio-4.0/BlockRegistry.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/SchedulerRegistration.hpp>

#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

namespace {

using namespace std::chrono_literals;

// Reads the session until `done` holds for it, for at most ten seconds, and returns the last read.
template <typename Predicate>
gr4cp::domain::Session read_until(gr4cp::app::SessionService& service, const std::string& id, Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    auto session = service.get(id);
    while (!done(session) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        session = service.get(id);
    }
    return session;
}

// Reads a block's settings until `key` holds `expected`, for at most ten seconds: a staged value applies at the
// block's next work call.
gr::property_map read_setting_until(gr4cp::runtime::Gr4RuntimeManager& runtime,
                                    const gr4cp::domain::Session& session,
                                    const std::string& block,
                                    const std::string& key,
                                    float expected) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    const auto holds = [&key, expected](const gr::property_map& settings) {
        const auto it = settings.find(key);
        return it != settings.end() && it->second.value_or(0.0F) == expected;
    };
    auto settings = runtime.get_block_settings(session, block);
    while (!holds(settings) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        settings = runtime.get_block_settings(session, block);
    }
    return settings;
}

// Waits, for at most ten seconds, until the session's run is ending by itself. It asks the runtime manager and observes
// nothing.
bool wait_until_run_is_ending(gr4cp::runtime::Gr4RuntimeManager& runtime,
                              const gr4cp::domain::Session& session,
                              const std::string& block) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            (void)runtime.get_block_settings(session, block);
        } catch (const gr4cp::runtime::SessionNotRunningError&) {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

// The type name of the scheduler the loader creates for `type`, or nothing.
std::optional<std::string> created_scheduler_type(const std::shared_ptr<gr::PluginLoader>& loader, std::string_view type) {
    const auto scheduler = loader->instantiateScheduler(type);
    if (!scheduler) {
        return std::nullopt;
    }
    return std::string(gr::SchedulerModel::asBlockModelPtr(scheduler)->typeName());
}

constexpr std::string_view kStudioSeriesSinkId = "gr::studio::StudioSeriesSink<float32>";
constexpr std::string_view kStudioWaterfallSinkId = "gr::studio::StudioWaterfallSink<float32>";

std::vector<std::string> process_environment() {
    std::vector<std::string> entries;
    for (char** entry = environ; *entry != nullptr; ++entry) {
        entries.emplace_back(*entry);
    }
    std::ranges::sort(entries);
    return entries;
}

// The environment as the process started with it, before any case or the test loader ran.
const std::vector<std::string> kEnvironmentAtStart = process_environment();

// Sets or unsets one environment variable for a scope and restores it after.
class ScopedVariable {
public:
    ScopedVariable(std::string name, std::optional<std::string> value) : name_(std::move(name)) {
        if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) {
            saved_ = existing;
        }
        set(value);
    }
    ~ScopedVariable() { set(saved_); }
    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

private:
    void set(const std::optional<std::string>& value) {
        if (value.has_value()) {
            ::setenv(name_.c_str(), value->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    std::string name_;
    std::optional<std::string> saved_;
};

bool block_is_registered(std::string_view block_id) {
    const auto available = gr4cp::test::plugin_loader()->availableBlocks();
    return std::ranges::any_of(available, [block_id](const auto& name) { return name == block_id; });
}

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
      tone_frequency: 25.0
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
      tone_frequency: 25.0
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
      tone_frequency: 25.0
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

    gr4cp::runtime::Gr4RuntimeManager runtime_{gr4cp::test::plugin_loader()};
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
        gr::property_map{{"tone_frequency", 1250.0}, {"amplitude", 0.5}},
        gr4cp::runtime::BlockSettingsMode::Staged));

    const auto settings = read_setting_until(runtime_, session, "src0", "tone_frequency", 1250.0F);
    ASSERT_TRUE(settings.contains("tone_frequency"));
    ASSERT_TRUE(settings.contains("amplitude"));
    EXPECT_EQ(settings.at("tone_frequency").value_or(0.0F), 1250.0F);
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
      tone_frequency: 25.0
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
        gr::property_map{{"tone_frequency", 1250.0}, {"amplitude", 0.5}},
        gr4cp::runtime::BlockSettingsMode::Staged));

    const auto settings = read_setting_until(runtime_, session, "gr__basic__SignalGenerator_float32__1", "tone_frequency", 1250.0F);
    ASSERT_TRUE(settings.contains("tone_frequency"));
    ASSERT_TRUE(settings.contains("amplitude"));
    EXPECT_EQ(settings.at("tone_frequency").value_or(0.0F), 1250.0F);
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
        gr::property_map{{"tone_frequency", 900.0}},
        gr4cp::runtime::BlockSettingsMode::Staged));

    const auto settings = read_setting_until(runtime_, session, runtime_identifier, "tone_frequency", 900.0F);
    ASSERT_TRUE(settings.contains("tone_frequency"));
    EXPECT_EQ(settings.at("tone_frequency").value_or(0.0F), 900.0F);

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
    if (!block_is_registered(kStudioSeriesSinkId)) {
        GTEST_SKIP() << "block not registered: " << kStudioSeriesSinkId;
    }

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
    if (!block_is_registered(kStudioWaterfallSinkId)) {
        GTEST_SKIP() << "block not registered: " << kStudioWaterfallSinkId;
    }

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

TEST_F(Gr4RuntimeManagerTest, PluginDirectoriesComeFromTheEnvironmentThenTheDefault) {
    {
        const ScopedVariable variable("GNURADIO4_PLUGIN_DIRECTORIES", "/env/one:/env/two:/default/one");
        EXPECT_EQ(gr4cp::runtime::plugin_directories("/default/one:/default/two"),
                  (std::vector<std::string>{"/env/one", "/env/two", "/default/one", "/default/two"}));
    }
    {
        const ScopedVariable variable("GNURADIO4_PLUGIN_DIRECTORIES", std::nullopt);
        EXPECT_EQ(gr4cp::runtime::plugin_directories("/default/one:/default/two"),
                  (std::vector<std::string>{"/default/one", "/default/two"}));
    }
}

// Each directory list loads into registries of its own. The blocks this process registered before do not count.
TEST(PluginLoaderTest, StartupRefusesAMissingAnUnreadableAndAnEmptyDirectory) {
    const auto root = std::filesystem::temp_directory_path() / std::format("gr4cp-plugin-directories-{}", ::getpid());
    const auto missing = (root / "missing").string();
    const auto unreadable = (root / "unreadable").string();
    const auto empty = (root / "empty").string();
    std::filesystem::create_directories(unreadable);
    std::filesystem::create_directories(empty);
    std::filesystem::permissions(unreadable, std::filesystem::perms::none);

    const auto refusal = [](std::vector<std::string> directories) {
        gr::BlockRegistry blocks;
        gr::SchedulerRegistry schedulers;
        try {
            (void)gr4cp::runtime::load_plugins(directories, blocks, schedulers);
        } catch (const gr4cp::runtime::PluginLoadError& error) {
            return std::string(error.what());
        }
        return std::string("<accepted>");
    };
    const auto missing_refusal = refusal({missing});
    const auto unreadable_refusal = refusal({unreadable, empty});
    const auto empty_refusal = refusal({missing, empty});

    std::filesystem::permissions(unreadable, std::filesystem::perms::owner_all);
    std::filesystem::remove_all(root);

    EXPECT_NE(missing_refusal.find("no GNU Radio 4 plugin directory exists: " + missing), std::string::npos) << missing_refusal;
    EXPECT_NE(unreadable_refusal.find("cannot read GNU Radio 4 plugin directory (" + unreadable), std::string::npos)
        << unreadable_refusal;
    EXPECT_NE(empty_refusal.find("found no registered blocks"), std::string::npos) << empty_refusal;
}

// A directory that holds a shared object which is neither a plugin nor a block library stops startup, and the one line
// names the directory, the file and the loader's reason.
TEST(PluginLoaderTest, StartupRefusalNamesTheDirectoryAndTheLibraryThatFailed) {
    const auto directory = std::filesystem::temp_directory_path() / std::format("gr4cp-library-directory-{}", ::getpid());
    std::filesystem::create_directories(directory);
    std::filesystem::copy_file(GR4CP_NOT_A_PLUGIN_LIBRARY, directory / "libnot-a-plugin.so");

    std::string refusal = "<accepted>";
    {
        gr::BlockRegistry blocks;
        gr::SchedulerRegistry schedulers;
        const std::vector<std::string> directories{directory.string()};
        try {
            (void)gr4cp::runtime::load_plugins(directories, blocks, schedulers);
        } catch (const gr4cp::runtime::PluginLoadError& error) {
            refusal = error.what();
        }
    }
    std::filesystem::remove_all(directory);

    EXPECT_NE(refusal.find("plugin directory " + directory.string() + " holds libnot-a-plugin.so"), std::string::npos)
        << refusal;
    EXPECT_NE(refusal.find("gr_plugin_make"), std::string::npos) << refusal;
    EXPECT_EQ(refusal.find('\n'), std::string::npos) << refusal;
}

TEST_F(Gr4RuntimeManagerTest, ProcessEnvironmentIsLeftUnchanged) {
    gr4cp::catalog::Gr4SchedulerCatalogProvider schedulers(gr4cp::test::plugin_loader());
    EXPECT_FALSE(schedulers.list().empty());

    auto session = make_session("runtime_environment", continuous_graph_yaml());
    runtime_.prepare(session);
    runtime_.start(session);
    std::this_thread::sleep_for(100ms);
    runtime_.stop(session);
    runtime_.destroy(session);

    EXPECT_EQ(process_environment(), kEnvironmentAtStart);
}

TEST_F(Gr4RuntimeManagerTest, NoStagingDirectoryIsCreated) {
    const auto temporary = std::filesystem::temp_directory_path() / std::format("gr4cp-staging-check-{}", ::getpid());
    std::filesystem::create_directories(temporary);
    {
        const ScopedVariable variable("TMPDIR", temporary.string());
        gr4cp::catalog::Gr4BlockCatalogProvider blocks(gr4cp::test::plugin_loader());
        EXPECT_FALSE(blocks.list().empty());
        gr4cp::catalog::Gr4SchedulerCatalogProvider schedulers(gr4cp::test::plugin_loader());
        EXPECT_FALSE(schedulers.list().empty());

        auto session = make_session("runtime_staging", continuous_graph_yaml());
        runtime_.prepare(session);
        runtime_.start(session);
        std::this_thread::sleep_for(100ms);
        runtime_.stop(session);
        runtime_.destroy(session);

        EXPECT_EQ(std::filesystem::temp_directory_path(), temporary);
    }
    EXPECT_TRUE(std::filesystem::is_empty(temporary));
    std::filesystem::remove_all(temporary);
}

TEST_F(Gr4RuntimeManagerTest, FiniteGraphReadsStoppedAfterItEnds) {
    gr4cp::test::register_test_blocks();
    gr4cp::storage::InMemorySessionRepository repository;
    gr4cp::app::SessionService service(repository, runtime_);
    const auto created = service.create("finite", R"(blocks:
  - id: "gr4cp::test::FiniteSource"
    parameters:
      name: "finite0"
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "sink0"
connections:
  - ["finite0", 0, "sink0", 0]
)");

    EXPECT_EQ(service.start(created.id).state, gr4cp::domain::SessionState::Running);
    auto session = read_until(service, created.id, [](const auto& read) { return read.state != gr4cp::domain::SessionState::Running; });
    EXPECT_EQ(session.state, gr4cp::domain::SessionState::Stopped);
    EXPECT_FALSE(session.last_error.has_value());
    EXPECT_EQ(service.list().front().state, gr4cp::domain::SessionState::Stopped);

    EXPECT_EQ(service.start(created.id).state, gr4cp::domain::SessionState::Running);
    session = read_until(service, created.id, [](const auto& read) { return read.state != gr4cp::domain::SessionState::Running; });
    EXPECT_EQ(session.state, gr4cp::domain::SessionState::Stopped);
    service.remove(created.id);
}

TEST_F(Gr4RuntimeManagerTest, NameTwoBlocksCarryIsRefusedWithTheCandidates) {
    auto session = make_session("runtime_shared_name", R"(blocks:
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    unique_name: "gen_a"
    parameters:
      name: "twin"
      sample_rate: 1000.0
      chunk_size: 32
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "sink_a"
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    unique_name: "gen_b"
    parameters:
      name: "twin"
      sample_rate: 1000.0
      chunk_size: 32
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "sink_b"
connections:
  - ["gen_a", 0, "sink_a", 0]
  - ["gen_b", 0, "sink_b", 0]
)");
    runtime_.prepare(session);
    runtime_.start(session);

    for (const bool write : {false, true}) {
        try {
            if (write) {
                runtime_.set_block_settings(session, "twin", gr::property_map{{"amplitude", 0.5}}, gr4cp::runtime::BlockSettingsMode::Staged);
            } else {
                (void)runtime_.get_block_settings(session, "twin");
            }
            ADD_FAILURE() << "expected BlockNotFoundError, write=" << write;
        } catch (const gr4cp::runtime::BlockNotFoundError& error) {
            const std::string message = error.what();
            EXPECT_NE(message.find("block name twin names more than one runtime block"), std::string::npos) << message;
            std::size_t candidates = 0;
            for (auto at = message.find("(name=twin)"); at != std::string::npos; at = message.find("(name=twin)", at + 1)) {
                ++candidates;
            }
            EXPECT_EQ(candidates, 2U) << message;
        }
    }

    runtime_.stop(session);
    runtime_.destroy(session);
}

// The binding reaches the block through the load's overrides: the endpoint the block holds is the one the stream route
// serves.
TEST_F(Gr4RuntimeManagerTest, StreamBindingReachesTheBlockThroughItsSettings) {
    gr4cp::test::register_test_blocks();
    auto session = make_session("runtime_stream_binding", R"(blocks:
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    parameters:
      name: "src0"
      sample_rate: 1000.0
      chunk_size: 32
  - id: "gr4cp::test::StreamEndpointSink"
    parameters:
      name: "stream0"
      transport: "websocket"
      payload_format: "series-window-json-v1"
connections:
  - ["src0", 0, "stream0", 0]
)");
    runtime_.prepare(session);
    runtime_.start(session);

    const auto route = runtime_.resolve_websocket_stream(session, "stream0");
    const auto settings = runtime_.get_block_settings(session, "stream0");
    EXPECT_FALSE(route.internal.endpoint.empty());
    ASSERT_TRUE(settings.contains("endpoint"));
    EXPECT_EQ(settings.at("endpoint").value_or(std::string()), route.internal.endpoint);
    EXPECT_EQ(settings.at("transport").value_or(std::string()), "websocket");

    runtime_.stop(session);
    runtime_.destroy(session);
}

// The stream routes refuse a run that is ending by itself, as the settings calls do.
TEST_F(Gr4RuntimeManagerTest, StreamOfARunEndingByItselfIsRefused) {
    gr4cp::test::register_test_blocks();
    auto session = make_session("runtime_stream_run_ending", R"(blocks:
  - id: "gr4cp::test::FiniteSource"
    parameters:
      name: "finite0"
  - id: "gr4cp::test::StreamEndpointSink"
    parameters:
      name: "stream0"
      transport: "websocket"
      payload_format: "series-window-json-v1"
connections:
  - ["finite0", 0, "stream0", 0]
)");
    runtime_.prepare(session);
    runtime_.start(session);
    ASSERT_TRUE(wait_until_run_is_ending(runtime_, session, "stream0"));

    EXPECT_THROW((void)runtime_.resolve_websocket_stream(session, "stream0"), gr4cp::runtime::SessionNotRunningError);
    EXPECT_FALSE(runtime_.active_stream_plan(session).has_value());

    runtime_.stop(session);
    runtime_.destroy(session);
}

// A bound block that declares no `endpoint` setting fails the load, and the error names the block.
TEST_F(Gr4RuntimeManagerTest, StreamBindingOfABlockWithoutAnEndpointFailsPrepareNamingIt) {
    auto session = make_session("runtime_stream_without_endpoint", R"(blocks:
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    parameters:
      name: "src0"
      sample_rate: 1000.0
      chunk_size: 32
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "plain0"
      stream:
        transport: "websocket"
        payload_format: "series-window-json-v1"
connections:
  - ["src0", 0, "plain0", 0]
)");
    try {
        runtime_.prepare(session);
        ADD_FAILURE() << "expected the load to fail";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("prepare failed"), std::string::npos) << message;
        EXPECT_NE(message.find("plain0"), std::string::npos) << message;
    }
    runtime_.destroy(session);
}

// A stop finds the failed run ended by itself, or its scheduler in its error state. It succeeds and hands back the
// run's error.
TEST_F(Gr4RuntimeManagerTest, StopOfARunThatFailedHandsBackItsError) {
    gr4cp::test::register_test_blocks();
    auto session = make_session("runtime_failed_stop", R"(blocks:
  - id: "gr4cp::test::FailingSource"
    parameters:
      name: "failing0"
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "sink0"
connections:
  - ["failing0", 0, "sink0", 0]
)");
    runtime_.prepare(session);
    runtime_.start(session);
    ASSERT_TRUE(wait_until_run_is_ending(runtime_, session, "failing0"));

    std::optional<std::string> run_error;
    EXPECT_NO_THROW(run_error = runtime_.stop(session));
    ASSERT_TRUE(run_error.has_value());
    EXPECT_NE(run_error->find("the source fails"), std::string::npos) << *run_error;
    runtime_.destroy(session);
}

TEST_F(Gr4RuntimeManagerTest, BlockErrorWhileRunningReachesLastError) {
    gr4cp::test::register_test_blocks();
    gr4cp::storage::InMemorySessionRepository repository;
    gr4cp::app::SessionService service(repository, runtime_);
    const auto created = service.create("fault", R"(blocks:
  - id: "gr4cp::test::FaultReportingSource"
    parameters:
      name: "fault0"
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "sink0"
connections:
  - ["fault0", 0, "sink0", 0]
)");

    EXPECT_EQ(service.start(created.id).state, gr4cp::domain::SessionState::Running);
    const auto session = read_until(service, created.id, [](const auto& read) { return read.last_error.has_value(); });
    ASSERT_TRUE(session.last_error.has_value());
    EXPECT_NE(session.last_error->find("the source reports a fault"), std::string::npos) << *session.last_error;
    service.remove(created.id);
}

TEST_F(Gr4RuntimeManagerTest, SchedulerAliasesResolveToCoreSchedulers) {
    EXPECT_EQ(gr4cp::runtime::core_scheduler_type("gr::scheduler::SimpleSingle"), "gr::scheduler::Simple<singleThreaded>");
    EXPECT_EQ(gr4cp::runtime::core_scheduler_type("gr::scheduler::SimpleMulti"), "gr::scheduler::Simple<multiThreaded>");
    EXPECT_EQ(gr4cp::runtime::core_scheduler_type("gr::scheduler::Simple<multiThreaded>"), "gr::scheduler::Simple<multiThreaded>");

    std::ignore = gr::registerBuiltinSchedulers();
    const auto loader = gr4cp::test::plugin_loader();
    const auto available = loader->availableSchedulers();
    std::vector<std::string> session_types;
    for (const auto alias : {std::string_view{"gr::scheduler::SimpleSingle"}, std::string_view{"gr::scheduler::SimpleMulti"}}) {
        const auto core_type = gr4cp::runtime::core_scheduler_type(alias);
        EXPECT_TRUE(std::ranges::binary_search(available, core_type)) << alias;
        const auto expected = created_scheduler_type(loader, core_type);
        ASSERT_TRUE(expected.has_value()) << core_type;

        auto session = make_session(std::format("runtime_alias_{}", alias.substr(alias.rfind(':') + 1)), continuous_graph_yaml());
        session.scheduler_alias = std::string(alias);
        EXPECT_NO_THROW(runtime_.prepare(session)) << alias;
        const auto session_type = runtime_.scheduler_type(session);
        ASSERT_TRUE(session_type.has_value()) << alias;
        EXPECT_EQ(*session_type, *expected) << alias;
        session_types.push_back(*session_type);

        EXPECT_NO_THROW(runtime_.start(session)) << alias;
        std::this_thread::sleep_for(100ms);
        EXPECT_EQ(runtime_.stop(session), std::nullopt) << alias;
        runtime_.destroy(session);
    }
    ASSERT_EQ(session_types.size(), 2U);
    EXPECT_NE(session_types.front(), session_types.back());
}

// The listing registers each alias into the global scheduler registry, and the loader creates the alias's core type
// by the alias's name.
TEST_F(Gr4RuntimeManagerTest, SchedulerCatalogListsAndRegistersTheAliases) {
    const auto loader = gr4cp::test::plugin_loader();
    const auto listed = gr4cp::catalog::Gr4SchedulerCatalogProvider(loader).list();
    for (const auto& [alias, type] : gr4cp::runtime::scheduler_aliases()) {
        EXPECT_TRUE(std::ranges::any_of(listed, [alias](const auto& descriptor) { return descriptor.id == alias; })) << alias;
        const auto expected = created_scheduler_type(loader, type);
        ASSERT_TRUE(expected.has_value()) << type;
        EXPECT_EQ(created_scheduler_type(loader, alias), expected) << alias;
    }
}

// A deleted session destroys its graph, and with it every block the graph held.
TEST_F(Gr4RuntimeManagerTest, DeletedSessionReleasesItsBlocks) {
    gr4cp::test::register_test_blocks();
    auto session = make_session("runtime_release", R"(blocks:
  - id: "gr4cp::test::CountedSource"
    parameters:
      name: "counted0"
  - id: "gr::blocks::testing::NullSink<float32>"
    parameters:
      name: "sink0"
connections:
  - ["counted0", 0, "sink0", 0]
)");
    const auto alive_before = gr4cp::test::CountedSource::alive();
    runtime_.prepare(session);
    EXPECT_EQ(gr4cp::test::CountedSource::alive(), alive_before + 1);
    runtime_.start(session);
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(runtime_.stop(session), std::nullopt);
    runtime_.destroy(session);
    EXPECT_EQ(gr4cp::test::CountedSource::alive(), alive_before);
}

}  // namespace
