#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <gnuradio-4.0/PluginLoader.hpp>

#include "gr4cp/runtime/gr4_runtime_manager.hpp"
#include "gr4cp/runtime/plugin_loader.hpp"

namespace gr4cp::catalog {

namespace {

bool is_alias_scheduler_id(const std::string& id) {
    return id.find('<') == std::string::npos;
}

void throw_on_failed_plugins(const gr::PluginLoader& loader) {
    if (const auto failures = runtime::load_failures(loader); !failures.empty()) {
        const auto& [path, error] = failures.front();
        throw SchedulerCatalogLoadError("failed to load GNU Radio 4 plugin library (" + path + ": " + error + ")");
    }
}

}  // namespace

Gr4SchedulerCatalogProvider::Gr4SchedulerCatalogProvider(std::shared_ptr<gr::PluginLoader> loader) : loader_(std::move(loader)) {}

// The loader's schedulers that carry no template arguments, the package's aliases among them. The listing registers
// the aliases into the global scheduler registry, where a document's nested scheduler finds them.
std::vector<domain::SchedulerDescriptor> Gr4SchedulerCatalogProvider::list() const {
    throw_on_failed_plugins(*loader_);
    runtime::register_scheduler_aliases();

    auto scheduler_ids = loader_->availableSchedulers();
    std::erase_if(scheduler_ids, [](const auto& id) { return !is_alias_scheduler_id(id); });
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
