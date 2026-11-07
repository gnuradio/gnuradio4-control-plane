#include "gr4cp/runtime/gr4_runtime_manager.hpp"
#include "final_settings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <complex>
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
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Runtime.hpp>
#include <gnuradio-4.0/SchedulerModel.hpp>
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

// The scheduler of a session that names none: the incubator's BlockingBackoff when a plugin offers it, else core's
// multi-threaded Simple scheduler, which gr::Runtime::create() registers itself.
std::string default_scheduler_type(const gr::PluginLoader& loader) {
    constexpr std::string_view kPreferred = "gr::incubator::scheduler::BlockingBackoff";
    if (const auto available = loader.availableSchedulers(); std::ranges::binary_search(available, kPreferred)) {
        return std::string(kPreferred);
    }
    return core_scheduler_type("gr::scheduler::SimpleMulti");
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

using detail::CapturedBlock;

template <typename Block>
std::optional<std::string> settings_name(const Block& block) {
    const auto value = block.get(std::string("name"));
    return value.has_value() ? value_as_string(*value) : std::nullopt;
}

template <typename Block>
std::string describe_runtime_block(const Block& block) {
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

template <typename Block>
BlockNotFoundError make_block_not_found_error(const std::map<std::string, Block, std::less<>>& blocks,
                                              std::string_view block_unique_name) {
    std::string available;
    for (const auto& [_, block] : blocks) {
        available += available.empty() ? describe_runtime_block(block) : ", " + describe_runtime_block(block);
    }
    return BlockNotFoundError(std::format("block not found in running session: {}; available runtime blocks: {}",
                                          block_unique_name,
                                          available.empty() ? "<none>" : available));
}

// Returns the block `requested_block_target` names: the block with that name, else the one with that unique name, else
// the one with that `name` setting, or null. Throws BlockNotFoundError with the candidates when two blocks carry the
// name or the `name` setting.
template <typename Block>
Block* resolve_runtime_block(std::map<std::string, Block, std::less<>>& blocks,
                                       std::string_view requested_block_target) {
    const auto only_match = [&blocks, requested_block_target](auto carries) -> Block* {
        std::vector<Block*> matches;
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

    if (auto* block = only_match([requested_block_target](const Block& block) {
            return block.name() == requested_block_target;
        })) {
        return block;
    }
    if (const auto it = blocks.find(requested_block_target); it != blocks.end()) {
        return &it->second;
    }
    return only_match([requested_block_target](const Block& block) {
        return settings_name(block) == requested_block_target;
    });
}

// Converts a double-precision complex patch value to single precision when the block holds a single-precision complex
// value, and each such element of a list when the block holds a list of them. The framework converts neither complex
// precision to the other.
gr::pmt::Value with_held_complex_precision(const gr::pmt::Value& value, const std::optional<gr::pmt::Value>& held) {
    if (!held.has_value()) {
        return value;
    }
    const auto narrowed = [](const gr::pmt::Value& item) -> gr::pmt::Value {
        if (const auto* wide = item.get_if<std::complex<double>>(); wide != nullptr) {
            return std::complex<float>(static_cast<float>(wide->real()), static_cast<float>(wide->imag()));
        }
        return item;
    };
    if (held->get_if<std::complex<float>>() != nullptr) {
        return narrowed(value);
    }
    const auto* elements = value.get_if<gr::Tensor<gr::pmt::Value>>();
    if (held->get_if<gr::Tensor<std::complex<float>>>() == nullptr || elements == nullptr) {
        return value;
    }
    gr::Tensor<gr::pmt::Value> result;
    result.reserve(elements->size());
    for (const auto& element : *elements) {
        result.push_back(narrowed(element));
    }
    return result;
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

// Moves the parameter `name` of a block entry under the entry's meta_information. The graph loader refuses a parameter
// the block does not declare and restores the entry's meta_information onto the block.
bool move_parameter_to_meta_information(gr::property_map& block, gr::property_map& parameters, std::string_view name) {
    const std::pmr::string key(name);
    const auto it = parameters.find(key);
    if (it == parameters.end()) {
        return false;
    }

    gr::property_map meta_information;
    if (const auto meta_it = block.find("meta_information"); meta_it != block.end()) {
        if (const auto* existing = meta_it->second.get_if<gr::property_map>(); existing != nullptr) {
            meta_information = *existing;
        }
    }
    meta_information.insert_or_assign(key, it->second);
    parameters.erase(it);
    block["meta_information"] = meta_information;
    return true;
}

// Moves a block's nested `stream` map under its meta_information. The control plane reads the map from the session's
// graph text, and no block declares it.
void move_stream_intent_to_meta_information(gr::property_map& block) {
    const auto parameters_it = block.find("parameters");
    if (parameters_it == block.end()) {
        return;
    }
    const auto* existing = parameters_it->second.get_if<gr::property_map>();
    if (existing == nullptr) {
        return;
    }

    auto parameters = *existing;
    if (move_parameter_to_meta_information(block, parameters, "stream")) {
        block["parameters"] = parameters;
    }
}

// Moves the flat `stream_id` and `payload_format` keys of each block bound to a managed stream under its
// meta_information. The binding's endpoint and transport reach the block as load overrides. Returns whether a key
// moved.
bool move_bound_stream_keys_to_meta_information(gr::property_map& root,
                                                const std::vector<domain::RuntimeStreamBinding>& bindings) {
    const auto it = root.find("blocks");
    if (bindings.empty() || it == root.end()) {
        return false;
    }
    auto* blocks = it->second.get_if<gr::Tensor<gr::pmt::Value>>();
    if (blocks == nullptr) {
        return false;
    }

    bool changed = false;

    for (auto& block_value : *blocks) {
        auto* block = block_value.get_if<gr::property_map>();
        if (block == nullptr) {
            continue;
        }

        gr::property_map parameters;
        if (const auto parameters_it = block->find("parameters"); parameters_it != block->end()) {
            if (const auto* existing = parameters_it->second.get_if<gr::property_map>(); existing != nullptr) {
                parameters = *existing;
            }
        }

        std::optional<std::string> name;
        if (const auto name_it = parameters.find("name"); name_it != parameters.end()) {
            name = value_as_string(name_it->second);
        }
        if (!name.has_value() || name->empty() ||
            std::ranges::none_of(bindings, [&name](const auto& binding) { return binding.plan.block_instance_name == *name; })) {
            continue;
        }

        const bool moved_id = move_parameter_to_meta_information(*block, parameters, "stream_id");
        const bool moved_format = move_parameter_to_meta_information(*block, parameters, "payload_format");
        if (moved_id || moved_format) {
            (*block)["parameters"] = parameters;
            changed = true;
        }
    }
    return changed;
}

// Returns the graph document the runtime loads: each block entry in the shape the graph loader reads, the stream keys
// of `bindings` and every nested `stream` map moved under the block's meta_information, and each connection's ports as
// indices or names.
std::string normalize_graph_content(std::string_view content, const std::vector<domain::RuntimeStreamBinding>& bindings) {
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
                    move_stream_intent_to_meta_information(*block);
                    changed = changed || (*block != before);
                }
            }
        }
    }

    changed = move_bound_stream_keys_to_meta_information(root, bindings) || changed;

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
        auto& registry = gr::globalSchedulerRegistry();
        ((std::ignore = registry.insert(kSchedulerAliases[Index].alias, "", &make_aliased_scheduler<Index>)), ...);
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

// On glibc, returns every malloc arena's free pages to the system when it is destroyed. glibc serves each new thread
// from an arena of its own, up to eight per core, and keeps freed pages resident in their arena. Without the release,
// each thread that built a graph keeps that graph's pages after the graph is destroyed.
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
    HeapRelease heap_release;  // the first member, destroyed after the runtime and the blocks
    std::optional<gr::Runtime> runtime;
    std::map<std::string, gr::BlockHandle, std::less<>> blocks;  // every block of the graph, by unique name
    bool running{false};

    auto capture_settings() const { return detail::capture_settings(blocks); }

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
    // The last run's final values, read until the next prepare or destroy. No block handles survive teardown.
    std::optional<std::map<std::string, CapturedBlock, std::less<>>> ended_settings;
    std::vector<domain::RuntimeStreamBinding> stream_bindings;
    NativeGraphFactory native_graph_factory;
    NativeExecutionHook native_execution_hook;
    std::optional<std::string> async_error;
    std::mutex mutex;
};

