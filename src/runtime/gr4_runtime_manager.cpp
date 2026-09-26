#include "gr4cp/runtime/gr4_runtime_manager.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <concepts>
#include <cstdlib>
#include <expected>
#include <format>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
#include <gnuradio-4.0/SchedulerRegistration.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>
#ifndef __EMSCRIPTEN__
#include <httplib.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace gr4cp::runtime {

namespace {

bool stream_debug_enabled() {
    if (const char* env = std::getenv("GR4CP_STREAM_DEBUG"); env != nullptr) {
        return *env != '\0' && std::string_view(env) != "0";
    }
    return false;
}

void stream_debug(std::string_view message) {
    if (!stream_debug_enabled()) {
        return;
    }
    std::clog << "[gr4cp stream] " << message << '\n';
}

std::runtime_error runtime_error(const domain::Session& session, std::string_view phase, const std::string& detail) {
    return std::runtime_error(std::format("session {} {} failed: {}", session.id, phase, detail));
}

constexpr std::array<SchedulerAlias, 2> kSchedulerAliases{{
    {"gr::scheduler::SimpleSingle", "gr::scheduler::Simple<singleThreaded>"},
    {"gr::scheduler::SimpleMulti", "gr::scheduler::Simple<multiThreaded>"},
}};

// Creates the core scheduler type the alias at `Index` names.
template <std::size_t Index>
std::unique_ptr<gr::SchedulerModel> make_aliased_scheduler(gr::property_map parameters) {
    return gr::globalSchedulerRegistry().create(kSchedulerAliases[Index].type, std::move(parameters));
}

std::string default_scheduler_type(const gr::PluginLoader& loader) {
    std::ignore = gr::registerBuiltinSchedulers();
    const auto available = loader.availableSchedulers();

    for (const auto preferred : {
             std::string_view{"gr::incubator::scheduler::BlockingBackoff"},
             std::string_view{"gr::scheduler::SimpleMulti"},
             std::string_view{"gr::scheduler::SimpleSingle"},
         }) {
        if (const auto type = core_scheduler_type(preferred); std::ranges::binary_search(available, type)) {
            return type;
        }
    }

    if (!available.empty()) {
        return available.front();
    }
    return std::string(gr::Runtime::kDefaultScheduler);
}

std::optional<std::string> value_as_string(const gr::pmt::Value& value) {
    std::optional<std::string> result;
    gr::pmt::ValueVisitor([&result](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, std::pmr::string> || std::same_as<T, std::string>) {
            result = std::string(item);
        } else if constexpr (std::same_as<T, std::string_view>) {
            result = std::string(item);
        }
    }).visit(value);
    return result;
}

std::optional<std::string> settings_name(const gr::BlockHandle& block) {
    const auto value = block.get(std::string("name"));
    return value.has_value() ? value_as_string(*value) : std::nullopt;
}

std::string describe_runtime_block(const gr::BlockHandle& block) {
    std::string description(block.uniqueName());

    if (const auto name = std::string(block.name()); !name.empty() && name != description) {
        description += std::format(" (name={})", name);
    }

    if (const auto type_name = std::string(block.typeName()); !type_name.empty()) {
        description += std::format(" [type={}]", type_name);
    }

    if (const auto name = settings_name(block);
        name.has_value() && !name->empty() && *name != block.uniqueName() && *name != block.name()) {
        description += std::format(" [settings.name={}]", *name);
    }

    return description;
}

BlockNotFoundError make_block_not_found_error(const std::map<std::string, gr::BlockHandle, std::less<>>& blocks,
                                              std::string_view block_unique_name) {
    std::string available;
    for (const auto& [_, block] : blocks) {
        available += available.empty() ? describe_runtime_block(block) : ", " + describe_runtime_block(block);
    }
    return BlockNotFoundError(std::format("block not found in running session: {}; available runtime blocks: {}",
                                          block_unique_name,
                                          available.empty() ? "<none>" : available));
}

