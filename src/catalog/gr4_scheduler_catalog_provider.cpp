#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

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

bool is_alias_scheduler_id(const std::string& id) {
    return id.find('<') == std::string::npos;
}

void register_default_schedulers() {
    auto& registry = gr::globalSchedulerRegistry();
    (void)registry.insert<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>>("=gr::scheduler::SimpleSingle");
    (void)registry.insert<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>>("=gr::scheduler::SimpleMulti");
}

#if !defined(__EMSCRIPTEN__)

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
        return name == "libgnuradio-blocklib-core.dylib" || name == "libgnuradio-plugin.dylib" ||
               name == "libgnuradio-blocklib-core.so" || name == "libgnuradio-plugin.so";
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
                throw SchedulerCatalogLoadError("failed to preload GNU Radio 4 support library " + library +
                                                (error != nullptr ? ": " + std::string(error) : ""));
            }
            handles.push_back(handle);
        }
    }
#else
    (void)plugin_directories;
#endif
}

struct CandidateLibraries {
    std::vector<std::filesystem::path> likely_libraries;
    std::vector<std::filesystem::path> staged_plugin_libraries;
    std::filesystem::path plugin_staging_directory;
};

std::filesystem::path create_stage_directory() {
#if defined(__unix__) || defined(__APPLE__)
    const auto process_id = static_cast<unsigned long long>(::getpid());
#else
    const auto process_id = 0ULL;
#endif
    const auto base = std::filesystem::temp_directory_path() /
                      ("gr4cp-gr4-scheduler-stage-" + std::to_string(process_id) + "-" +
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
        throw SchedulerCatalogLoadError("failed to stage GNU Radio 4 library: " + source.string());
    }
    return destination;
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
        throw SchedulerCatalogLoadError("no likely GNU Radio 4 plugin libraries were found in configured plugin directories");
    }

    candidates.plugin_staging_directory = create_stage_directory();
    for (const auto& library : candidates.likely_libraries) {
        if (is_plugin_library(library)) {
            candidates.staged_plugin_libraries.push_back(
                stage_library(library, candidates.plugin_staging_directory / library.filename()));
        }
    }

    return candidates;
}

template <typename Loader>
void throw_on_failed_plugins(const Loader& loader) {
    if constexpr (requires { loader.failedPlugins(); }) {
        if (!loader.failedPlugins().empty()) {
            const auto& [path, error] = *loader.failedPlugins().begin();
            throw SchedulerCatalogLoadError("failed to load GNU Radio 4 plugin library (" + path + ": " + error + ")");
        }
    }
}

#endif  // !defined(__EMSCRIPTEN__)

}  // namespace

Gr4SchedulerCatalogProvider::Gr4SchedulerCatalogProvider(std::vector<std::filesystem::path> plugin_directories)
    : plugin_directories_(plugin_directories.empty() ? default_plugin_directories() : std::move(plugin_directories)) {}

std::vector<domain::SchedulerDescriptor> Gr4SchedulerCatalogProvider::list() const {
#if defined(__EMSCRIPTEN__)
    register_default_schedulers();
    auto scheduler_ids = gr::globalSchedulerRegistry().keys();
#else
    if (plugin_directories_.empty()) {
        throw SchedulerCatalogLoadError("GNU Radio 4 plugin directories are not configured");
    }

    prepend_library_search_path(plugin_directories_);
    preload_support_libraries(plugin_directories_);
    register_default_schedulers();

    const auto candidate_libraries = collect_candidate_libraries(plugin_directories_);
    std::vector<std::string> plugin_loader_directories;
    if (!candidate_libraries.staged_plugin_libraries.empty()) {
        plugin_loader_directories.push_back(candidate_libraries.plugin_staging_directory.string());
    }

    gr::PluginLoader loader(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), plugin_loader_directories);
    throw_on_failed_plugins(loader);

    auto scheduler_ids = loader.availableSchedulers();
#endif
    std::sort(scheduler_ids.begin(), scheduler_ids.end());
    scheduler_ids.erase(std::unique(scheduler_ids.begin(), scheduler_ids.end()), scheduler_ids.end());
    scheduler_ids.erase(std::remove_if(scheduler_ids.begin(), scheduler_ids.end(),
                                       [](const auto& id) { return !is_alias_scheduler_id(id); }),
                        scheduler_ids.end());
    if (scheduler_ids.empty()) {
        throw SchedulerCatalogLoadError("GNU Radio 4 plugin loader found no registered schedulers");
    }

    std::vector<domain::SchedulerDescriptor> descriptors;
    descriptors.reserve(scheduler_ids.size());
    for (const auto& id : scheduler_ids) {
        descriptors.emplace_back(id);
    }
    return descriptors;
}

}  // namespace gr4cp::catalog