Gr4RuntimeManager::Gr4RuntimeManager(std::shared_ptr<gr::PluginLoader> loader) : loader_(std::move(loader)) {
    register_scheduler_aliases();
}

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

void Gr4RuntimeManager::set_native_graph_factory(const domain::Session &session, NativeGraphFactory factory,
                                                 NativeExecutionHook hook) {
    auto resources = get_or_create_resources(session);
    std::lock_guard resource_lock(resources->mutex);
    if (resources->execution || resources->lifecycle_phase == LifecyclePhase::Stopping) {
        throw runtime_error(session, "register native graph",
                            "execution already prepared; stop it before changing its factory");
    }
    resources->native_graph_factory = std::move(factory);
    resources->native_execution_hook = std::move(hook);
}

Gr4RuntimeManager::ExecutionStatus Gr4RuntimeManager::execution_status(const domain::Session &session) {
    auto resources = find_resources(session.id);
    if (!resources)
        return {0, "unprepared"};
    std::lock_guard lock(resources->mutex);
    auto *execution = resources->execution.get();
    if (execution && execution->runtime->state() == gr::Runtime::State::Error) {
        resources->lifecycle_phase = LifecyclePhase::Failed;
        resources->async_error = "native execution entered ERROR";
    }
    return {resources->generation,
            (resources->async_error || resources->lifecycle_phase == LifecyclePhase::Failed) ? "failed"
            : execution && execution->running
                ? (execution->runtime->state() == gr::Runtime::State::Paused ? "paused" : "running")
                : "stopped"};
}

