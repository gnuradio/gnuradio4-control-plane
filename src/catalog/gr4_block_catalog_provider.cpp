#include "gr4cp/catalog/gr4_block_catalog_provider.hpp"
#include "gr4cp/catalog/gr4_block_catalog_provider_detail.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <gnuradio-4.0/BlockModel.hpp>
#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Settings.hpp>
#include <nlohmann/json.hpp>

namespace gr4cp::catalog {

namespace {

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

std::vector<std::filesystem::path> default_plugin_directories() {
    if (const char* env = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES"); env != nullptr && *env != '\0') {
        return split_plugin_directories(env);
    }
#if defined(GR4CP_DEFAULT_GNURADIO4_PLUGIN_DIR)
    return split_plugin_directories(GR4CP_DEFAULT_GNURADIO4_PLUGIN_DIR);
#else
    return {};
#endif
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

struct CandidateLibraries {
    std::vector<std::filesystem::path> likely_libraries;
    std::vector<std::filesystem::path> staged_libraries;
    std::vector<std::filesystem::path> staged_plugin_libraries;
    std::vector<std::filesystem::path> staged_shared_libraries;
    std::filesystem::path plugin_staging_directory;
    std::filesystem::path shared_staging_directory;
};

std::mutex& loader_pool_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::vector<std::unique_ptr<gr::PluginLoader>>& loader_pool() {
    static std::vector<std::unique_ptr<gr::PluginLoader>> pool;
    return pool;
}

gr::PluginLoader* retain_loader(std::unique_ptr<gr::PluginLoader> loader) {
    std::lock_guard lock(loader_pool_mutex());
    auto& pool = loader_pool();
    pool.push_back(std::move(loader));
    return pool.back().get();
}

std::shared_ptr<gr::BlockModel> instantiate_from_retained_loaders(std::string_view block_type, const gr::property_map& params) {
    std::lock_guard lock(loader_pool_mutex());
    for (auto it = loader_pool().rbegin(); it != loader_pool().rend(); ++it) {
        if (auto model = (*it)->instantiate(block_type, params)) {
            return model;
        }
    }
    return {};
}

std::vector<std::string> available_blocks_from_retained_loaders() {
    std::lock_guard lock(loader_pool_mutex());
    std::vector<std::string> blocks;
    for (const auto& loader : loader_pool()) {
        const auto loader_blocks = loader->availableBlocks();
        blocks.insert(blocks.end(), loader_blocks.begin(), loader_blocks.end());
    }
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    return blocks;
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
                throw CatalogLoadError("failed to preload GNU Radio 4 support library " + library +
                                       (error != nullptr ? ": " + std::string(error) : ""));
            }
            handles.push_back(handle);
        }
    }
#else
    (void)plugin_directories;
#endif
}

bool is_likely_gr4_library(const std::filesystem::path& path) {
    if (!is_shared_library(path)) {
        return false;
    }
    const auto name = path.filename().string();
    if (name.rfind("libGr", 0) == 0) {
        return name.find("Shared") != std::string::npos || name.find("Plugin") != std::string::npos;
    }
    if (name.rfind("libGood", 0) == 0 && name.find("Plugin") != std::string::npos) {
        return true;
    }
    return false;
}

bool is_plugin_library(const std::filesystem::path& path) {
    return is_shared_library(path) && path.filename().string().find("Plugin") != std::string::npos;
}

bool is_shared_block_library(const std::filesystem::path& path) {
    return is_shared_library(path) && path.filename().string().find("Shared") != std::string::npos;
}