// The name rule: a block's name, then its unique name, then its `name` setting. A name or a `name` setting two blocks
// carry is refused with the candidates.
gr::BlockHandle* resolve_runtime_block(std::map<std::string, gr::BlockHandle, std::less<>>& blocks,
                                       std::string_view requested_block_target) {
    const auto only_match = [&blocks, requested_block_target](auto carries) -> gr::BlockHandle* {
        std::vector<gr::BlockHandle*> matches;
        for (auto& [_, block] : blocks) {
            if (carries(block)) {
                matches.push_back(&block);
            }
        }
        if (matches.size() > 1) {
            std::string candidates;
            for (const auto* block : matches) {
                candidates += candidates.empty() ? describe_runtime_block(*block) : ", " + describe_runtime_block(*block);
            }
            throw BlockNotFoundError(std::format("block name {} names more than one runtime block: {}",
                                                 requested_block_target,
                                                 candidates));
        }
        return matches.empty() ? nullptr : matches.front();
    };

    if (auto* block = only_match([requested_block_target](const gr::BlockHandle& block) {
            return block.name() == requested_block_target;
        })) {
        return block;
    }
    if (const auto it = blocks.find(requested_block_target); it != blocks.end()) {
        return &it->second;
    }
    return only_match([requested_block_target](const gr::BlockHandle& block) {
        return settings_name(block) == requested_block_target;
    });
}

SessionNotRunningError not_running(const domain::Session& session, std::string_view action) {
    return SessionNotRunningError(std::format("session {} {} failed: session runtime is not running", session.id, action));
}

void normalize_block(gr::property_map& block) {
    std::optional<std::string> studio_node_id;
    if (const auto it = block.find("id"); it != block.end()) {
        studio_node_id = value_as_string(it->second);
    }

    if (const auto it = block.find("block"); it != block.end()) {
        if (const auto block_type = value_as_string(it->second); block_type.has_value() && !block_type->empty()) {
            block["id"] = *block_type;
        }
    }

    if (!block.contains("id")) {
        if (const auto it = block.find("block_type"); it != block.end()) {
            if (const auto block_type = value_as_string(it->second); block_type.has_value() && !block_type->empty()) {
                block["id"] = *block_type;
            }
        }
    }

    gr::property_map parameters;
    bool needs_parameters_write = false;

    if (const auto it = block.find("parameters"); it != block.end()) {
        if (const auto* existing = it->second.get_if<gr::property_map>(); existing != nullptr) {
            parameters = *existing;
        }
    }

    if (const auto it = block.find("raw_parameters"); it != block.end()) {
        if (const auto* raw = it->second.get_if<gr::property_map>(); raw != nullptr) {
            for (const auto& [key, value] : *raw) {
                if (!parameters.contains(key)) {
                    parameters.insert_or_assign(key, value);
                    needs_parameters_write = true;
                }
            }
        }
    }

    if (!parameters.contains("name")) {
        if (studio_node_id.has_value() && !studio_node_id->empty()) {
            parameters["name"] = *studio_node_id;
            needs_parameters_write = true;
        }
    }

    if (!parameters.contains("name")) {
        for (const auto* candidate_key : {"instance_name", "name"}) {
            const auto it = block.find(candidate_key);
            if (it == block.end()) {
                continue;
            }
            if (const auto name = value_as_string(it->second); name.has_value() && !name->empty()) {
                parameters["name"] = *name;
                needs_parameters_write = true;
                break;
            }
        }
    }

    if (needs_parameters_write || !parameters.empty()) {
        block["parameters"] = parameters;
    }
}

