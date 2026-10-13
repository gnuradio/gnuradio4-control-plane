#include "gr4cp/app/session_service.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "gr4cp/domain/session.hpp"
#include "gr4cp/runtime/runtime_manager.hpp"
#include "gr4cp/storage/in_memory_session_repository.hpp"

namespace {

std::string unsupported_transport_graph() {
    return R"(blocks:
  - id: "gr::testing::ManagedStreamBlock<float32>"
    parameters:
      name: "spectrum0"
      stream:
        transport: "grpc"
        payload_format: "dataset-xy-json-v1"
connections: []
)";
}

class FakeRuntimeManager final : public gr4cp::runtime::RuntimeManager {
public:
    void prepare(const gr4cp::domain::Session& session) override {
        if (const auto contract_error = gr4cp::domain::validate_stream_runtime_contract(session); contract_error.has_value()) {
            throw std::runtime_error(*contract_error);
        }
        record("prepare");
    }

    void start(const gr4cp::domain::Session&) override {
        record("start");
    }

    std::optional<std::string> stop(const gr4cp::domain::Session&) override {
        record("stop");
        std::unique_lock lock(control_mutex_);
        stop_entered_ = true;
        control_condition_.notify_all();
        control_condition_.wait(lock, [this]() { return !block_stop_; });
        return std::exchange(stop_run_error, std::nullopt);
    }

    void destroy(const gr4cp::domain::Session&) override {
        record("destroy");
    }

    std::optional<gr4cp::domain::StreamRuntimePlan> active_stream_plan(const gr4cp::domain::Session&) override {
        return std::nullopt;
    }

    gr4cp::runtime::HttpStreamResponse fetch_http_stream(const gr4cp::domain::Session&, const std::string&) override {
        record("fetch_http_stream");
        if (http_stream_failures_remaining_ > 0) {
            --http_stream_failures_remaining_;
            throw std::runtime_error("failed to reach internal HTTP stream endpoint");
        }
        return http_stream_response_;
    }

    gr4cp::runtime::WebSocketStreamRoute resolve_websocket_stream(const gr4cp::domain::Session&,
                                                                  const std::string&) override {
        return {};
    }

    void set_block_settings(const gr4cp::domain::Session&,
                            const std::string&,
                            const gr::property_map&,
                            gr4cp::runtime::BlockSettingsMode) override {
        record("set_block_settings");
    }

    gr::property_map get_block_settings(const gr4cp::domain::Session&, const std::string&) override {
        record("get_block_settings");
        return {};
    }

    gr4cp::runtime::RunObservation observe(const gr4cp::domain::Session&) override { return std::exchange(observation, {}); }

    void fail_on(std::string action, std::string message = "boom") {
        fail_action_ = std::move(action);
        fail_message_ = std::move(message);
    }

    void block_stop() {
        std::lock_guard lock(control_mutex_);
        block_stop_ = true;
        stop_entered_ = false;
    }

    void release_stop() {
        std::lock_guard lock(control_mutex_);
        block_stop_ = false;
        control_condition_.notify_all();
    }

    bool wait_for_stop_entry(std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
        std::unique_lock lock(control_mutex_);
        return control_condition_.wait_for(lock, timeout, [this]() { return stop_entered_; });
    }

    void fail_http_stream_fetches(int attempts) {
        http_stream_failures_remaining_ = attempts;
    }

    std::vector<std::string> actions;
    gr4cp::runtime::RunObservation observation;  // what the next observe() reports
    std::optional<std::string> stop_run_error;   // what the next stop() reports
    gr4cp::runtime::HttpStreamResponse http_stream_response_{.status = 200,
                                                             .body = R"({"samples":[1,2,3]})",
                                                             .content_type = "application/json"};

private:
    void record(const std::string& action) {
        actions.push_back(action);
        if (fail_action_ == action) {
            throw std::runtime_error(fail_message_);
        }
    }

    std::string fail_action_;
    std::string fail_message_{"boom"};
    std::mutex control_mutex_;
    std::condition_variable control_condition_;
    bool block_stop_{false};
    bool stop_entered_{false};
    int http_stream_failures_remaining_{0};
};

class SessionServiceTest : public ::testing::Test {
protected:
    gr4cp::storage::InMemorySessionRepository repository;
    FakeRuntimeManager runtime;
    gr4cp::app::SessionService service{repository, runtime};
};

