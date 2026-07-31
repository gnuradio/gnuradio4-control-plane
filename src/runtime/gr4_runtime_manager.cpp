#include "gr4cp/runtime/gr4_runtime_manager.hpp"

#include <chrono>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include <gnuradio-4.0/Graph_yaml_importer.hpp>
#include <gnuradio-4.0/Graph.hpp>
#include <gnuradio-4.0/Message.hpp>
#include <gnuradio-4.0/Port.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>
#include <gnuradio-4.0/YamlPmt.hpp>
#include <httplib.h>

namespace gr4cp::runtime {

namespace {

template<typename TResult>
bool connection_succeeded(const TResult& result) {
    if constexpr (requires { result.has_value(); }) {
        return result.has_value();
    } else if constexpr (std::is_enum_v<std::remove_cvref_t<TResult>>) {
        using raw_t = std::underlying_type_t<std::remove_cvref_t<TResult>>;
        return static_cast<raw_t>(result) == 0;
    } else {
        return static_cast<bool>(result);
    }
}

template<typename TResult>
std::string connection_error_message(const TResult& result) {
    if constexpr (requires { result.error().message; }) {
        return result.error().message;
    } else {
        return "failed to connect ports";
    }
}

std::vector<std::filesystem::path> split_plugin_directories(std::string_view paths) {
    std::vector<std::filesystem::path> directories;
    std::size_t start = 0;
    while (start <= paths.size()) {
        const auto end = paths.find(':', start);
        const auto token = paths.substr(start, end == std::string_view::npos ? paths.size() - start : end - start);
        if (!token.empty()) {
            directories.emplace_back(token);
        }
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    return directories;
}

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

void prepend_library_search_path(const std::vector<std::filesystem::path>& plugin_directories) {
#if !defined(_WIN32)
    std::string value;
    for (const auto& directory : plugin_directories) {
        if (!value.empty()) {
            value += ":";
        }
        value += directory.string();
    }
    if (const char* existing = std::getenv("LD_LIBRARY_PATH"); existing != nullptr && *existing != '\0') {
        if (!value.empty()) {
            value += ":";
        }
        value += existing;
    }
    if (!value.empty()) {
        ::setenv("LD_LIBRARY_PATH", value.c_str(), 1);
    }
#else
    (void)plugin_directories;
#endif
}

void configure_plugin_environment(const std::vector<std::filesystem::path>& plugin_directories) {
    std::string value;
    for (const auto& directory : plugin_directories) {
        if (!value.empty()) {
            value += ":";
        }
        value += directory.string();
    }
    if (!value.empty()) {
        ::setenv("GNURADIO4_PLUGIN_DIRECTORIES", value.c_str(), 1);
    }
    prepend_library_search_path(plugin_directories);
}

bool is_shared_library(const std::filesystem::path& path) {
#if defined(__APPLE__)
    constexpr auto extension = ".dylib";
#elif defined(_WIN32)
    constexpr auto extension = ".dll";
#else
    constexpr auto extension = ".so";
#endif
    return path.extension() == extension;
}

bool is_shared_block_library(const std::filesystem::path& path) {
    return is_shared_library(path) && path.filename().string().find("Shared") != std::string::npos;
}

void preload_support_libraries(const std::vector<std::filesystem::path>& plugin_directories) {
#if defined(__unix__) || defined(__APPLE__)
    static std::vector<void*> handles;
    static std::set<std::string> loaded_libraries;

    auto should_preload = [](const std::filesystem::path& path) {
        const auto name = path.filename().string();
        return name == "libgnuradio-blocklib-core.so" || name == "libgnuradio-plugin.so";
    };

    for (const auto& directory : plugin_directories) {
        if (!std::filesystem::is_directory(directory)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file() || !should_preload(entry.path())) {
                continue;
            }
            const auto library = entry.path().string();
            if (!loaded_libraries.insert(library).second) {
                continue;
            }
            void* handle = ::dlopen(library.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (handle == nullptr) {
                loaded_libraries.erase(library);
                const char* error = ::dlerror();
                throw std::runtime_error("failed to preload GNU Radio 4 support library " + library +
                                         (error != nullptr ? ": " + std::string(error) : ""));
            }
            handles.push_back(handle);
        }
    }
#else
    (void)plugin_directories;
#endif
}

std::string module_name_from_library(const std::filesystem::path& path) {
    std::string name = path.filename().string();
    if (name.rfind("lib", 0) == 0) {
        name = name.substr(3);
    }
    if (name.ends_with(".so")) {
        name.resize(name.size() - 3);
    } else if (name.ends_with(".dylib")) {
        name.resize(name.size() - 6);
    }
    if (name.ends_with("Shared")) {
        name.resize(name.size() - 6);
    }
    return name;
}

void bootstrap_shared_block_libraries(const std::vector<std::filesystem::path>& plugin_directories) {
#if defined(__unix__) || defined(__APPLE__)
    static std::vector<void*> handles;
    static std::set<std::string> retained;

    using init_fn_t = std::size_t (*)(gr::BlockRegistry&);

    for (const auto& directory : plugin_directories) {
        if (!std::filesystem::is_directory(directory)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file() || !is_shared_block_library(entry.path())) {
                continue;
            }

            const auto library_string = entry.path().string();
            if (!retained.insert(library_string).second) {
                continue;
            }

            void* handle = ::dlopen(library_string.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (handle == nullptr) {
                retained.erase(library_string);
                const char* error = ::dlerror();
                throw std::runtime_error("failed to load GNU Radio 4 shared block library " + library_string +
                                         (error != nullptr ? ": " + std::string(error) : ""));
            }

            const auto symbol_name = std::string("gr_blocklib_init_module_") + module_name_from_library(entry.path());
            auto* init_fn = reinterpret_cast<init_fn_t>(::dlsym(handle, symbol_name.c_str()));
            if (init_fn == nullptr) {
                handles.push_back(handle);
                continue;
            }

            try {
                (void)init_fn(gr::globalBlockRegistry());
            } catch (const std::exception& error) {
                throw std::runtime_error("failed to initialize GNU Radio 4 shared block library " + library_string +
                                         ": " + error.what());
            }
            handles.push_back(handle);
        }
    }
#else
    (void)plugin_directories;
#endif
}

void ensure_runtime_environment(const std::vector<std::filesystem::path>& plugin_directories) {
    static std::once_flag once;
    static std::optional<std::string> bootstrap_error;

    std::call_once(once, [&]() {
        try {
            if (plugin_directories.empty()) {
                throw std::runtime_error("no GNU Radio 4 plugin directories are configured");
            }
            configure_plugin_environment(plugin_directories);
            preload_support_libraries(plugin_directories);
            bootstrap_shared_block_libraries(plugin_directories);
            auto& loader = gr::globalPluginLoader();
            (void)loader.availableBlocks();
        } catch (const std::exception& error) {
            bootstrap_error = error.what();
        }
    });

    if (bootstrap_error.has_value()) {
        throw std::runtime_error("GNU Radio 4 runtime initialization failed: " + *bootstrap_error);
    }
}

std::runtime_error runtime_error(const domain::Session& session, std::string_view phase, const std::string& detail) {
    return std::runtime_error(std::format("session {} {} failed: {}", session.id, phase, detail));
}

void register_default_schedulers() {
    auto& registry = gr::globalSchedulerRegistry();
    (void)registry.insert<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>>("=gr::scheduler::SimpleSingle");
    (void)registry.insert<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>>("=gr::scheduler::SimpleMulti");
}

std::string default_scheduler_alias() {
    register_default_schedulers();

    auto available = gr::globalPluginLoader().availableSchedulers();
    std::ranges::sort(available);

    for (const auto preferred : {
             std::string_view{"gr::incubator::scheduler::BlockingBackoff"},
             std::string_view{"gr::scheduler::SimpleMulti"},
             std::string_view{"gr::scheduler::SimpleSingle"},
         }) {
        if (std::ranges::binary_search(available, preferred)) {
            return std::string(preferred);
        }
    }

    if (!available.empty()) {
        return available.front();
    }
    return "gr::scheduler::SimpleMulti";
}

std::string next_request_id() {
    thread_local std::mt19937_64 generator(std::random_device{}());
    std::uniform_int_distribution<unsigned long long> distribution;
    return std::format("req_{:016x}", distribution(generator));
}

std::optional<std::string> value_to_string(const gr::pmt::Value& value) {
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

std::string describe_runtime_block(const std::shared_ptr<gr::BlockModel>& block) {
    std::string description(block->uniqueName());

    if (const auto name = std::string(block->name()); !name.empty() && name != description) {
        description += std::format(" (name={})", name);
    }

    if (const auto type_name = std::string(block->typeName()); !type_name.empty()) {
        description += std::format(" [type={}]", type_name);
    }

    if (const auto& settings = block->settings().get(); settings.contains("name")) {
        if (const auto settings_name = value_to_string(settings.at("name")); settings_name.has_value() && !settings_name->empty() &&
                                                                           *settings_name != std::string(block->uniqueName()) &&
                                                                           *settings_name != std::string(block->name())) {
            description += std::format(" [settings.name={}]", *settings_name);
        }
    }

    return description;
}

std::string summarize_runtime_blocks(const gr::BlockModel& root) {
    if (root.blocks().empty()) {
        return "<none>";
    }

    std::string result;
    bool first = true;
    for (const auto& block : root.blocks()) {
        if (!first) {
            result += ", ";
        }
        result += describe_runtime_block(block);
        first = false;
    }
    return result;
}

BlockNotFoundError make_block_not_found_error(const gr::BlockModel& root, std::string_view block_unique_name) {
    return BlockNotFoundError(std::format("block not found in running session: {}; available runtime blocks: {}",
                                          block_unique_name,
                                          summarize_runtime_blocks(root)));
}

std::shared_ptr<gr::BlockModel> resolve_runtime_block(const gr::BlockModel& root, std::string_view requested_block_target) {
    for (const auto& block : root.blocks()) {
        if (block->name() == requested_block_target) {
            return block;
        }
    }

    for (const auto& block : root.blocks()) {
        if (block->uniqueName() == requested_block_target) {
            return block;
        }
    }

    for (const auto& block : root.blocks()) {
        const auto& settings = block->settings().get();
        if (!settings.contains("name")) {
            continue;
        }
        const auto settings_name = value_to_string(settings.at("name"));
        if (settings_name.has_value() && *settings_name == requested_block_target) {
            return block;
        }
    }

    return nullptr;
}

void normalize_block(gr::property_map& block) {
    std::optional<std::string> studio_node_id;
    if (const auto it = block.find("id"); it != block.end()) {
        studio_node_id = value_to_string(it->second);
    }

    if (const auto it = block.find("block"); it != block.end()) {
        if (const auto block_type = value_to_string(it->second); block_type.has_value() && !block_type->empty()) {
            block["id"] = *block_type;
        }
    }

    if (!block.contains("id")) {
        if (const auto it = block.find("block_type"); it != block.end()) {
            if (const auto block_type = value_to_string(it->second); block_type.has_value() && !block_type->empty()) {
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
            if (const auto name = value_to_string(it->second); name.has_value() && !name->empty()) {
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
                if (const auto token = value_to_string((*sequence)[index]); token.has_value()) {
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
        return value_to_string(it->second);
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

bool inject_stream_bindings(gr::property_map& root, const std::vector<domain::RuntimeStreamBinding>& bindings) {
    if (bindings.empty()) {
        return false;
    }

    auto it = root.find("blocks");
    if (it == root.end()) {
        return false;
    }
    auto* blocks = it->second.get_if<gr::Tensor<gr::pmt::Value>>();
    if (blocks == nullptr) {
        return false;
    }

    bool changed = false;
    std::set<std::string> injected_stream_ids;
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
            name = value_to_string(name_it->second);
        }
        if (!name.has_value() || name->empty()) {
            if (const auto instance_it = block->find("instance_name"); instance_it != block->end()) {
                name = value_to_string(instance_it->second);
            }
        }
        if (!name.has_value() || name->empty()) {
            continue;
        }

        for (const auto& binding : bindings) {
            if (binding.plan.block_instance_name != *name) {
                continue;
            }
            parameters["endpoint"] = binding.internal.endpoint;
            (*block)["parameters"] = parameters;
            injected_stream_ids.insert(binding.plan.stream_id);
            changed = true;
            break;
        }
    }

    if (injected_stream_ids.size() != bindings.size()) {
        throw std::runtime_error("failed to inject all managed stream bindings into runtime graph");
    }

    return changed;
}

std::string normalize_graph_content(std::string_view content, const std::vector<domain::RuntimeStreamBinding>& bindings = {}) {
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

    changed = inject_stream_bindings(root, bindings) || changed;

    return changed ? gr::pmt::yaml::serialize(root) : std::string(content);
}

template <gr::message::Command Command>
gr::Message send_block_settings_request(gr::BlockModel& scheduler_block,
                                        std::string_view block_unique_name,
                                        std::string_view endpoint,
                                        gr::property_map payload,
                                        std::chrono::milliseconds timeout) {
    gr::MsgPortOutBuiltin request_port;
    gr::MsgPortInBuiltin response_port;

    if (auto connection = request_port.connect(*scheduler_block.msgIn); !connection_succeeded(connection)) {
        throw std::runtime_error(std::format("failed to connect runtime request port: {}", connection_error_message(connection)));
    }
    if (auto connection = scheduler_block.msgOut->connect(response_port); !connection_succeeded(connection)) {
        throw std::runtime_error(std::format("failed to connect runtime response port: {}", connection_error_message(connection)));
    }

    const auto request_id = next_request_id();
    gr::sendMessage<Command>(request_port, block_unique_name, endpoint, std::move(payload), request_id);

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto available = response_port.streamReader().available();
        if (available == 0UZ) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        gr::ReaderSpanLike auto replies = response_port.streamReader().get<gr::SpanReleasePolicy::ProcessAll>(available);
        for (const auto& reply : replies) {
            if (reply.clientRequestID != request_id || reply.endpoint != endpoint || reply.serviceName != block_unique_name) {
                continue;
            }
            return reply;
        }
    }

    throw ReplyTimeoutError(std::format("timed out waiting for runtime reply from block {}", block_unique_name));
}

gr::property_map parse_settings_reply(const gr::Message& reply, std::string_view block_unique_name, std::string_view endpoint) {
    if (!reply.data.has_value()) {
        throw std::runtime_error(std::format("runtime returned error for block {} {}: {}", block_unique_name, endpoint, reply.data.error().message));
    }
    return reply.data.value();
}

}  // namespace

struct Gr4RuntimeManager::Execution {
    std::shared_ptr<gr::SchedulerModel> scheduler;
    bool running{false};
};

struct Gr4RuntimeManager::SessionRuntimeResources {
    std::string session_id;
    std::uint64_t generation{0};
    LifecyclePhase lifecycle_phase{LifecyclePhase::Idle};
    bool teardown_complete{true};
    std::optional<std::string> teardown_error;
    std::optional<std::string> async_error;
    std::unique_ptr<Execution> execution;
    std::vector<domain::RuntimeStreamBinding> stream_bindings;
    std::mutex mutex;
};

Gr4RuntimeManager::Gr4RuntimeManager(std::vector<std::filesystem::path> plugin_directories)
    : plugin_directories_(plugin_directories.empty() ? default_plugin_directories() : std::move(plugin_directories)) {}

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
            stop_locked(session, *resource, resource_lock);
        } catch (...) {
        }

    }
}

std::vector<std::filesystem::path> Gr4RuntimeManager::default_plugin_directories() {
    if (const char* env = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES"); env != nullptr && *env != '\0') {
        return split_plugin_directories(env);
    }
#if defined(GR4CP_DEFAULT_GNURADIO4_PLUGIN_DIR)
    return split_plugin_directories(GR4CP_DEFAULT_GNURADIO4_PLUGIN_DIR);
#else
    return {};
#endif
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

    resources->async_error.reset();
    resources->teardown_error.reset();
    resources->teardown_complete = false;
    resources->lifecycle_phase = LifecyclePhase::Starting;
    execution.scheduler->start();
    execution.running = true;
    resources->lifecycle_phase = LifecyclePhase::Running;
}

void Gr4RuntimeManager::stop(const domain::Session& session) {
    auto resources = find_resources(session.id);
    if (!resources) {
        return;
    }

    std::unique_lock resource_lock(resources->mutex);
    stop_locked(session, *resources, resource_lock);
}

void Gr4RuntimeManager::destroy(const domain::Session& session) {
    auto resources = find_resources(session.id);
    if (!resources) {
        return;
    }

    {
        std::unique_lock resource_lock(resources->mutex);
        stop_locked(session, *resources, resource_lock);
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
    auto resources = find_resources(session.id);
    if (!resources) {
        return std::nullopt;
    }

    std::lock_guard resource_lock(resources->mutex);
    const auto* execution = resources->execution.get();
    if (execution == nullptr || !execution->running || resources->lifecycle_phase != LifecyclePhase::Running ||
        resources->stream_bindings.empty()) {
        return std::nullopt;
    }

    domain::StreamRuntimePlan plan;
    for (const auto& binding : resources->stream_bindings) {
        plan.streams.push_back(binding.plan);
    }
    return plan;
}

HttpStreamResponse Gr4RuntimeManager::fetch_http_stream(const domain::Session& session, const std::string& stream_id) {
    domain::InternalStreamBinding internal;
    {
        auto resources = find_resources(session.id);
        if (!resources) {
            throw runtime_error(session, "stream fetch", "session runtime is not running");
        }

        std::lock_guard resource_lock(resources->mutex);
        const auto* execution = resources->execution.get();
        if (execution == nullptr || !execution->running || resources->lifecycle_phase != LifecyclePhase::Running) {
            throw runtime_error(session, "stream fetch", "session runtime is not running");
        }

        const auto binding_it = std::ranges::find_if(resources->stream_bindings, [&](const auto& binding) {
            return binding.plan.stream_id == stream_id && binding.plan.transport != "websocket";
        });
        if (binding_it == resources->stream_bindings.end()) {
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
}

WebSocketStreamRoute Gr4RuntimeManager::resolve_websocket_stream(const domain::Session& session, const std::string& stream_id) {
    auto resources = find_resources(session.id);
    if (!resources) {
        throw runtime_error(session, "websocket route", "session runtime is not running");
    }

    std::lock_guard resource_lock(resources->mutex);
    const auto* execution = resources->execution.get();
    if (execution == nullptr || !execution->running || resources->lifecycle_phase != LifecyclePhase::Running) {
        throw runtime_error(session, "websocket route", "session runtime is not running");
    }

    const auto binding_it = std::ranges::find_if(resources->stream_bindings, [&](const auto& binding) {
        return binding.plan.stream_id == stream_id && binding.plan.transport == "websocket";
    });
    if (binding_it == resources->stream_bindings.end()) {
        throw runtime_error(session, "websocket route", "stream not found in active runtime resources: " + stream_id);
    }

    return WebSocketStreamRoute{.internal = binding_it->internal};
}

void Gr4RuntimeManager::set_block_settings(const domain::Session& session,
                                           const std::string& block_target,
                                           const gr::property_map& patch,
                                           BlockSettingsMode mode) {
    auto resources = find_resources(session.id);
    if (!resources) {
        throw runtime_error(session, "settings update", "session runtime is not running");
    }
    std::lock_guard resource_lock(resources->mutex);
    auto* execution = resources->execution.get();
    if (execution == nullptr || !execution->running || resources->lifecycle_phase != LifecyclePhase::Running) {
        throw runtime_error(session, "settings update", "session runtime is not running");
    }

    auto* scheduler_block = execution->scheduler->asBlockModel();
    if (scheduler_block == nullptr) {
        throw runtime_error(session, "settings update", "scheduler does not expose a runtime block model");
    }

    const auto resolved_block = resolve_runtime_block(*scheduler_block, block_target);
    if (!resolved_block) {
        throw make_block_not_found_error(*scheduler_block, block_target);
    }
    const auto runtime_block_id = std::string(resolved_block->uniqueName());

    const auto* endpoint = mode == BlockSettingsMode::Staged ? gr::block::property::kStagedSetting
                                                             : gr::block::property::kSetting;
    try {
        const auto reply = send_block_settings_request<gr::message::Command::Set>(
            *scheduler_block,
            runtime_block_id,
            endpoint,
            patch,
            std::chrono::milliseconds(1000));
        (void)parse_settings_reply(reply, runtime_block_id, endpoint);
    } catch (const BlockNotFoundError&) {
        throw;
    } catch (const ReplyTimeoutError&) {
        throw;
    } catch (const std::exception& error) {
        throw runtime_error(session, "settings update", error.what());
    }
}

gr::property_map Gr4RuntimeManager::get_block_settings(const domain::Session& session, const std::string& block_target) {
    auto resources = find_resources(session.id);
    if (!resources) {
        throw runtime_error(session, "settings read", "session runtime is not running");
    }
    std::lock_guard resource_lock(resources->mutex);
    auto* execution = resources->execution.get();
    if (execution == nullptr || !execution->running || resources->lifecycle_phase != LifecyclePhase::Running) {
        throw runtime_error(session, "settings read", "session runtime is not running");
    }

    auto* scheduler_block = execution->scheduler->asBlockModel();
    if (scheduler_block == nullptr) {
        throw runtime_error(session, "settings read", "scheduler does not expose a runtime block model");
    }

    const auto resolved_block = resolve_runtime_block(*scheduler_block, block_target);
    if (!resolved_block) {
        throw make_block_not_found_error(*scheduler_block, block_target);
    }
    const auto runtime_block_id = std::string(resolved_block->uniqueName());

    try {
        const auto reply = send_block_settings_request<gr::message::Command::Get>(
            *scheduler_block,
            runtime_block_id,
            gr::block::property::kSetting,
            {},
            std::chrono::milliseconds(1000));
        return parse_settings_reply(reply, runtime_block_id, gr::block::property::kSetting);
    } catch (const BlockNotFoundError&) {
        throw;
    } catch (const ReplyTimeoutError&) {
        throw;
    } catch (const std::exception& error) {
        throw runtime_error(session, "settings read", error.what());
    }
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

    ensure_runtime_environment(plugin_directories_);

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
        auto graph = gr::loadGrc(gr::globalPluginLoader(), normalize_graph_content(session.grc_content, stream_bindings));
        const auto scheduler_alias = session.scheduler_alias.value_or(default_scheduler_alias());
        execution->scheduler = gr::globalPluginLoader().instantiateScheduler(scheduler_alias);
        if (!execution->scheduler) {
            throw runtime_error(session, "prepare", "scheduler not found: " + scheduler_alias);
        }
        execution->scheduler->setGraph(std::move(*graph));
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

void Gr4RuntimeManager::stop_locked(const domain::Session& session,
                                    SessionRuntimeResources& resources,
                                    std::unique_lock<std::mutex>& lock) {
    (void)lock;

    auto* execution = resources.execution.get();
    if (execution == nullptr) {
        release_stream_bindings_locked(resources);
        resources.lifecycle_phase = LifecyclePhase::Idle;
        resources.teardown_complete = true;
        resources.teardown_error.reset();
        return;
    }

    if (resources.lifecycle_phase == LifecyclePhase::Stopping) {
        throw runtime_error(session, "stop", "session runtime stop already in progress");
    }

    resources.lifecycle_phase = LifecyclePhase::Stopping;
    resources.teardown_complete = false;
    resources.teardown_error.reset();

    if (execution->running) {
        try {
            execution->scheduler->stop();
        } catch (const std::exception& error) {
            resources.lifecycle_phase = LifecyclePhase::Failed;
            resources.teardown_error = std::format("failed to stop scheduler: {}", error.what());
            throw runtime_error(session, "stop", *resources.teardown_error);
        }
    }

    execution = resources.execution.get();
    if (execution) {
        execution->running = false;
    }
    release_stream_bindings_locked(resources);
    resources.execution.reset();
    resources.teardown_complete = true;
    if (resources.async_error.has_value()) {
        resources.lifecycle_phase = LifecyclePhase::Failed;
        resources.teardown_error = resources.async_error;
        throw runtime_error(session, "execution", *resources.async_error);
    }
    resources.lifecycle_phase = LifecyclePhase::Idle;
    resources.teardown_error.reset();
}

}  // namespace gr4cp::runtime