std::filesystem::path create_stage_directory() {
    const auto base = std::filesystem::temp_directory_path() /
                      ("gr4cp-gr4-stage-" + std::to_string(::getpid()) + "-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(base);
    return base;
}

std::filesystem::path stage_library(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::create_symlink(source, destination, error);
    if (!error) {
        return destination;
    }
    error.clear();
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        throw CatalogLoadError("failed to stage GNU Radio 4 library: " + source.string());
    }
    return destination;
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

void bootstrap_blocklib_symbols(const std::vector<std::filesystem::path>& staged_libraries) {
#if defined(__unix__) || defined(__APPLE__)
    static std::vector<void*> handles;
    static std::set<std::string> retained;

    using init_fn_t = std::size_t (*)(gr::BlockRegistry&);

    for (const auto& library : staged_libraries) {
        const auto library_string = library.string();
        if (!retained.insert(library_string).second) {
            continue;
        }
        void* handle = ::dlopen(library_string.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (handle == nullptr) {
            retained.erase(library_string);
            continue;
        }
        const auto symbol_name = std::string("gr_blocklib_init_module_") + module_name_from_library(library);
        auto* init_fn = reinterpret_cast<init_fn_t>(::dlsym(handle, symbol_name.c_str()));
        if (init_fn == nullptr) {
            handles.push_back(handle);
            continue;
        }
        try {
            (void)init_fn(gr::globalBlockRegistry());
            handles.push_back(handle);
        } catch (...) {
            ::dlclose(handle);
            retained.erase(library_string);
            throw;
        }
    }
#else
    (void)staged_libraries;
#endif
}

CandidateLibraries collect_candidate_libraries(const std::vector<std::filesystem::path>& plugin_directories) {
    CandidateLibraries candidates;
    for (const auto& directory : plugin_directories) {
        if (!std::filesystem::is_directory(directory)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file() || !is_likely_gr4_library(entry.path())) {
                continue;
            }
            candidates.likely_libraries.push_back(std::filesystem::absolute(entry.path()));
        }
    }

    std::sort(candidates.likely_libraries.begin(), candidates.likely_libraries.end());
    candidates.likely_libraries.erase(std::unique(candidates.likely_libraries.begin(), candidates.likely_libraries.end()),
                                      candidates.likely_libraries.end());
    if (candidates.likely_libraries.empty()) {
        throw CatalogLoadError("no likely GNU Radio 4 plugin libraries were found in configured plugin directories");
    }

    candidates.plugin_staging_directory = create_stage_directory();
    candidates.shared_staging_directory = create_stage_directory();
    for (const auto& library : candidates.likely_libraries) {
        if (is_plugin_library(library)) {
            const auto staged = stage_library(library, candidates.plugin_staging_directory / library.filename());
            candidates.staged_libraries.push_back(staged);
            candidates.staged_plugin_libraries.push_back(staged);
        }
        if (is_shared_block_library(library)) {
            const auto staged = stage_library(library, candidates.shared_staging_directory / library.filename());
            candidates.staged_libraries.push_back(staged);
            candidates.staged_shared_libraries.push_back(staged);
        }
    }
    return candidates;
}

std::string to_title_case(std::string value) {
    bool capitalize = true;
    for (char& ch : value) {
        if (ch == '_' || ch == '-' || ch == '.') {
            ch = ' ';
            capitalize = true;
            continue;
        }
        ch = static_cast<char>(capitalize ? std::toupper(static_cast<unsigned char>(ch))
                                          : std::tolower(static_cast<unsigned char>(ch)));
        capitalize = std::isspace(static_cast<unsigned char>(ch)) != 0;
    }
    return value;
}

std::string derive_name_from_id(const std::string& id) {
    const auto separator = id.find_last_of("./:");
    const auto raw_name = separator == std::string::npos ? id : id.substr(separator + 1);
    return to_title_case(raw_name);
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

std::optional<gr::property_map> value_to_map(const gr::pmt::Value& value) {
    std::optional<gr::property_map> result;
    gr::pmt::ValueVisitor([&result](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, gr::property_map>) {
            result = item;
        }
    }).visit(value);
    return result;
}

std::optional<std::vector<std::string>> value_to_string_list(const gr::pmt::Value& value) {
    std::optional<std::vector<std::string>> result;
    gr::pmt::ValueVisitor([&result](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, gr::Tensor<std::pmr::string>>) {
            std::vector<std::string> values;
            values.reserve(item.size());
            for (const auto& entry : item) {
                values.emplace_back(entry);
            }
            result = std::move(values);
        } else if constexpr (std::same_as<T, gr::Tensor<gr::pmt::Value>>) {
            std::vector<std::string> values;
            values.reserve(item.size());
            for (const auto& entry : item) {
                const auto string_value = value_to_string(entry);
                if (!string_value.has_value()) {
                    return;
                }
                values.push_back(*string_value);
            }
            result = std::move(values);
        } else if constexpr (std::same_as<T, std::string_view>) {
            result = std::vector<std::string>{std::string(item)};
        }
    }).visit(value);
    return result;
}

std::string block_name(const gr::BlockModel& block, const std::string& id) {
    const std::string reflected_name(block.name());
    if (!reflected_name.empty() && reflected_name != id && reflected_name.find("::") == std::string::npos) {
        return reflected_name;
    }
    return derive_name_from_id(id);
}

std::string block_category(const gr::BlockModel& block, const std::string& id) {
    return detail::derive_category_from_metadata(block.metaInformation(), id);
}

std::optional<std::string> parameter_meta_string(const gr::property_map& meta,
                                                 std::string_view parameter_name,
                                                 std::string_view suffix);

std::string block_summary(const gr::BlockModel& block) {
    const auto& meta = block.metaInformation();
    if (const auto description = parameter_meta_string(meta, "", "description");
        description.has_value() && !description->empty()) {
        return *description;
    }
    if (const auto description = parameter_meta_string(meta, "", "Description");
        description.has_value() && !description->empty()) {
        return *description;
    }
    return "";
}

std::optional<std::string> parameter_meta_string(const gr::property_map& meta,
                                                 std::string_view parameter_name,
                                                 std::string_view suffix) {
    const auto it = meta.find(std::string(parameter_name) + std::string(suffix));
    if (it == meta.end()) {
        return std::nullopt;
    }
    return value_to_string(it->second);
}