TEST(DomainSessionTest, ConvertsStateToAndFromString) {
    EXPECT_EQ(gr4cp::domain::to_string(gr4cp::domain::SessionState::Stopped), "stopped");
    EXPECT_EQ(gr4cp::domain::to_string(gr4cp::domain::SessionState::Running), "running");
    EXPECT_EQ(gr4cp::domain::to_string(gr4cp::domain::SessionState::Error), "error");

    EXPECT_EQ(gr4cp::domain::session_state_from_string("stopped"), gr4cp::domain::SessionState::Stopped);
    EXPECT_EQ(gr4cp::domain::session_state_from_string("running"), gr4cp::domain::SessionState::Running);
    EXPECT_EQ(gr4cp::domain::session_state_from_string("error"), gr4cp::domain::SessionState::Error);
    EXPECT_THROW(gr4cp::domain::session_state_from_string("nope"), std::invalid_argument);
}

TEST(DomainSessionTest, FormatsTimestampAsIso8601Utc) {
    const auto timestamp = gr4cp::domain::Timestamp{std::chrono::seconds{1704067200}};
    EXPECT_EQ(gr4cp::domain::format_timestamp_utc(timestamp), "2024-01-01T00:00:00Z");
}

TEST_F(SessionServiceTest, CreateSessionSuccess) {
    const auto session = service.create("demo", "graph");

    EXPECT_TRUE(session.id.starts_with("sess_"));
    EXPECT_EQ(session.name, "demo");
    EXPECT_EQ(session.grc_content, "graph");
    EXPECT_EQ(session.state, gr4cp::domain::SessionState::Stopped);
    EXPECT_FALSE(session.last_error.has_value());
    EXPECT_EQ(session.created_at, session.updated_at);

    const auto stored = repository.get(session.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->name, "demo");
}

TEST_F(SessionServiceTest, CreateSessionWithEmptyGrcFails) {
    EXPECT_THROW(service.create("demo", ""), gr4cp::app::ValidationError);
}

TEST_F(SessionServiceTest, GetExistingSession) {
    const auto created = service.create("demo", "graph");

    const auto fetched = service.get(created.id);

    EXPECT_EQ(fetched.id, created.id);
    EXPECT_EQ(fetched.name, "demo");
}

TEST_F(SessionServiceTest, GetMissingSessionFails) {
    EXPECT_THROW(service.get("missing"), gr4cp::app::NotFoundError);
}

TEST_F(SessionServiceTest, ListSessions) {
    const auto first = service.create("one", "graph1");
    const auto second = service.create("two", "graph2");

    const auto sessions = service.list();

    EXPECT_EQ(sessions.size(), 2U);
    EXPECT_TRUE(repository.get(first.id).has_value());
    EXPECT_TRUE(repository.get(second.id).has_value());
}

TEST_F(SessionServiceTest, StartFromStopped) {
    const auto created = service.create("demo", "graph");

    const auto started = service.start(created.id);

    EXPECT_EQ(started.state, gr4cp::domain::SessionState::Running);
    EXPECT_FALSE(started.last_error.has_value());
    EXPECT_GT(started.updated_at, created.updated_at);
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"prepare", "start"}));
}

TEST_F(SessionServiceTest, StartWhenAlreadyRunningFails) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);

    EXPECT_THROW(service.start(created.id), gr4cp::app::InvalidStateError);
}

TEST_F(SessionServiceTest, StopFromRunning) {
    const auto created = service.create("demo", "graph");
    const auto started = service.start(created.id);
    runtime.actions.clear();

    const auto stopped = service.stop(created.id);

    EXPECT_EQ(stopped.state, gr4cp::domain::SessionState::Stopped);
    EXPECT_FALSE(stopped.last_error.has_value());
    EXPECT_GT(stopped.updated_at, started.updated_at);
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"stop"}));
}

TEST_F(SessionServiceTest, StopFromStoppedIsNoOpSuccess) {
    const auto created = service.create("demo", "graph");

    const auto stopped = service.stop(created.id);

    EXPECT_EQ(stopped.state, gr4cp::domain::SessionState::Stopped);
    EXPECT_EQ(stopped.updated_at, created.updated_at);
    EXPECT_TRUE(runtime.actions.empty());
}