bool normalize_connection(gr::pmt::Value& connection) {
    auto normalize_port = [](const std::string& token) -> gr::pmt::Value {
        if (token == "in" || token == "out") {
            return std::int64_t{0};
        }
        if (!token.empty() && std::all_of(token.begin(), token.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return std::int64_t{std::stoll(token)};
        }
        if (token.size() > 2 && (token.rfind("in", 0) == 0 || token.rfind("out", 0) == 0)) {
            const auto suffix = token.substr(token.rfind('t') + 1);
            if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
                return std::int64_t{std::stoll(suffix)};
            }
        }
        return token;
    };

    if (auto* sequence = connection.get_if<gr::Tensor<gr::pmt::Value>>(); sequence != nullptr) {
        bool changed = false;
        if (sequence->size() >= 4) {
            for (const std::size_t index : {std::size_t{1}, std::size_t{3}}) {
                if (const auto token = value_as_string((*sequence)[index]); token.has_value()) {
                    const auto normalized = normalize_port(*token);
                    changed = changed || (normalized != (*sequence)[index]);
                    (*sequence)[index] = normalized;
                }
            }
        }
        return changed;
    }

    const auto* map = connection.get_if<gr::property_map>();
    if (map == nullptr) {
        return false;
    }

    auto get_required_string = [&](std::string_view key) -> std::optional<std::string> {
        const auto it = map->find(std::string(key));
        if (it == map->end()) {
            return std::nullopt;
        }
        return value_as_string(it->second);
    };

    const auto source_block = get_required_string("source_block");
    const auto source_port = get_required_string("source_port_token");
    const auto dest_block = get_required_string("dest_block");
    const auto dest_port = get_required_string("dest_port_token");
    if (!source_block.has_value() || !source_port.has_value() || !dest_block.has_value() || !dest_port.has_value()) {
        return false;
    }

    gr::Tensor<gr::pmt::Value> sequence;
    sequence.reserve(4);
    sequence.push_back(*source_block);
    sequence.push_back(normalize_port(*source_port));
    sequence.push_back(*dest_block);
    sequence.push_back(normalize_port(*dest_port));
    connection = std::move(sequence);
    return true;
}

std::string normalize_graph_content(std::string_view content) {
    const auto parsed = gr::pmt::yaml::deserialize(content);
    if (!parsed.has_value()) {
        return std::string(content);
    }

    auto root = *parsed;
    bool changed = false;

    if (auto it = root.find("blocks"); it != root.end()) {
        if (auto* blocks = it->second.get_if<gr::Tensor<gr::pmt::Value>>(); blocks != nullptr) {
            for (auto& block_value : *blocks) {
                if (auto* block = block_value.get_if<gr::property_map>(); block != nullptr) {
                    const auto before = *block;
                    normalize_block(*block);
                    changed = changed || (*block != before);
                }
            }
        }
    }

    if (auto it = root.find("connections"); it != root.end()) {
        if (auto* connections = it->second.get_if<gr::Tensor<gr::pmt::Value>>(); connections != nullptr) {
            for (auto& connection : *connections) {
                changed = normalize_connection(connection) || changed;
            }
        }
    }

    return changed ? gr::pmt::yaml::serialize(root) : std::string(content);
}

}  // namespace

std::span<const SchedulerAlias> scheduler_aliases() {
    return kSchedulerAliases;
}

void register_scheduler_aliases() {
    static const bool registered = []<std::size_t... Index>(std::index_sequence<Index...>) {
        std::ignore = gr::registerBuiltinSchedulers();
        auto& registry = gr::globalSchedulerRegistry();
        ((std::ignore = registry.insert(kSchedulerAliases[Index].alias,
                                        "",
                                        &make_aliased_scheduler<Index>,
                                        registry.attributes(kSchedulerAliases[Index].type).value_or(gr::property_map{}))),
         ...);
        return true;
    }(std::make_index_sequence<kSchedulerAliases.size()>{});
    std::ignore = registered;
}

std::string core_scheduler_type(std::string_view scheduler_id) {
    for (const auto& [alias, type] : kSchedulerAliases) {
        if (alias == scheduler_id) {
            return std::string(type);
        }
    }
    return std::string(scheduler_id);
}

namespace {

// Returns every malloc arena's free pages to the system when it is destroyed. glibc serves each new thread from an arena
// of its own, up to eight per core, and keeps freed pages resident in their arena. A server that builds each session's
// graph on the thread that took the request otherwise holds one graph's pages per such thread after the sessions are
// deleted.
struct HeapRelease {
    HeapRelease() = default;
    HeapRelease(const HeapRelease&) = delete;
    HeapRelease& operator=(const HeapRelease&) = delete;
    ~HeapRelease() {
#if defined(__GLIBC__)
        ::malloc_trim(0);
#endif
    }
};

}  // namespace

