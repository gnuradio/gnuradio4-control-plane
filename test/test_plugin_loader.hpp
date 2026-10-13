#pragma once

#include "gr4cp/runtime/plugin_loader.hpp"

#include <memory>

namespace gr4cp::test {

// One loader for every case of a test binary, built on first use as the server builds its own at startup.
inline std::shared_ptr<gr::PluginLoader> plugin_loader() {
    static const std::shared_ptr<gr::PluginLoader> loader = runtime::load_plugins();
    return loader;
}

}  // namespace gr4cp::test