TEST_F(SessionServiceTest, FetchHttpStreamRetriesTransientInternalEndpointFailures) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.actions.clear();
    runtime.fail_http_stream_fetches(2);

    const auto response = service.fetch_http_stream(created.id, "series0");

    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, R"({"samples":[1,2,3]})");
    EXPECT_EQ(response.content_type, "application/json");
    EXPECT_EQ(runtime.actions,
              (std::vector<std::string>{"fetch_http_stream", "fetch_http_stream", "fetch_http_stream"}));
}

TEST_F(SessionServiceTest, FetchHttpStreamDoesNotRetryNonTransientRuntimeFailures) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.actions.clear();
    runtime.fail_on("fetch_http_stream", "stream not found in active runtime resources: series0");

    EXPECT_THROW(service.fetch_http_stream(created.id, "series0"), gr4cp::app::RuntimeError);
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"fetch_http_stream"}));
}

TEST_F(SessionServiceTest, RestartFromStopped) {
    const auto created = service.create("demo", "graph");

    const auto restarted = service.restart(created.id);

    EXPECT_EQ(restarted.state, gr4cp::domain::SessionState::Running);
    EXPECT_GT(restarted.updated_at, created.updated_at);
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"destroy", "prepare", "start"}));
}

TEST_F(SessionServiceTest, RestartFromRunning) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.actions.clear();

    const auto restarted = service.restart(created.id);

    EXPECT_EQ(restarted.state, gr4cp::domain::SessionState::Running);
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"stop", "destroy", "prepare", "start"}));
}

TEST_F(SessionServiceTest, RemoveStoppedSession) {
    const auto created = service.create("demo", "graph");

    service.remove(created.id);

    EXPECT_FALSE(repository.get(created.id).has_value());
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"destroy"}));
}

TEST_F(SessionServiceTest, RemoveRunningSession) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.actions.clear();

    service.remove(created.id);

    EXPECT_FALSE(repository.get(created.id).has_value());
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"stop", "destroy"}));
}

TEST_F(SessionServiceTest, RunThatEndedWithAnErrorReadsError) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.observation = {.ended = true, .run_error = "the run failed"};

    const auto read = service.get(created.id);

    EXPECT_EQ(read.state, gr4cp::domain::SessionState::Error);
    EXPECT_EQ(read.last_error, std::optional<std::string>("the run failed"));
    EXPECT_EQ(service.list().front().state, gr4cp::domain::SessionState::Error);
}

TEST_F(SessionServiceTest, RemoveAndRestartReadAnEndedRunBeforeTheTeardown) {
    const auto removed = service.create("removed", "graph");
    service.start(removed.id);
    runtime.fail_on("stop", "the run failed");
    runtime.observation = {.ended = true, .run_error = "the run failed"};
    runtime.actions.clear();

    service.remove(removed.id);

    EXPECT_FALSE(repository.get(removed.id).has_value());
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"destroy"}));

    const auto restarted = service.create("restarted", "graph");
    service.start(restarted.id);
    runtime.observation = {.ended = true, .run_error = "the run failed"};
    runtime.actions.clear();

    EXPECT_EQ(service.restart(restarted.id).state, gr4cp::domain::SessionState::Running);
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"destroy", "prepare", "start"}));
}

TEST_F(SessionServiceTest, StartOfARunThatEndedUnreadStartsItAgain) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.observation = {.ended = true};
    runtime.actions.clear();

    EXPECT_EQ(service.start(created.id).state, gr4cp::domain::SessionState::Running);
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"prepare", "start"}));
}

