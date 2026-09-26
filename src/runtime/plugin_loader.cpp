#include "gr4cp/runtime/plugin_loader.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gnuradio-4.0/PluginLoader.hpp>
#if !defined(__EMSCRIPTEN__)
#include <gnuradio-4.0/BlockLookup.hpp>
#endif

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

std::string joined(std::span<const std::string> directories) {
    std::string text;
    for (const auto& directory : directories) {
        text += text.empty() ? directory : ":" + directory;
    }
    return text.empty() ? "<none>" : text;
}

}  // namespace

std::vector<std::string> plugin_directories(std::string_view configured_default) {
    std::vector<std::string> directories;
#if !defined(__EMSCRIPTEN__)
    for (auto& directory : gr::tools::searchDirectories({}, {})) {
        directories.push_back(std::move(directory.path));
    }
#endif
    for (const auto part : std::views::split(configured_default, ':')) {
        std::string directory(std::string_view(part.begin(), part.end()));
        if (!directory.empty() && std::ranges::find(directories, directory) == directories.end()) {
            directories.push_back(std::move(directory));
        }
    }
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

    auto loader = std::make_shared<gr::PluginLoader>(blocks, schedulers, directories);
    if (!loader->failedDirectories().empty()) {
        const auto& [path, error] = *loader->failedDirectories().begin();
        throw PluginLoadError(std::format("cannot read GNU Radio 4 plugin directory ({}: {})", path, error));
    }
    if (!loader->failedPlugins().empty()) {
        const auto& [path, error] = *std::ranges::min_element(loader->failedPlugins(), {}, [](const auto& failure) {
            return failure.first;
        });
        const std::filesystem::path file(path);
        const auto others = loader->failedPlugins().size() - 1U;
        throw PluginLoadError(std::format("plugin directory {} holds {}, which failed to load as a GNU Radio 4 plugin or "
                                          "block library: {}. A plugin directory holds plugins and block libraries "
                                          "only.{}",
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