std::optional<std::vector<std::string>> parameter_meta_string_list(const gr::property_map& meta,
                                                                   std::string_view parameter_name,
                                                                   std::string_view suffix) {
    const auto it = meta.find(std::string(parameter_name) + std::string(suffix));
    if (it == meta.end()) {
        return std::nullopt;
    }
    return value_to_string_list(it->second);
}

std::string parameter_type_name(const gr::pmt::Value& value) {
    using ValueType = gr::pmt::Value::ValueType;

    if (value.is_map()) {
        return "map";
    }
    if (value.is_complex()) {
        return "complex";
    }
    if (value.is_tensor()) {
        switch (value.value_type()) {
        case ValueType::Bool: return "bool_vector";
        case ValueType::Int8:
        case ValueType::Int16:
        case ValueType::Int32:
        case ValueType::Int64:
        case ValueType::UInt8:
        case ValueType::UInt16:
        case ValueType::UInt32:
        case ValueType::UInt64: return "int_vector";
        case ValueType::Float32: return "float32_vector";
        case ValueType::Float64: return "float64_vector";
        case ValueType::ComplexFloat32: return "complex_float32_vector";
        case ValueType::ComplexFloat64: return "complex_float64_vector";
        case ValueType::String: return "string_vector";
        case ValueType::Value: return "value_vector";
        case ValueType::Monostate: return "vector";
        }
    }
    if (value.is_string()) {
        return "string";
    }
    if (value.value_type() == gr::pmt::Value::ValueType::Bool) {
        return "bool";
    }
    if (value.is_integral()) {
        return "int";
    }
    if (value.is_floating_point()) {
        return "float";
    }
    return "value";
}

domain::BlockParameterDefault parameter_default_value(const gr::pmt::Value& value) {
    domain::BlockParameterDefault result = nullptr;
    gr::pmt::ValueVisitor([&result](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, bool>) {
            result = item;
        } else if constexpr (std::integral<T> && !std::same_as<T, bool>) {
            if constexpr (std::signed_integral<T>) {
                if (item >= std::numeric_limits<int>::min() && item <= std::numeric_limits<int>::max()) {
                    result = static_cast<int>(item);
                }
            } else if (item <= static_cast<T>(std::numeric_limits<int>::max())) {
                result = static_cast<int>(item);
            }
        } else if constexpr (std::floating_point<T>) {
            result = static_cast<double>(item);
        } else if constexpr (std::same_as<T, std::pmr::string> || std::same_as<T, std::string>) {
            result = std::string(item);
        } else if constexpr (std::same_as<T, std::string_view>) {
            result = std::string(item);
        }
    }).visit(value);
    return result;
}