// The first run ended with its error before the stop read it. The second run's error comes back from the stop itself.
TEST_F(SessionServiceTest, StopOfAFailedRunNobodyReadReportsTheRunsError) {
    const auto ended = service.create("ended", "graph");
    service.start(ended.id);
    runtime.observation = {.ended = true, .run_error = "the run failed"};
    runtime.actions.clear();

    const auto stopped_after_its_end = service.stop(ended.id);

    EXPECT_EQ(stopped_after_its_end.state, gr4cp::domain::SessionState::Error);
    EXPECT_EQ(stopped_after_its_end.last_error, std::optional<std::string>("the run failed"));
    EXPECT_TRUE(runtime.actions.empty());
    EXPECT_EQ(service.get(ended.id).last_error, std::optional<std::string>("the run failed"));

    const auto ending = service.create("ending", "graph");
    service.start(ending.id);
    runtime.stop_run_error = "the run failed as it stopped";
    runtime.actions.clear();

    const auto stopped_in_its_end = service.stop(ending.id);

    EXPECT_EQ(stopped_in_its_end.state, gr4cp::domain::SessionState::Error);
    EXPECT_EQ(stopped_in_its_end.last_error, std::optional<std::string>("the run failed as it stopped"));
    EXPECT_EQ(runtime.actions, (std::vector<std::string>{"stop"}));
    EXPECT_EQ(service.get(ending.id).state, gr4cp::domain::SessionState::Error);
}

TEST_F(SessionServiceTest, EndedRunIsDestroyedAfterTheServiceReleasesItsLock) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    std::future<void> other_read;
    std::optional<bool> other_read_answered;
    runtime.observation.ended = true;
    runtime.observation.ended_run = std::shared_ptr<int>(new int{0}, [&](int* run) {
        other_read = std::async(std::launch::async, [this] { (void)service.list(); });
        other_read_answered = other_read.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        delete run;
    });

    EXPECT_EQ(service.get(created.id).state, gr4cp::domain::SessionState::Stopped);
    ASSERT_TRUE(other_read_answered.has_value());
    EXPECT_TRUE(*other_read_answered);
}

TEST_F(SessionServiceTest, RuntimeFailureCausesErrorStateAndLastErrorToBeSet) {
    const auto created = service.create("demo", "graph");
    runtime.fail_on("start", "start exploded");

    EXPECT_THROW(service.start(created.id), gr4cp::app::RuntimeError);

    const auto stored = repository.get(created.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->state, gr4cp::domain::SessionState::Error);
    ASSERT_TRUE(stored->last_error.has_value());
    EXPECT_NE(stored->last_error->find("start exploded"), std::string::npos);
}

TEST_F(SessionServiceTest, RestartDuringSlowStopIsRejectedDeterministically) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.actions.clear();
    runtime.block_stop();

    auto stop_future = std::async(std::launch::async, [this, id = created.id]() { return service.stop(id); });
    ASSERT_TRUE(runtime.wait_for_stop_entry());

    EXPECT_THROW(service.restart(created.id), gr4cp::app::InvalidStateError);

    runtime.release_stop();
    const auto stopped = stop_future.get();
    EXPECT_EQ(stopped.state, gr4cp::domain::SessionState::Stopped);
}

TEST_F(SessionServiceTest, RemoveDuringSlowStopPreservesSessionRecord) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.actions.clear();
    runtime.block_stop();

    auto stop_future = std::async(std::launch::async, [this, id = created.id]() { return service.stop(id); });
    ASSERT_TRUE(runtime.wait_for_stop_entry());

    EXPECT_THROW(service.remove(created.id), gr4cp::app::InvalidStateError);
    EXPECT_TRUE(repository.get(created.id).has_value());

    runtime.release_stop();
    (void)stop_future.get();
}

TEST_F(SessionServiceTest, StopFailureMarksSessionAsErrorWithTeardownMessage) {
    const auto created = service.create("demo", "graph");
    service.start(created.id);
    runtime.fail_on("stop", "teardown timed out after 2000 ms");

    EXPECT_THROW(service.stop(created.id), gr4cp::app::RuntimeError);

    const auto stored = repository.get(created.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->state, gr4cp::domain::SessionState::Error);
    ASSERT_TRUE(stored->last_error.has_value());
    EXPECT_NE(stored->last_error->find("teardown timed out"), std::string::npos);
}

TEST_F(SessionServiceTest, StartFailsExplicitlyForUnsupportedAuthoredTransport) {
    const auto created = service.create("demo", unsupported_transport_graph());

    EXPECT_THROW(service.start(created.id), gr4cp::app::RuntimeError);

    const auto stored = repository.get(created.id);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->state, gr4cp::domain::SessionState::Error);
    ASSERT_TRUE(stored->last_error.has_value());
    EXPECT_NE(stored->last_error->find("managed stream transport is not supported"), std::string::npos);
}

}  // namespace
