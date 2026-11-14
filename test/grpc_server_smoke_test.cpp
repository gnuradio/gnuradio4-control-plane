#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "gr4cp/v1/control_plane.grpc.pb.h"

extern char **environ;

namespace {

namespace v1 = gr4cp::v1;

// The path of the server executable, from the variable ctest sets for these
// tests. An unset variable fails the test with its name.
std::string required_path(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    throw std::runtime_error(std::string(name) +
                             " is not set; run the test through ctest");
  }
  return value;
}

// The port in a port file, or nothing while the server has not finished writing
// it.
std::optional<int> read_port_file(const std::filesystem::path &path) {
  std::ifstream input(path);
  std::string line;
  if (!input || !std::getline(input, line) || input.eof() || line.empty()) {
    return std::nullopt;
  }
  return std::stoi(line);
}

// Runs the server executable with free REST and gRPC ports, and ends it when
// destroyed.
class ServerProcess {
public:
  ServerProcess() {
    directory_ = std::filesystem::temp_directory_path() /
                 ("gr4cp-grpc-smoke-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory_);

    const std::string executable = required_path("GR4CP_SERVER_EXECUTABLE");
    std::vector<std::string> variables;
    for (char **entry = environ; *entry != nullptr; ++entry) {
      const std::string variable(*entry);
      if (!variable.starts_with("GR4CP_PORT") &&
          !variable.starts_with("GR4CP_GRPC_PORT")) {
        variables.push_back(variable);
      }
    }
    variables.push_back("GR4CP_PORT=0");
    variables.push_back("GR4CP_PORT_FILE=" + rest_port_file().string());
    variables.push_back("GR4CP_GRPC_PORT=0");
    variables.push_back("GR4CP_GRPC_PORT_FILE=" + grpc_port_file().string());

    std::vector<char *> environment;
    for (auto &variable : variables) {
      environment.push_back(variable.data());
    }
    environment.push_back(nullptr);
    std::string argument = executable;
    char *arguments[] = {argument.data(), nullptr};

    if (posix_spawn(&pid_, executable.c_str(), nullptr, nullptr, arguments,
                    environment.data()) != 0) {
      pid_ = -1;
      throw std::runtime_error("failed to launch " + executable);
    }
  }

  ServerProcess(const ServerProcess &) = delete;
  ServerProcess &operator=(const ServerProcess &) = delete;

  ~ServerProcess() {
    if (pid_ > 0 && !exited_) {
      (void)kill(pid_, SIGKILL);
      (void)waitpid(pid_, nullptr, 0);
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  std::filesystem::path rest_port_file() const {
    return directory_ / "rest.port";
  }
  std::filesystem::path grpc_port_file() const {
    return directory_ / "grpc.port";
  }

  // Waits for the server to write the port file. Returns nothing when the
  // server exits or the time runs out.
  std::optional<int> wait_for_port(const std::filesystem::path &path,
                                   std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (const auto port = read_port_file(path)) {
        return port;
      }
      if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
        exited_ = true;
        return std::nullopt;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return std::nullopt;
  }

private:
  std::filesystem::path directory_;
  pid_t pid_{-1};
  bool exited_{false};
};

// Starts the server executable and connects a gRPC channel and a REST client to
// it.
class GrpcServerExecutableTest : public ::testing::Test {
protected:
  void SetUp() override {
    // The gRPC API starts after the catalogs load, so its port file is the last
    // one written.
    grpc_port = server.wait_for_port(server.grpc_port_file(),
                                     std::chrono::seconds(120));
    ASSERT_TRUE(grpc_port.has_value())
        << "the server exited or did not write GR4CP_GRPC_PORT_FILE";
    ASSERT_GT(*grpc_port, 0);
    rest_port = read_port_file(server.rest_port_file());
    ASSERT_TRUE(rest_port.has_value());

    channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(*grpc_port),
                                  grpc::InsecureChannelCredentials());
    ASSERT_TRUE(channel->WaitForConnected(std::chrono::system_clock::now() +
                                          std::chrono::seconds(10)));
    sessions = v1::SessionService::NewStub(channel);
    settings = v1::BlockSettingsService::NewStub(channel);
  }

  // The REST error code a failed call carries in its trailing metadata, or an
  // empty string.
  static std::string error_code(const grpc::ClientContext &context) {
    const auto &trailers = context.GetServerTrailingMetadata();
    const auto it = trailers.find("gr4cp-error-code");
    return it == trailers.end()
               ? std::string()
               : std::string(it->second.data(), it->second.size());
  }

  v1::Session create_session(const std::string &grc) {
    v1::CreateSessionRequest request;
    request.set_name("smoke");
    request.set_grc(grc);
    v1::Session session;
    grpc::ClientContext context;
    const auto status = sessions->CreateSession(&context, request, &session);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return session;
  }

  grpc::Status read_settings(const std::string &id, const std::string &block,
                             v1::GetBlockSettingsResponse &response) {
    v1::GetBlockSettingsRequest request;
    request.set_session_id(id);
    request.set_block_unique_name(block);
    grpc::ClientContext context;
    return settings->GetBlockSettings(&context, request, &response);
  }

  ServerProcess server;
  std::optional<int> grpc_port;
  std::optional<int> rest_port;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<v1::SessionService::Stub> sessions;
  std::unique_ptr<v1::BlockSettingsService::Stub> settings;
};

TEST_F(GrpcServerExecutableTest, ServesBothApisOverTheSameSessions) {
  EXPECT_NE(*rest_port, *grpc_port);
  httplib::Client rest("127.0.0.1", *rest_port);

  v1::GetHealthResponse health;
  {
    grpc::ClientContext context;
    const auto status = v1::ServerService::NewStub(channel)->GetHealth(
        &context, v1::GetHealthRequest{}, &health);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }
  EXPECT_TRUE(health.ok());
  const auto rest_health = rest.Get("/healthz");
  ASSERT_TRUE(rest_health);
  ASSERT_EQ(rest_health->status, 200);
  EXPECT_EQ(nlohmann::json::parse(rest_health->body).at("server_instance_id"),
            health.server_instance_id());

  v1::GetVersionResponse version;
  {
    grpc::ClientContext context;
    const auto status = v1::ServerService::NewStub(channel)->GetVersion(
        &context, v1::GetVersionRequest{}, &version);
    ASSERT_TRUE(status.ok()) << status.error_message();
  }
  EXPECT_FALSE(version.version().empty());
  EXPECT_EQ(version.server_instance_id(), health.server_instance_id());
  const auto rest_version = rest.Get("/version");
  ASSERT_TRUE(rest_version);
  ASSERT_EQ(rest_version->status, 200);
  const auto rest_version_body = nlohmann::json::parse(rest_version->body);
  EXPECT_EQ(rest_version_body.at("version"), version.version());
  EXPECT_EQ(rest_version_body.at("server_instance_id"),
            health.server_instance_id());

  {
    grpc::ClientContext context;
    v1::ListBlocksResponse blocks;
    const auto status = v1::CatalogService::NewStub(channel)->ListBlocks(
        &context, v1::ListBlocksRequest{}, &blocks);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_GT(blocks.blocks_size(), 0);
  }

  const auto created = create_session("graph");
  const auto rest_session = rest.Get("/sessions/" + created.id());
  ASSERT_TRUE(rest_session);
  ASSERT_EQ(rest_session->status, 200);
  EXPECT_EQ(nlohmann::json::parse(rest_session->body).at("name"), "smoke");

  const auto removed = rest.Delete("/sessions/" + created.id());
  ASSERT_TRUE(removed);
  {
    grpc::ClientContext context;
    v1::GetSessionRequest request;
    request.set_session_id(created.id());
    v1::Session session;
    EXPECT_EQ(sessions->GetSession(&context, request, &session).error_code(),
              grpc::StatusCode::NOT_FOUND);
  }
}

// Runs a clocked signal generator into a counting sink in the GNU Radio 4
// runtime, with every call made over gRPC.
TEST_F(GrpcServerExecutableTest, RunsAFlowgraphAndItsLiveSettings) {
  const auto created = create_session(R"(blocks:
  - id: "gr::blocks::basic::ClockSource"
    parameters:
      name: "clock0"
      sample_rate: 1000.0
      chunk_size: 32
      n_samples_max: 0
  - id: "gr::blocks::basic::SignalGenerator<float32>"
    parameters:
      name: "sig0"
      sample_rate: 1000.0
  - id: "gr::blocks::testing::CountingSink<float32>"
    parameters:
      name: "count0"
connections:
  - ["clock0", 0, "sig0", 0]
  - ["sig0", 0, "count0", 0]
)");
  ASSERT_FALSE(created.id().empty());

  {
    grpc::ClientContext context;
    v1::StartSessionRequest request;
    request.set_session_id(created.id());
    v1::Session started;
    const auto status = sessions->StartSession(&context, request, &started);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(started.state(), v1::SESSION_STATE_RUNNING);
    EXPECT_TRUE(started.has_execution_id());
  }

  // The sink's 64-bit count arrives as an unsigned integer. A live read answers
  // with the block's settings, which the sink's work does not write, so the
  // count itself is checked after the stop.
  {
    v1::GetBlockSettingsResponse read;
    const auto status = read_settings(created.id(), "count0", read);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_TRUE(read.settings().contains("count"));
    EXPECT_EQ(read.settings().at("count").kind_case(), v1::Value::kUintValue);
  }

  {
    grpc::ClientContext context;
    v1::UpdateBlockSettingsRequest request;
    request.set_session_id(created.id());
    request.set_block_unique_name("sig0");
    request.set_mode(v1::BLOCK_SETTINGS_MODE_IMMEDIATE);
    (*request.mutable_settings())["amplitude"].set_double_value(0.5);
    v1::UpdateBlockSettingsResponse updated;
    const auto status =
        settings->UpdateBlockSettings(&context, request, &updated);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_TRUE(updated.accepted());
    EXPECT_EQ(updated.applied_via(), "settings");
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  double amplitude = 0.0;
  do {
    v1::GetBlockSettingsResponse read;
    const auto status = read_settings(created.id(), "sig0", read);
    ASSERT_TRUE(status.ok()) << status.error_message();
    ASSERT_TRUE(read.settings().contains("amplitude"));
    amplitude = read.settings().at("amplitude").double_value();
  } while (amplitude != 0.5 && std::chrono::steady_clock::now() < deadline);
  EXPECT_EQ(amplitude, 0.5);

  // The clock paces the graph at 1000 samples a second, so this lets a few
  // chunks reach the sink.
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  {
    grpc::ClientContext context;
    v1::StopSessionRequest request;
    request.set_session_id(created.id());
    v1::Session stopped;
    const auto status = sessions->StopSession(&context, request, &stopped);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(stopped.state(), v1::SESSION_STATE_STOPPED);
    EXPECT_EQ(stopped.completion_reason(), v1::COMPLETION_REASON_STOPPED);
  }

  // A stopped run answers with the settings it left, here the number of samples
  // that reached the sink.
  v1::GetBlockSettingsResponse final_read;
  const auto status = read_settings(created.id(), "count0", final_read);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(final_read.settings().at("count").kind_case(),
            v1::Value::kUintValue);
  EXPECT_GT(final_read.settings().at("count").uint_value(), 0U);
}

// Content the runtime cannot load fails the start as a runtime error and leaves
// the session in the error state.
TEST_F(GrpcServerExecutableTest, StartOfAGraphTheRuntimeRefusesIsInternal) {
  const auto created = create_session("not a flowgraph");
  {
    grpc::ClientContext context;
    v1::StartSessionRequest request;
    request.set_session_id(created.id());
    v1::Session started;
    const auto status = sessions->StartSession(&context, request, &started);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
    EXPECT_EQ(error_code(context), "runtime_error");
  }
  grpc::ClientContext context;
  v1::GetSessionRequest request;
  request.set_session_id(created.id());
  v1::Session session;
  ASSERT_TRUE(sessions->GetSession(&context, request, &session).ok());
  EXPECT_EQ(session.state(), v1::SESSION_STATE_ERROR);
  EXPECT_TRUE(session.has_last_error());
}

} // namespace