std::optional<std::string> parameter_value_kind(const gr::pmt::Value& value) {
    if (!value.is_map() && !value.is_tensor()) {
        return std::string("scalar");
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> parameter_enum_choices(const gr::property_map& meta, std::string_view name) {
    return parameter_meta_string_list(meta, name, "::enum_values");
}

std::optional<std::string> parameter_enum_type(const gr::property_map& meta, std::string_view name) {
    return parameter_meta_string(meta, name, "::enum_type");
}

std::optional<std::string> parameter_runtime_mutability(std::string_view name) {
    if (name == "name" || name == "unique_name") {
        return std::string("immutable");
    }
    return std::nullopt;
}

std::optional<std::string> parameter_ui_hint(std::string_view name) {
    if (name == "name" || name == "unique_name") {
        return std::string("read_only");
    }
    if (name == "compute_domain" || name == "disconnect_on_done" || name == "ui_constraints" ||
        name == "input_chunk_size" || name == "output_chunk_size" || name == "stride") {
        return std::string("advanced");
    }

    return std::nullopt;
}

std::string parameter_summary(const gr::property_map& meta, std::string_view name) {
    if (const auto description = parameter_meta_string(meta, name, "::description");
        description.has_value() && !description->empty()) {
        return *description;
    }
    if (const auto documentation = parameter_meta_string(meta, name, "::documentation");
        documentation.has_value() && !documentation->empty()) {
        return *documentation;
    }
    return "";
}

std::string_view cardinality_kind_to_string(domain::BlockPortCardinalityKind kind) {
    switch (kind) {
    case domain::BlockPortCardinalityKind::Fixed: return "fixed";
    case domain::BlockPortCardinalityKind::Dynamic: return "dynamic";
    }
    return "fixed";
}

std::optional<domain::BlockPortCardinalityKind> cardinality_kind_from_string(std::string_view value) {
    if (value == "fixed") {
        return domain::BlockPortCardinalityKind::Fixed;
    }
    if (value == "dynamic") {
        return domain::BlockPortCardinalityKind::Dynamic;
    }
    return std::nullopt;
}

std::optional<int> port_count_lower_bound(std::string_view parameter_name) {
    if (parameter_name == "n_inputs" || parameter_name == "n_outputs" || parameter_name == "n_ports") {
        return 1;
    }
    return std::nullopt;
}

std::optional<int> port_count_upper_bound(std::string_view parameter_name) {
    if (parameter_name == "n_inputs" || parameter_name == "n_outputs" || parameter_name == "n_ports") {
        return 32;
    }
    return std::nullopt;
}

std::optional<std::string> infer_collection_size_parameter(std::string_view collection_name,
                                                           bool is_input,
                                                           const std::vector<domain::BlockParameterDescriptor>& parameters) {
    std::vector<std::string> candidates;
    const auto add_candidate = [&](std::string value) {
        candidates.push_back(std::move(value));
    };

    auto add_patterns = [&](std::string_view alias) {
        add_candidate("n_" + std::string(alias));
        add_candidate("num_" + std::string(alias));
        add_candidate(std::string(alias));
        add_candidate(std::string(alias) + "_count");
        add_candidate("count_" + std::string(alias));
    };

    const std::string base(collection_name);
    const auto singular = [&base]() {
        if (base.size() > 3 && base.ends_with("ies")) {
            return base.substr(0, base.size() - 3) + "y";
        }
        if (base.size() > 1 && base.ends_with('s')) {
            return base.substr(0, base.size() - 1);
        }
        return base;
    }();
    const auto plural = [&base]() {
        if (base.ends_with('s')) {
            return base;
        }
        if (base.ends_with('y') && base.size() > 1) {
            return base.substr(0, base.size() - 1) + "ies";
        }
        return base + "s";
    }();

    add_patterns(base);
    if (singular != base) {
        add_patterns(singular);
    }
    if (plural != base && plural != singular) {
        add_patterns(plural);
    }
    add_patterns(is_input ? "in" : "out");
    add_patterns(is_input ? "input" : "output");
    add_patterns(is_input ? "inputs" : "outputs");

    std::vector<std::string> unique_candidates;
    std::set<std::string> seen;
    unique_candidates.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        if (seen.insert(candidate).second) {
            unique_candidates.push_back(candidate);
        }
    }

    const auto matches = [&](const std::string& candidate) {
        return std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
                   return parameter.name == candidate;
               }) != parameters.end();
    };

    for (const auto& candidate : unique_candidates) {
        if (matches(candidate)) {
            return candidate;
        }
    }

    return std::nullopt;
}

std::optional<int> parameter_default_int(const domain::BlockParameterDescriptor& parameter) {
    if (const auto* default_value = std::get_if<int>(&parameter.default_value)) {
        return *default_value;
    }
    return std::nullopt;
}

std::string trim_copy(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\n\r");
    return std::string(value.substr(first, last - first + 1));
}

std::optional<std::string> infer_collection_element_type(std::string_view block_id) {
    const auto open = block_id.find('<');
    const auto close = block_id.rfind('>');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open + 1) {
        return std::nullopt;
    }

    std::string_view args = block_id.substr(open + 1, close - open - 1);
    std::size_t depth = 0;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const char ch = args[index];
        if (ch == '<') {
            ++depth;
        } else if (ch == '>') {
            if (depth > 0) {
                --depth;
            }
        } else if (ch == ',' && depth == 0) {
            args = args.substr(0, index);
            break;
        }
    }

    const auto type = trim_copy(args);
    if (type.empty()) {
        return std::nullopt;
    }
    return type;
}

std::optional<int> infer_collection_render_port_count(std::string_view collection_name,
                                                      bool is_input,
                                                      const std::vector<domain::BlockParameterDescriptor>& parameters,
                                                      std::size_t current_count,
                                                      std::optional<int> min_port_count,
                                                      std::optional<int> max_port_count) {
    const auto size_parameter = infer_collection_size_parameter(collection_name, is_input, parameters);
    if (size_parameter.has_value()) {
        const auto parameter_it = std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
            return parameter.name == *size_parameter;
        });
        if (parameter_it != parameters.end()) {
            if (const auto default_count = parameter_default_int(*parameter_it); default_count.has_value()) {
                auto count = *default_count;
                if (min_port_count.has_value() && count < *min_port_count) {
                    count = *min_port_count;
                }
                if (max_port_count.has_value() && *max_port_count >= 0 && count > *max_port_count) {
                    count = *max_port_count;
                }
                return count;
            }
        }
    }

    if (current_count > 0) {
        return static_cast<int>(current_count);
    }
    if (min_port_count.has_value() && *min_port_count > 0) {
        return *min_port_count;
    }
    return std::nullopt;
}

struct CollectionMetadata {
    domain::BlockPortCardinalityKind cardinality_kind{domain::BlockPortCardinalityKind::Fixed};
    std::optional<int> current_port_count;
    std::optional<int> render_port_count;
    std::optional<int> min_port_count;
    std::optional<int> max_port_count;
    std::optional<std::string> size_parameter;
    std::optional<std::string> handle_name_template;
    std::optional<std::string> element_type;
};