void Gr4RuntimeManager::with_execution(const domain::Session &session, std::uint64_t generation,
                                       const std::function<void(gr::SchedulerModel &)> &operation) {
    with_execution(session, generation, operation, false);
}

void Gr4RuntimeManager::with_execution(const domain::Session &session, std::uint64_t generation,
                                       const std::function<void(gr::SchedulerModel &)> &operation, bool allow_paused) {
    auto resources = find_resources(session.id);
    if (!resources)
        throw runtime_error(session, "native operation", "session execution absent");
    std::lock_guard lock(resources->mutex);
    auto *execution = resources->execution.get();
    if (generation != resources->generation)
        throw runtime_error(session, "native operation", "stale execution generation");
    if (!execution || !execution->running || resources->lifecycle_phase != LifecyclePhase::Running ||
        (execution->runtime->state() != gr::Runtime::State::Running &&
         !(allow_paused && execution->runtime->state() == gr::Runtime::State::Paused)))
        throw runtime_error(session, "native operation", "execution is not running");
    auto record_failure = [&] {
        if (execution->runtime->state() == gr::Runtime::State::Error) {
            resources->lifecycle_phase = LifecyclePhase::Failed;
            resources->async_error = "native execution entered ERROR";
        }
    };
    auto refresh_blocks = [&] {
        std::map<std::string, gr::BlockHandle, std::less<>> current;
        for (auto &block : execution->runtime->blocks())
            current.emplace(std::string(block.uniqueName()), block);
        execution->blocks.swap(current);
    };
    try {
        execution->runtime->withScheduler(operation);
    } catch (...) {
        refresh_blocks();
        record_failure();
        throw;
    }
    refresh_blocks();
    record_failure();
    if (resources->async_error)
        throw runtime_error(session, "native operation", *resources->async_error);
}

void Gr4RuntimeManager::pause(const domain::Session &session) { change_pause(session, std::nullopt, true); }
void Gr4RuntimeManager::resume(const domain::Session &session) { change_pause(session, std::nullopt, false); }
void Gr4RuntimeManager::pause(const domain::Session &session, std::uint64_t generation) {
    change_pause(session, generation, true);
}
void Gr4RuntimeManager::resume(const domain::Session &session, std::uint64_t generation) {
    change_pause(session, generation, false);
}

