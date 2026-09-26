#include "gr4cp/runtime/plugin_loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <gnuradio-4.0/PluginLoader.hpp>

namespace gr4cp::runtime {

namespace {

#if defined(GR4CP_PLUGIN_DIRECTORIES)
constexpr std::string_view kReplacingDirectories = GR4CP_PLUGIN_DIRECTORIES;
#else
constexpr std::string_view kReplacingDirectories = "";
#endif
#if defined(GR4CP_EXTRA_PLUGIN_DIRECTORIES)
constexpr std::string_view kExtraDirectories = GR4CP_EXTRA_PLUGIN_DIRECTORIES;
#else
constexpr std::string_view kExtraDirectories = "";
#endif

// Appends each directory of the colon-separated `list` that `directories` does not hold yet.
void append_directories(std::vector<std::string>& directories, std::string_view list) {
    for (const auto part : std::views::split(list, ':')) {
        std::string directory(std::string_view(part.begin(), part.end()));
        if (!directory.empty() && std::ranges::find(directories, directory) == directories.end()) {
            directories.push_back(std::move(directory));
        }
    }
}

#if !defined(__EMSCRIPTEN__)
// Core's reason for a shared object that opened, has no plugin entry point and registered nothing; core gives such a
// file no other mark. Startup passes over such a file, as a library directory holds core libraries of this kind. A
// rewording in core makes every such file stop startup, and
// PluginLoaderTest.StartupPassesOverACoreLibraryBesideThePlugins fails.
constexpr std::string_view kNoEntryPointReason = "Failed to load symbol gr_plugin_make";

std::string joined(std::span<const std::string> directories) {
    std::string text;
    for (const auto& directory : directories) {
        text += text.empty() ? directory : ":" + directory;
    }
    return text.empty() ? "<none>" : text;
}
#endif

}  // namespace

std::vector<std::string> plugin_directories(std::string_view configured_default) {
    std::vector<std::string> directories;
    if (const char* environment = std::getenv("GNURADIO4_PLUGIN_DIRECTORIES"); environment != nullptr) {
        append_directories(directories, environment);
    }
    append_directories(directories, configured_default);
    return directories;
}

std::vector<std::string> plugin_directories() {
#if defined(__EMSCRIPTEN__)
    return {};
#else
    std::string configured_default = kReplacingDirectories.empty() ? std::string(gr::installedPluginDirectory())
                                                                   : std::string(kReplacingDirectories);
    if (!kExtraDirectories.empty()) {
        configured_default += ":" + std::string(kExtraDirectories);
    }
    return plugin_directories(configured_default);
#endif
}

std::vector<std::pair<std::string, std::string>> load_failures([[maybe_unused]] const gr::PluginLoader& loader) {
#if defined(__EMSCRIPTEN__)
    return {};
#else
    std::vector<std::pair<std::string, std::string>> failures;
    for (const auto& [file, reason] : loader.failedPlugins()) {
        if (reason != kNoEntryPointReason) {
            failures.emplace_back(file, reason);
        }
    }
    std::ranges::sort(failures);
    return failures;
#endif
}

std::shared_ptr<gr::PluginLoader> load_plugins() {
#if defined(__EMSCRIPTEN__)
    return std::make_shared<gr::PluginLoader>(gr::globalBlockRegistry(), gr::globalSchedulerRegistry(), std::span<const std::string>{});
#else
    return load_plugins(plugin_directories(), gr::globalBlockRegistry(), gr::globalSchedulerRegistry());
#endif
}

#if !defined(__EMSCRIPTEN__)
std::shared_ptr<gr::PluginLoader> load_plugins(std::span<const std::string> directories,
                                               gr::BlockRegistry& blocks,
                                               gr::SchedulerRegistry& schedulers) {
    const auto exists = [](const std::string& directory) {
        std::error_code ignored;
        return std::filesystem::is_directory(directory, ignored);
    };
    if (std::ranges::none_of(directories, exists)) {
        throw PluginLoadError(std::format("no GNU Radio 4 plugin directory exists: {}", joined(directories)));
    }

    for (const auto& directory : directories) {
        std::error_code error;
        if (exists(directory)) {
            std::ignore = std::filesystem::directory_iterator(directory, error);
        }
        if (error) {
            throw PluginLoadError(std::format("cannot read GNU Radio 4 plugin directory ({}: {})", directory, error.message()));
        }
    }

    auto loader = std::make_shared<gr::PluginLoader>(blocks, schedulers, directories);
    const auto failures = load_failures(*loader);
    if (!failures.empty()) {
        const auto& [path, error] = failures.front();
        const std::filesystem::path file(path);
        const auto others = failures.size() - 1U;
        throw PluginLoadError(std::format("plugin directory {} holds {}, which failed to load as a GNU Radio 4 plugin or "
                                          "block library: {}.{}",
                                          file.parent_path().string(),
                                          file.filename().string(),
                                          error,
                                          others == 0U ? std::string() : std::format(" {} more files failed.", others)));
    }
    if (loader->availableBlocks().empty()) {
        throw PluginLoadError(std::format("GNU Radio 4 plugin loader found no registered blocks in {}", joined(directories)));
    }
    return loader;
}
#endif

}  // namespace gr4cp::runtime