std::optional<std::string> reflected_canonical_type(const gr::BlockModel& block) {
    const auto canonical = std::string(block.typeName());
    if (canonical.empty()) {
        return std::nullopt;
    }
    return canonical;
}

CollectionMetadata infer_collection_metadata(std::string_view collection_name,
                                             std::string_view block_id,
                                             bool is_input,
                                             const std::vector<domain::BlockParameterDescriptor>& parameters,
                                             std::size_t current_count) {
    CollectionMetadata metadata;
    metadata.cardinality_kind = domain::BlockPortCardinalityKind::Dynamic;
    metadata.current_port_count = static_cast<int>(current_count);
    metadata.min_port_count = current_count == 0 ? 0 : 1;
    metadata.max_port_count = -1;
    metadata.size_parameter = infer_collection_size_parameter(collection_name, is_input, parameters);
    metadata.handle_name_template = std::string(collection_name) + "#${index}";
    metadata.element_type = infer_collection_element_type(block_id);

    if (metadata.size_parameter.has_value()) {
        if (const auto lower = port_count_lower_bound(*metadata.size_parameter)) {
            metadata.min_port_count = *lower;
        }
        if (const auto upper = port_count_upper_bound(*metadata.size_parameter)) {
            metadata.max_port_count = *upper;
        }
    }
    metadata.render_port_count = infer_collection_render_port_count(collection_name,
                                                                     is_input,
                                                                     parameters,
                                                                     current_count,
                                                                     metadata.min_port_count,
                                                                     metadata.max_port_count);

    return metadata;
}

domain::BlockPortDescriptor to_port_descriptor(const gr::BlockModel::DynamicPortOrCollection& port_or_collection,
                                               std::string_view block_id,
                                               const std::vector<domain::BlockParameterDescriptor>& parameters,
                                               bool is_input) {
    return std::visit(
        [&](const auto& item) -> domain::BlockPortDescriptor {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::same_as<T, gr::DynamicPort>) {
                domain::BlockPortDescriptor descriptor;
                descriptor.name = std::string(item.metaInfo.name);
                descriptor.type = item.typeName();
                descriptor.cardinality_kind = domain::BlockPortCardinalityKind::Fixed;
                descriptor.current_port_count = 1;
                descriptor.render_port_count = 1;
                descriptor.min_port_count = 1;
                descriptor.max_port_count = 1;
                return descriptor;
            } else {
                std::string type = item.ports.empty() ? std::string() : std::string(item.ports.front().typeName());
                const auto metadata = infer_collection_metadata(item.name, block_id, is_input, parameters, item.ports.size());
                if (type.empty() && metadata.element_type.has_value()) {
                    type = *metadata.element_type;
                }
                domain::BlockPortDescriptor descriptor;
                descriptor.name = std::string(item.name);
                descriptor.type = std::move(type);
                descriptor.cardinality_kind = metadata.cardinality_kind;
                descriptor.current_port_count = metadata.current_port_count;
                descriptor.render_port_count = metadata.render_port_count;
                descriptor.min_port_count = metadata.min_port_count;
                descriptor.max_port_count = metadata.max_port_count;
                descriptor.size_parameter = metadata.size_parameter;
                descriptor.handle_name_template = metadata.handle_name_template;
                return descriptor;
            }
        },
        port_or_collection);
}

std::vector<domain::BlockPortDescriptor> to_ports(gr::BlockModel::DynamicPorts& ports,
                                                  std::string_view block_id,
                                                  const std::vector<domain::BlockParameterDescriptor>& parameters,
                                                  bool is_input) {
    std::vector<domain::BlockPortDescriptor> descriptors;
    descriptors.reserve(ports.size());
    for (const auto& port : ports) {
        descriptors.push_back(to_port_descriptor(port, block_id, parameters, is_input));
    }
    return descriptors;
}

