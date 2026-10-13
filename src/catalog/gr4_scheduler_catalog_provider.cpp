#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include <gnuradio-4.0/PluginLoader.hpp>

#include "gr4cp/runtime/gr4_runtime_manager.hpp"

namespace gr4cp::catalog {

namespace {

bool is_alias_scheduler_id(const std::string& id) {
    return id.find('<') == std::string::npos;
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

}  // namespace

Gr4SchedulerCatalogProvider::Gr4SchedulerCatalogProvider(std::shared_ptr<gr::PluginLoader> loader) : loader_(std::move(loader)) {}

// The loader's schedulers that carry no template arguments, and each package alias whose core type the loader offers.
// The listing registers the aliases into the global scheduler registry, where a document's nested scheduler finds them.
std::vector<domain::SchedulerDescriptor> Gr4SchedulerCatalogProvider::list() const {
    throw_on_failed_plugins(*loader_);
    runtime::register_scheduler_aliases();

    const auto available = loader_->availableSchedulers();
    std::vector<std::string> scheduler_ids;
    std::ranges::copy_if(available, std::back_inserter(scheduler_ids), is_alias_scheduler_id);
    for (const auto& [alias, type] : runtime::scheduler_aliases()) {
        if (std::ranges::binary_search(available, type)) {
            scheduler_ids.emplace_back(alias);
        }
    }
    std::ranges::sort(scheduler_ids);
    const auto duplicates = std::ranges::unique(scheduler_ids);
    scheduler_ids.erase(duplicates.begin(), duplicates.end());
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