struct Gr4RuntimeManager::Execution {
    HeapRelease heap_release;  // declared first, so that it is destroyed after the runtime and the blocks
    std::optional<gr::Runtime> runtime;
    std::map<std::string, gr::BlockHandle, std::less<>> blocks;  // every block of the graph, by unique name
    bool running{false};
};

// A session's run, held under its resources' lock.
struct Gr4RuntimeManager::RunningExecution {
    std::shared_ptr<SessionRuntimeResources> resources;
    std::unique_lock<std::mutex> lock;
    Execution& execution;
};

struct Gr4RuntimeManager::SessionRuntimeResources {
    std::string session_id;
    std::uint64_t generation{0};
    LifecyclePhase lifecycle_phase{LifecyclePhase::Idle};
    bool teardown_complete{true};
    std::optional<std::string> teardown_error;
    std::unique_ptr<Execution> execution;
    std::vector<domain::RuntimeStreamBinding> stream_bindings;
    std::mutex mutex;
};

Gr4RuntimeManager::Gr4RuntimeManager(std::shared_ptr<gr::PluginLoader> loader) : loader_(std::move(loader)) {}

Gr4RuntimeManager::~Gr4RuntimeManager() {
    std::vector<std::shared_ptr<SessionRuntimeResources>> resources;
    {
        std::lock_guard lock(resources_mutex_);
        resources.reserve(resources_.size());
        for (const auto& [_, resource] : resources_) {
            resources.push_back(resource);
        }
        resources_.clear();
    }

    for (const auto& resource : resources) {
        if (!resource) {
            continue;
        }
        try {
            domain::Session session;
            session.id = resource->session_id;
            std::unique_lock resource_lock(resource->mutex);
            std::ignore = stop_locked(session, *resource, resource_lock);
        } catch (...) {
        }

    }
}

void Gr4RuntimeManager::prepare(const domain::Session& session) {
    auto resources = get_or_create_resources(session);
    std::lock_guard resource_lock(resources->mutex);
    (void)prepare_locked(session, *resources);
}

void Gr4RuntimeManager::start(const domain::Session& session) {
    auto resources = get_or_create_resources(session);
    std::unique_lock resource_lock(resources->mutex);
    auto& execution = prepare_locked(session, *resources);

    if (resources->lifecycle_phase == LifecyclePhase::Stopping) {
        throw runtime_error(session, "start", "session runtime teardown is still in progress");
    }
    if (execution.running && resources->lifecycle_phase == LifecyclePhase::Running) {
        return;
    }

    resources->teardown_error.reset();
    resources->teardown_complete = false;
    resources->lifecycle_phase = LifecyclePhase::Starting;
    if (const auto error = execution.runtime->start(); error.has_value()) {
        resources->lifecycle_phase = LifecyclePhase::Idle;
        resources->teardown_complete = true;
        throw runtime_error(session, "start", error->message);
    }
    execution.running = true;
    resources->lifecycle_phase = LifecyclePhase::Running;
}

std::optional<std::string> Gr4RuntimeManager::stop(const domain::Session& session) {
    auto resources = find_resources(session.id);
    if (!resources) {
        return std::nullopt;
    }

    std::unique_lock resource_lock(resources->mutex);
    return stop_locked(session, *resources, resource_lock);
}

void Gr4RuntimeManager::destroy(const domain::Session& session) {
    auto resources = find_resources(session.id);
    if (!resources) {
        return;
    }

    {
        std::unique_lock resource_lock(resources->mutex);
        std::ignore = stop_locked(session, *resources, resource_lock);
        resources->execution.reset();
        release_stream_bindings_locked(*resources);
        resources->lifecycle_phase = LifecyclePhase::Idle;
        resources->teardown_complete = true;
        resources->teardown_error.reset();
    }

    std::lock_guard lock(resources_mutex_);
    const auto it = resources_.find(session.id);
    if (it != resources_.end() && it->second == resources) {
        resources_.erase(it);
    }
}

std::optional<domain::StreamRuntimePlan> Gr4RuntimeManager::active_stream_plan(const domain::Session& session) {
    const auto run = live_execution(session);
    if (!run.has_value() || run->resources->stream_bindings.empty()) {
        return std::nullopt;
    }

    domain::StreamRuntimePlan plan;
    for (const auto& binding : run->resources->stream_bindings) {
        plan.streams.push_back(binding.plan);
    }
    return plan;
}