std::vector<domain::BlockParameterDescriptor> to_parameters(gr::BlockModel& block) {
    std::vector<domain::BlockParameterDescriptor> descriptors;
    (void)block.settings().applyStagedParameters();
    const auto settings = block.settings().get();
    const auto& meta_information = block.metaInformation();
    descriptors.reserve(settings.size());
    for (const auto& [name, value] : settings) {
        domain::BlockParameterDescriptor descriptor(
            std::string(name),
            parameter_type_name(value),
            false,
            parameter_default_value(value),
            parameter_summary(meta_information, name));
        descriptor.runtime_mutability = parameter_runtime_mutability(name);
        descriptor.value_kind = parameter_value_kind(value);
        if (const auto enum_choices = parameter_enum_choices(meta_information, name); enum_choices.has_value()) {
            descriptor.enum_choices = *enum_choices;
        }
        descriptor.enum_type = parameter_enum_type(meta_information, name);
        descriptor.ui_hint = parameter_ui_hint(name);
        descriptors.push_back(std::move(descriptor));
    }
    std::sort(descriptors.begin(), descriptors.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return descriptors;
}

domain::BlockDescriptor reflect_block(const std::string& id) {
    gr::property_map init_params;
    std::shared_ptr<gr::BlockModel> block = instantiate_from_retained_loaders(id, init_params);
    if (!block) {
        if (auto created = gr::globalBlockRegistry().create(id, std::move(init_params))) {
            block = std::shared_ptr<gr::BlockModel>(created.release());
        }
    }
    if (!block) {
        throw CatalogLoadError("failed to instantiate GNU Radio 4 block for catalog: " + id);
    }

    block->settings().init();
    auto parameters = to_parameters(*block);
    const auto summary = block_summary(*block);
    return {
        .id = id,
        .canonical_type = reflected_canonical_type(*block),
        .name = block_name(*block, id),
        .category = block_category(*block, id),
        .summary = summary,
        .inputs = to_ports(block->dynamicInputPorts(), id, parameters, true),
        .outputs = to_ports(block->dynamicOutputPorts(), id, parameters, false),
        .parameters = std::move(parameters),
    };
}

nlohmann::json to_json(const domain::BlockParameterDefault& value) {
    return std::visit([](const auto& item) -> nlohmann::json { return item; }, value);
}

nlohmann::json to_json(const domain::BlockPortDescriptor& port) {
    nlohmann::json value{{"name", port.name},
                         {"type", port.type},
                         {"cardinality_kind", std::string(cardinality_kind_to_string(port.cardinality_kind))}};
    if (port.current_port_count.has_value()) {
        value["current_port_count"] = *port.current_port_count;
    }
    if (port.render_port_count.has_value()) {
        value["render_port_count"] = *port.render_port_count;
    }
    if (port.min_port_count.has_value()) {
        value["min_port_count"] = *port.min_port_count;
    }
    if (port.max_port_count.has_value()) {
        value["max_port_count"] = *port.max_port_count;
    }
    if (port.size_parameter.has_value()) {
        value["size_parameter"] = *port.size_parameter;
    }
    if (port.handle_name_template.has_value()) {
        value["handle_name_template"] = *port.handle_name_template;
    }
    return value;
}

nlohmann::json to_json(const domain::BlockParameterDescriptor& parameter) {
    nlohmann::json value{{"name", parameter.name},
                         {"type", parameter.type},
                         {"required", parameter.required},
                         {"default_value", to_json(parameter.default_value)},
                         {"summary", parameter.summary}};
    if (parameter.runtime_mutability.has_value()) {
        value["runtime_mutability"] = *parameter.runtime_mutability;
    }
    if (parameter.value_kind.has_value()) {
        value["value_kind"] = *parameter.value_kind;
    }
    if (parameter.enum_choices.has_value()) {
        value["enum_choices"] = *parameter.enum_choices;
    }
    if (parameter.enum_type.has_value()) {
        value["enum_type"] = *parameter.enum_type;
    }
    if (!parameter.enum_labels.empty()) {
        value["enum_labels"] = parameter.enum_labels;
    }
    if (parameter.enum_source.has_value()) {
        value["enum_source"] = *parameter.enum_source;
    }
    if (parameter.ui_hint.has_value()) {
        value["ui_hint"] = *parameter.ui_hint;
    }
    if (parameter.allow_custom_value.has_value()) {
        value["allow_custom_value"] = *parameter.allow_custom_value;
    }
    return value;
}

nlohmann::json to_json(const domain::BlockDescriptor& block) {
    nlohmann::json inputs = nlohmann::json::array();
    for (const auto& input : block.inputs) {
        inputs.push_back(to_json(input));
    }

    nlohmann::json outputs = nlohmann::json::array();
    for (const auto& output : block.outputs) {
        outputs.push_back(to_json(output));
    }

    nlohmann::json parameters = nlohmann::json::array();
    for (const auto& parameter : block.parameters) {
        parameters.push_back(to_json(parameter));
    }

    nlohmann::json value{{"id", block.id},
                         {"name", block.name},
                         {"category", block.category},
                         {"summary", block.summary},
                         {"inputs", std::move(inputs)},
                         {"outputs", std::move(outputs)},
                         {"parameters", std::move(parameters)}};
    if (block.canonical_type.has_value()) {
        value["canonical_type"] = *block.canonical_type;
    }
    return value;
}

domain::BlockParameterDefault parse_default_value(const nlohmann::json& value) {
    if (value.is_null()) {
        return nullptr;
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<int>();
    }
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    throw CatalogLoadError("invalid GNU Radio 4 block reflection payload: unsupported parameter default value");
}

domain::BlockPortDescriptor parse_port_descriptor(const nlohmann::json& value) {
    domain::BlockPortDescriptor port;
    port.name = value.at("name").get<std::string>();
    port.type = value.at("type").get<std::string>();
    if (const auto it = value.find("cardinality_kind"); it != value.end() && it->is_string()) {
        if (const auto parsed = cardinality_kind_from_string(it->get<std::string>()); parsed.has_value()) {
            port.cardinality_kind = *parsed;
        }
    }
    if (const auto it = value.find("current_port_count"); it != value.end() && it->is_number_integer()) {
        port.current_port_count = it->get<int>();
    }
    if (const auto it = value.find("render_port_count"); it != value.end() && it->is_number_integer()) {
        port.render_port_count = it->get<int>();
    }
    if (const auto it = value.find("min_port_count"); it != value.end() && it->is_number_integer()) {
        port.min_port_count = it->get<int>();
    }
    if (const auto it = value.find("max_port_count"); it != value.end() && it->is_number_integer()) {
        port.max_port_count = it->get<int>();
    }
    if (const auto it = value.find("size_parameter"); it != value.end() && it->is_string()) {
        port.size_parameter = it->get<std::string>();
    }
    if (const auto it = value.find("handle_name_template"); it != value.end() && it->is_string()) {
        port.handle_name_template = it->get<std::string>();
    }
    return port;
}

domain::BlockParameterDescriptor parse_parameter_descriptor(const nlohmann::json& value) {
    domain::BlockParameterDescriptor parameter(
        value.at("name").get<std::string>(),
        value.at("type").get<std::string>(),
        value.at("required").get<bool>(),
        parse_default_value(value.at("default_value")),
        value.at("summary").get<std::string>());
    if (const auto it = value.find("runtime_mutability"); it != value.end() && it->is_string()) {
        parameter.runtime_mutability = it->get<std::string>();
    }
    if (const auto it = value.find("value_kind"); it != value.end() && it->is_string()) {
        parameter.value_kind = it->get<std::string>();
    }
    if (const auto it = value.find("enum_choices"); it != value.end() && it->is_array()) {
        parameter.enum_choices = it->get<std::vector<std::string>>();
    }
    if (const auto it = value.find("enum_type"); it != value.end() && it->is_string()) {
        parameter.enum_type = it->get<std::string>();
    }
    if (const auto it = value.find("enum_labels"); it != value.end() && it->is_object()) {
        parameter.enum_labels = it->get<std::map<std::string, std::string>>();
    }
    if (const auto it = value.find("enum_source"); it != value.end() && it->is_string()) {
        parameter.enum_source = it->get<std::string>();
    }
    if (const auto it = value.find("ui_hint"); it != value.end() && it->is_string()) {
        parameter.ui_hint = it->get<std::string>();
    }
    if (const auto it = value.find("allow_custom_value"); it != value.end() && it->is_boolean()) {
        parameter.allow_custom_value = it->get<bool>();
    }
    return parameter;
}

domain::BlockDescriptor parse_block_descriptor(const std::string& payload) {
    const auto value = nlohmann::json::parse(payload);
    domain::BlockDescriptor block;
    block.id = value.at("id").get<std::string>();
    if (const auto it = value.find("canonical_type"); it != value.end() && it->is_string()) {
        const auto canonical_type = it->get<std::string>();
        if (!canonical_type.empty()) {
            block.canonical_type = canonical_type;
        }
    }
    block.name = value.at("name").get<std::string>();
    block.category = value.at("category").get<std::string>();
    block.summary = value.at("summary").get<std::string>();

    for (const auto& input : value.at("inputs")) {
        block.inputs.push_back(parse_port_descriptor(input));
    }
    for (const auto& output : value.at("outputs")) {
        block.outputs.push_back(parse_port_descriptor(output));
    }
    for (const auto& parameter : value.at("parameters")) {
        block.parameters.push_back(parse_parameter_descriptor(parameter));
    }
    return block;
}

void write_all(int fd, std::string_view data) {
    std::size_t written = 0;
    while (written < data.size()) {
        const auto result = ::write(fd, data.data() + written, data.size() - written);
        if (result <= 0) {
            break;
        }
        written += static_cast<std::size_t>(result);
    }
}

domain::BlockDescriptor reflect_block_in_subprocess(const std::string& id) {
    std::array<int, 2> pipe_fds{-1, -1};
    if (::pipe(pipe_fds.data()) != 0) {
        throw CatalogLoadError("failed to create GNU Radio 4 reflection pipe for block: " + id);
    }

    const pid_t pid = ::fork();
    if (pid == -1) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        throw CatalogLoadError("failed to fork GNU Radio 4 reflection worker for block: " + id);
    }

    if (pid == 0) {
        ::close(pipe_fds[0]);
        try {
            const auto payload = to_json(reflect_block(id)).dump();
            write_all(pipe_fds[1], payload);
            ::close(pipe_fds[1]);
            _exit(0);
        } catch (...) {
            ::close(pipe_fds[1]);
            _exit(2);
        }
    }

    ::close(pipe_fds[1]);
    std::string payload;
    std::array<char, 4096> buffer{};
    while (true) {
        const auto count = ::read(pipe_fds[0], buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            ::close(pipe_fds[0]);
            int status = 0;
            (void)::waitpid(pid, &status, 0);
            throw CatalogLoadError("failed to read GNU Radio 4 reflection payload for block: " + id);
        }
        payload.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(pipe_fds[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) == -1) {
        throw CatalogLoadError("failed to wait for GNU Radio 4 reflection worker for block: " + id);
    }
    if (WIFSIGNALED(status)) {
        std::ostringstream message;
        message << "GNU Radio 4 block reflection crashed for block " << id << " (signal " << WTERMSIG(status) << ")";
        throw CatalogLoadError(message.str());
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw CatalogLoadError("GNU Radio 4 block reflection failed for block: " + id);
    }
    if (payload.empty()) {
        throw CatalogLoadError("GNU Radio 4 block reflection returned empty payload for block: " + id);
    }

    try {
        return parse_block_descriptor(payload);
    } catch (const std::exception& error) {
        throw CatalogLoadError("GNU Radio 4 block reflection returned invalid payload for block " + id + ": " +
                               error.what());
    }
}

}  // namespace

