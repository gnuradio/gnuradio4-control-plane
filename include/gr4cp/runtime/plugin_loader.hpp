#pragma once

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gr {
class BlockRegistry;
class PluginLoader;
class SchedulerRegistry;
}

namespace gr4cp::runtime {

class PluginLoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The directories the program searches for blocks, in order: those of GNURADIO4_PLUGIN_DIRECTORIES, then those of
// the colon-separated `configured_default`. A directory named twice is searched once.
std::vector<std::string> plugin_directories(std::string_view configured_default);

// Returns the directories the program searches: plugin_directories() of the build's default, which is
// GR4CP_PLUGIN_DIRECTORIES or else gr::installedPluginDirectory(), followed by GR4CP_EXTRA_PLUGIN_DIRECTORIES. A
// WebAssembly build searches none.
std::vector<std::string> plugin_directories();

// The shared objects that `loader` failed to load, as file and reason, sorted by path. A shared object that opened,
// has no plugin entry point and registered nothing is not among them. A WebAssembly build returns an empty list: core
// builds it without plugins.
std::vector<std::pair<std::string, std::string>> load_failures(const gr::PluginLoader& loader);

// Builds a loader over the global registries and plugin_directories(). A native build throws as the overload below
// does. A WebAssembly build searches no directory and throws nothing: its blocks are linked into the program.
std::shared_ptr<gr::PluginLoader> load_plugins();

#if !defined(__EMSCRIPTEN__)
// Builds a loader over `directories` that registers into `blocks` and `schedulers`. A shared object that opens, has
// no plugin entry point and registers nothing is passed over. Throws PluginLoadError, its message one line, when none
// of the directories exists, one of them cannot be read, any other shared object in one of them fails to load, or the
// loader holds no block. The message for a shared object names its directory, its file and the loader's reason.
std::shared_ptr<gr::PluginLoader> load_plugins(std::span<const std::string> directories,
                                               gr::BlockRegistry& blocks,
                                               gr::SchedulerRegistry& schedulers);
#endif

}  // namespace gr4cp::runtime