HttpStreamResponse Gr4RuntimeManager::fetch_http_stream(const domain::Session& session, const std::string& stream_id) {
#ifdef __EMSCRIPTEN__
    (void)stream_id;
    throw runtime_error(session, "stream fetch", "HTTP stream fetch is not available in WASM builds");
#else
    domain::InternalStreamBinding internal;
    {
        const auto run = live_execution(session);
        if (!run.has_value()) {
            throw not_running(session, "stream fetch");
        }

        const auto& bindings = run->resources->stream_bindings;
        const auto binding_it = std::ranges::find_if(bindings, [&](const auto& binding) {
            return binding.plan.stream_id == stream_id && binding.plan.transport != "websocket";
        });
        if (binding_it == bindings.end()) {
            throw runtime_error(session, "stream fetch", "stream not found in active runtime resources: " + stream_id);
        }
        internal = binding_it->internal;
    }

    stream_debug(std::format("session={} stream_id={} fetch_target={}", session.id, stream_id, internal.endpoint));

    httplib::Client client(internal.host, internal.port);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(2, 0);
    const auto response = client.Get(internal.path.c_str());
    if (!response) {
        stream_debug(std::format("session={} stream_id={} fetch_failed target={} error={}",
                                 session.id,
                                 stream_id,
                                 internal.endpoint,
                                 static_cast<int>(response.error())));
        throw runtime_error(session, "stream fetch", "failed to reach internal HTTP stream endpoint");
    }

    stream_debug(std::format("session={} stream_id={} fetch_status={} target={}",
                             session.id,
                             stream_id,
                             response->status,
                             internal.endpoint));

    auto content_type = response->get_header_value("Content-Type");
    if (content_type.empty()) {
        content_type = "application/octet-stream";
    }
    return HttpStreamResponse{
        .status = response->status,
        .body = response->body,
        .content_type = content_type,
    };
#endif
}

WebSocketStreamRoute Gr4RuntimeManager::resolve_websocket_stream(const domain::Session& session, const std::string& stream_id) {
    const auto run = live_execution(session);
    if (!run.has_value()) {
        throw not_running(session, "websocket route");
    }

    const auto& bindings = run->resources->stream_bindings;
    const auto binding_it = std::ranges::find_if(bindings, [&](const auto& binding) {
        return binding.plan.stream_id == stream_id && binding.plan.transport == "websocket";
    });
    if (binding_it == bindings.end()) {
        throw runtime_error(session, "websocket route", "stream not found in active runtime resources: " + stream_id);
    }

    return WebSocketStreamRoute{.internal = binding_it->internal};
}

void Gr4RuntimeManager::set_block_settings(const domain::Session& session,
                                           const std::string& block_target,
                                           const gr::property_map& patch,
                                           BlockSettingsMode mode) {
    (void)mode;
    auto run = live_execution(session);
    if (!run.has_value()) {
        throw not_running(session, "settings update");
    }

    auto* block = resolve_runtime_block(run->execution.blocks, block_target);
    if (block == nullptr) {
        throw make_block_not_found_error(run->execution.blocks, block_target);
    }
    if (const auto staged = block->setStaged(patch); !staged.has_value()) {
        throw runtime_error(session, "settings update", staged.error().message);
    }
}

gr::property_map Gr4RuntimeManager::get_block_settings(const domain::Session& session, const std::string& block_target) {
    auto run = live_execution(session);
    if (!run.has_value()) {
        throw not_running(session, "settings read");
    }

    auto* block = resolve_runtime_block(run->execution.blocks, block_target);
    if (block == nullptr) {
        throw make_block_not_found_error(run->execution.blocks, block_target);
    }
    return block->get();
}