namespace detail {

bool is_sane_explicit_category(std::string_view value) {
    return !value.empty() && value.find_first_of("<>(),") == std::string_view::npos;
}

std::string derive_category_from_block_id(std::string_view id) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start <= id.size()) {
        const auto separator = id.find("::", start);
        const auto token = id.substr(start, separator == std::string_view::npos ? id.size() - start : separator - start);
        if (!token.empty()) {
            segments.push_back(token);
        }
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 2;
    }

    if (!segments.empty() && segments.front() == "gr") {
        segments.erase(segments.begin());
    }

    if (segments.size() < 2) {
        return "Uncategorized";
    }

    segments.pop_back();

    std::string category;
    for (const auto segment : segments) {
        if (!category.empty()) {
            category += "/";
        }
        category.append(segment);
    }

    return category.empty() ? "Uncategorized" : category;
}

std::string derive_category_from_metadata(const gr::property_map& meta, std::string_view fallback_id) {
    const auto drawable = meta.find("Drawable");
    if (drawable != meta.end()) {
        const auto drawable_info = value_to_map(drawable->second);
        if (drawable_info.has_value()) {
            const auto category = drawable_info->find("Category");
            if (category != drawable_info->end()) {
                if (const auto value = value_to_string(category->second); value.has_value() && !value->empty()) {
                    if (is_sane_explicit_category(*value)) {
                        return *value;
                    }
                }
            }
        }
    }
    return derive_category_from_block_id(fallback_id);
}

}  // namespace detail