void Gr4RuntimeManager::change_pause(const domain::Session &session, std::optional<std::uint64_t> generation,
                                     bool paused) {
    auto resources = find_resources(session.id);
    if (!resources)
        throw runtime_error(session, "pause/resume", "execution absent");
    std::lock_guard lock(resources->mutex);
    if (generation && *generation != resources->generation)
        throw runtime_error(session, "pause/resume", "stale execution generation");
    auto *execution = resources->execution.get();
    if (!execution || !execution->running || resources->lifecycle_phase != LifecyclePhase::Running)
        throw runtime_error(session, "pause/resume", "execution is not active");
    execution->runtime->withScheduler([&](gr::SchedulerModel &scheduler) {
        auto &block = *scheduler.asBlockModel();
        const auto target = paused ? gr::lifecycle::State::PAUSED : gr::lifecycle::State::RUNNING;
        if (block.state() == target)
            return;
        const auto expected = paused ? gr::lifecycle::State::RUNNING : gr::lifecycle::State::PAUSED;
        if (block.state() != expected)
            throw runtime_error(session, "pause/resume", "execution is not active");
        const auto request = paused ? gr::lifecycle::State::REQUESTED_PAUSE : gr::lifecycle::State::RUNNING;
        if (auto changed = block.changeStateTo(request); !changed)
            throw runtime_error(session, "pause/resume", changed.error().message);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (block.state() != target) {
            if (block.state() == gr::lifecycle::State::ERROR || std::chrono::steady_clock::now() >= deadline)
                throw runtime_error(session, "pause/resume", "execution did not settle in requested state");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
}

void Gr4RuntimeManager::restart(const domain::Session &session) {
    stop(session);
    // prepare replaces the ended execution but retains the native factory and its provider.
    prepare(session);
    start(session);
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
    if (const auto started = execution.runtime->start(); !started.has_value()) {
        resources->lifecycle_phase = LifecyclePhase::Idle;
        resources->teardown_complete = true;
        throw runtime_error(session, "start", started.error().message);
    }
    execution.running = true;
    resources->lifecycle_phase = LifecyclePhase::Running;
    try {
        if (resources->native_graph_factory)
            execution.runtime->withScheduler([&](gr::SchedulerModel &scheduler) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (!scheduler.workerStarted()) {
                    if (execution.runtime->waitFor(std::chrono::nanoseconds{0}) ||
                        std::chrono::steady_clock::now() >= deadline)
                        throw runtime_error(session, "native initialization", "worker did not start");
                    std::this_thread::yield();
                }
                if (resources->native_execution_hook)
                    resources->native_execution_hook(scheduler, resources->generation, true);
            });
    } catch (...) {
        execution.runtime->stop();
        resources->lifecycle_phase = LifecyclePhase::Failed;
        throw;
    }
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
        resources->ended_settings.reset();
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
    const auto accepted = block->writableMembers();
    std::string foreign;
    for (const auto& [key, _] : patch) {
        if (!accepted.contains(std::string(key))) {
            foreign += foreign.empty() ? std::string(key) : ", " + std::string(key);
        }
    }
    if (!foreign.empty()) {
        throw SettingsRefusedError(std::format("block {} has no setting named {}", block_target, foreign));
    }
    gr::property_map converted;
    for (const auto& [key, value] : patch) {
        converted.insert_or_assign(key, with_held_complex_precision(value, block->get(std::string(key))));
    }
    if (const auto staged = block->setStaged(converted); !staged.has_value()) {
        throw SettingsRefusedError(std::format("block {} refused the settings: {}", block_target, staged.error().message));
    }
}

gr::property_map Gr4RuntimeManager::get_block_settings(const domain::Session& session, const std::string& block_target) {
    auto resources = find_resources(session.id);
    if (!resources) {
        throw not_running(session, "settings read");
    }
    std::lock_guard resource_lock(resources->mutex);

    // the run in progress or a run that has ended unread, else the session's last ended run; a stopping run has neither
    Execution* source = nullptr;
    if (auto* execution = resources->execution.get();
        execution != nullptr && execution->running && resources->lifecycle_phase == LifecyclePhase::Running) {
        using enum gr::Runtime::State;
        const auto& runtime = *execution->runtime;
        const auto state = runtime.state();
        const bool stopping = state == RequestedStop || state == Stopped || state == Error;
        source = !stopping || runtime.waitFor(std::chrono::nanoseconds{0}) ? execution : nullptr;
    }
    if (source == nullptr) {
        if (resources->execution == nullptr && resources->ended_settings.has_value()) {
            auto& captured = *resources->ended_settings;
            auto* block = resolve_runtime_block(captured, block_target);
            if (block == nullptr) {
                throw make_block_not_found_error(captured, block_target);
            }
            return block->get();
        }
        throw not_running(session, "settings read");
    }

    auto* block = resolve_runtime_block(source->blocks, block_target);
    if (block == nullptr) {
        throw make_block_not_found_error(source->blocks, block_target);
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
    if (resources.native_execution_hook) {
        try {
            runtime.withScheduler([&](gr::SchedulerModel &scheduler) {
                resources.native_execution_hook(scheduler, resources.generation, false);
            });
        } catch (const std::exception &error) {
            observation.run_error = error.what();
        }
    }
    if (!observation.run_error)
        observation.run_error = resources.async_error;
    resources.async_error = observation.run_error;
    resources.ended_settings = run->execution.capture_settings();
    // the graph goes first, then the ports its stream bindings held
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

// Returns the session's run in progress, or nothing when it has none, the run has ended, or its scheduler is stopping,
// stopped or in error.
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
    std::uint64_t generation;
    {
        std::lock_guard lock(resources_mutex_);
        if (next_generation_ == UINT64_MAX)
            throw runtime_error(session, "prepare", "generation exhausted");
        generation = ++next_generation_;
    }
    try {
        std::map<std::string, gr::property_map, std::less<>> overrides;
        for (const auto& binding : stream_bindings) {
            overrides[binding.plan.block_instance_name] = gr::property_map{
                {"endpoint", binding.internal.endpoint},
                {"transport", binding.plan.transport},
            };
        }
        auto graph = resources.native_graph_factory
                         ? std::expected<gr::RuntimeGraph, gr::RuntimeError>(
                               gr::RuntimeGraph::adopt(resources.native_graph_factory(session, generation), loader_))
                         : gr::RuntimeGraph::fromYaml(
                               loader_, normalize_graph_content(session.grc_content, stream_bindings), overrides);
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
    resources.ended_settings.reset();
    resources.stream_bindings = std::move(stream_bindings);
    resources.generation = generation;
    resources.async_error.reset();
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
        // Native controllers can finalize the accepted downstream tail. Keep the
        // ordinary serialized-graph Stop behavior supplied by the base unchanged.
        if (resources.native_graph_factory && !ended_by_itself &&
            execution->runtime->state() != gr::Runtime::State::Paused) {
            execution->runtime->withScheduler([&](gr::SchedulerModel &scheduler) {
                if (!scheduler.stopAfterDrain(std::chrono::seconds(5)))
                    resources.async_error = "native downstream drain failed or exceeded its deadline";
            });
        }
        execution->runtime->stop();
    }
    const auto result = execution->runtime->result();

    if (execution->running) {
        if (resources.native_execution_hook) {
            try {
                execution->runtime->withScheduler([&](gr::SchedulerModel &scheduler) {
                    resources.native_execution_hook(scheduler, resources.generation, false);
                });
            } catch (const std::exception &error) {
                resources.async_error = error.what();
            }
        }
        resources.ended_settings = execution->capture_settings();
    }
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
    if (resources.async_error)
        return resources.async_error;
    return result.has_value() ? std::nullopt : std::optional<std::string>(result.error().message);
}

}  // namespace gr4cp::runtime