RunObservation Gr4RuntimeManager::observe(const domain::Session& session) {
    auto run = running_execution(session);
    if (!run.has_value()) {
        throw not_running(session, "observe");
    }
    auto& runtime = *run->execution.runtime;

    RunObservation observation;
    for (const auto& event : runtime.pollEvents(std::numeric_limits<std::size_t>::max())) {
        if (event.isError) {
            observation.last_error = event.text;
        }
    }
    if (!runtime.waitFor(std::chrono::nanoseconds{0})) {
        return observation;
    }

    observation.ended = true;
    if (const auto result = runtime.result(); !result.has_value()) {
        observation.run_error = result.error().message;
    }
    auto& resources = *run->resources;
    // the graph and its blocks go first, then the ports their stream bindings held
    observation.ended_run = std::shared_ptr<Execution>(
        resources.execution.release(),
        [this, bindings = std::exchange(resources.stream_bindings, {})](Execution* ended) {
            delete ended;
            for (const auto& binding : bindings) {
                stream_allocator_.release(binding.internal);
            }
        });
    resources.lifecycle_phase = LifecyclePhase::Idle;
    resources.teardown_complete = true;
    return observation;
}

std::shared_ptr<Gr4RuntimeManager::SessionRuntimeResources> Gr4RuntimeManager::get_or_create_resources(const domain::Session& session) {
    std::lock_guard lock(resources_mutex_);
    auto& resource = resources_[session.id];
    if (!resource) {
        resource = std::make_shared<SessionRuntimeResources>();
        resource->session_id = session.id;
    }
    return resource;
}

std::shared_ptr<Gr4RuntimeManager::SessionRuntimeResources> Gr4RuntimeManager::find_resources(const std::string& session_id) {
    std::lock_guard lock(resources_mutex_);
    const auto it = resources_.find(session_id);
    return it == resources_.end() ? nullptr : it->second;
}

std::optional<std::string> Gr4RuntimeManager::scheduler_type(const domain::Session& session) {
    auto resources = find_resources(session.id);
    if (!resources) {
        return std::nullopt;
    }
    std::lock_guard resource_lock(resources->mutex);
    const auto* execution = resources->execution.get();
    if (execution == nullptr || !execution->runtime.has_value()) {
        return std::nullopt;
    }
    return std::string(execution->runtime->scheduler().typeName());
}

std::optional<Gr4RuntimeManager::RunningExecution> Gr4RuntimeManager::running_execution(const domain::Session& session) {
    auto resources = find_resources(session.id);
    if (!resources) {
        return std::nullopt;
    }
    std::unique_lock resource_lock(resources->mutex);
    auto* execution = resources->execution.get();
    if (execution == nullptr || !execution->running || resources->lifecycle_phase != LifecyclePhase::Running) {
        return std::nullopt;
    }
    return RunningExecution{.resources = std::move(resources), .lock = std::move(resource_lock), .execution = *execution};
}

// A run that has ended, or whose scheduler has left the running state, takes no settings and serves no stream.
std::optional<Gr4RuntimeManager::RunningExecution> Gr4RuntimeManager::live_execution(const domain::Session& session) {
    auto run = running_execution(session);
    if (!run.has_value()) {
        return std::nullopt;
    }
    using enum gr::Runtime::State;
    const auto& runtime = *run->execution.runtime;
    const auto state = runtime.state();
    if (state == RequestedStop || state == Stopped || state == Error || runtime.waitFor(std::chrono::nanoseconds{0})) {
        return std::nullopt;
    }
    return run;
}

