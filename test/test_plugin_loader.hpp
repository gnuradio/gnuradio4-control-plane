#pragma once

#include "gr4cp/runtime/plugin_loader.hpp"

#include <memory>

namespace gr4cp::test {

// Returns one loader that every case of a test binary shares, built by load_plugins() on first use.
inline std::shared_ptr<gr::PluginLoader> plugin_loader() {
    static const std::shared_ptr<gr::PluginLoader> loader = runtime::load_plugins();
    return loader;
}

}  // namespace gr4cp::test