Gr4BlockCatalogProvider::Gr4BlockCatalogProvider(std::vector<std::filesystem::path> plugin_directories)
    : plugin_directories_(plugin_directories.empty() ? default_plugin_directories() : std::move(plugin_directories)) {}

std::vector<domain::BlockDescriptor> Gr4BlockCatalogProvider::list() const {
    if (plugin_directories_.empty()) {
        throw CatalogLoadError("GNU Radio 4 plugin directories are not configured");
    }

    prepend_library_search_path(plugin_directories_);
    preload_support_libraries(plugin_directories_);
    const auto candidate_libraries = collect_candidate_libraries(plugin_directories_);
    bootstrap_blocklib_symbols(candidate_libraries.staged_shared_libraries);

    std::vector<std::string> plugin_loader_directories;
    if (!candidate_libraries.staged_plugin_libraries.empty()) {
        plugin_loader_directories.push_back(candidate_libraries.plugin_staging_directory.string());
    }
    auto plugin_loader = std::make_unique<gr::PluginLoader>(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(),
                                                            plugin_loader_directories);
    if (!plugin_loader->failedPlugins().empty()) {
        const auto& [path, error] = *plugin_loader->failedPlugins().begin();
        throw CatalogLoadError("failed to load GNU Radio 4 plugin library (" + path + ": " + error + ")");
    }
    retain_loader(std::move(plugin_loader));

    auto blocks = gr::globalBlockRegistry().keys();
    const auto retained_blocks = available_blocks_from_retained_loaders();
    blocks.insert(blocks.end(), retained_blocks.begin(), retained_blocks.end());
    std::sort(blocks.begin(), blocks.end());
    blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
    if (blocks.empty()) {
        throw CatalogLoadError("GNU Radio 4 plugin loader found no registered blocks");
    }

    std::vector<domain::BlockDescriptor> descriptors;
    descriptors.reserve(blocks.size());
    for (const auto& id : blocks) {
        try {
            descriptors.push_back(reflect_block_in_subprocess(id));
        } catch (const CatalogLoadError& error) {
            std::cerr << "Skipping GNU Radio 4 block from catalog: " << error.what() << '\n';
        }
    }
    if (descriptors.empty()) {
        throw CatalogLoadError("GNU Radio 4 catalog reflection produced no usable blocks");
    }
    return descriptors;
}

}  // namespace gr4cp::catalog