Gr4RuntimeManager::Execution& Gr4RuntimeManager::prepare_locked(const domain::Session& session,
                                                               SessionRuntimeResources& resources) {
    if (resources.lifecycle_phase == LifecyclePhase::Stopping) {
        throw runtime_error(session, "prepare", "session runtime teardown is still in progress");
    }
    if (resources.execution) {
        return *resources.execution;
    }

    if (const auto contract_error = domain::validate_stream_runtime_contract(session); contract_error.has_value()) {
        throw runtime_error(session, "prepare", *contract_error);
    }

    release_stream_bindings_locked(resources);
    std::vector<domain::RuntimeStreamBinding> stream_bindings;
    if (const auto plan = domain::derive_stream_runtime_plan(session); plan.has_value()) {
        stream_bindings.reserve(plan->streams.size());
        for (auto stream_plan : plan->streams) {
            auto internal = stream_plan.transport == "websocket" ? stream_allocator_.allocate_websocket()
                                                                  : stream_allocator_.allocate_http();
            stream_plan.ready = true;
            stream_debug(std::format("session={} stream_id={} transport={} injected_endpoint={} browser_path={}",
                                     session.id,
                                     stream_plan.stream_id,
                                     stream_plan.transport,
                                     internal.endpoint,
                                     stream_plan.path));
            stream_bindings.push_back(domain::RuntimeStreamBinding{
                .plan = stream_plan,
                .internal = internal,
                .browser = domain::to_browser_stream_descriptor(stream_plan),
            });
        }
    }

    auto execution = std::make_unique<Execution>();
    try {
        std::map<std::string, gr::property_map, std::less<>> overrides;
        for (const auto& binding : stream_bindings) {
            overrides[binding.plan.block_instance_name] = gr::property_map{
                {"endpoint", binding.internal.endpoint},
                {"transport", binding.plan.transport},
            };
        }
        auto graph = gr::RuntimeGraph::fromYaml(loader_, normalize_graph_content(session.grc_content), overrides);
        if (!graph.has_value()) {
            throw std::runtime_error(graph.error().message);
        }
        for (auto& block : graph->blocks(gr::RuntimeGraph::Recursive::Yes)) {
            execution->blocks.emplace(std::string(block.uniqueName()), block);
        }

        const auto scheduler = session.scheduler_alias.has_value() ? core_scheduler_type(*session.scheduler_alias)
                                                                   : default_scheduler_type(*loader_);
        auto runtime = gr::Runtime::create(std::move(*graph), scheduler);
        if (!runtime.has_value()) {
            throw std::runtime_error(runtime.error().message);
        }
        execution->runtime.emplace(std::move(*runtime));
    } catch (const std::exception& error) {
        for (const auto& binding : stream_bindings) {
            stream_allocator_.release(binding.internal);
        }
        throw runtime_error(session, "prepare", error.what());
    }

    resources.execution = std::move(execution);
    resources.stream_bindings = std::move(stream_bindings);
    resources.generation += 1;
    resources.lifecycle_phase = LifecyclePhase::Idle;
    resources.teardown_complete = true;
    resources.teardown_error.reset();
    return *resources.execution;
}

void Gr4RuntimeManager::release_stream_bindings_locked(SessionRuntimeResources& resources) {
    for (const auto& binding : resources.stream_bindings) {
        stream_allocator_.release(binding.internal);
    }
    resources.stream_bindings.clear();
}

std::optional<std::string> Gr4RuntimeManager::stop_locked(const domain::Session& session,
                                                          SessionRuntimeResources& resources,
                                                          std::unique_lock<std::mutex>& lock) {
    (void)lock;

    auto* execution = resources.execution.get();
    if (execution == nullptr) {
        release_stream_bindings_locked(resources);
        resources.lifecycle_phase = LifecyclePhase::Idle;
        resources.teardown_complete = true;
        resources.teardown_error.reset();
        return std::nullopt;
    }

    if (resources.lifecycle_phase == LifecyclePhase::Stopping) {
        throw runtime_error(session, "stop", "session runtime stop already in progress");
    }

    resources.lifecycle_phase = LifecyclePhase::Stopping;
    resources.teardown_complete = false;
    resources.teardown_error.reset();

    // A run that ended by itself, or whose scheduler entered its error state, stops with success, and its result goes
    // back to the caller.
    const bool ended_by_itself = execution->runtime->waitFor(std::chrono::nanoseconds{0}) ||
                                 execution->runtime->state() == gr::Runtime::State::Error;
    if (execution->running) {
        execution->runtime->stop();
    }
    const auto result = execution->runtime->result();

    execution->running = false;
    release_stream_bindings_locked(resources);
    resources.execution.reset();
    resources.teardown_complete = true;
    if (!result.has_value() && !ended_by_itself) {
        resources.lifecycle_phase = LifecyclePhase::Failed;
        resources.teardown_error = result.error().message;
        throw runtime_error(session, "execution", *resources.teardown_error);
    }
    resources.lifecycle_phase = LifecyclePhase::Idle;
    resources.teardown_error.reset();
    return result.has_value() ? std::nullopt : std::optional<std::string>(result.error().message);
}

}  // namespace gr4cp::runtime
