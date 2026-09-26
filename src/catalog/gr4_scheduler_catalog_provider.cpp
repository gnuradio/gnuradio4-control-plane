#include "gr4cp/catalog/gr4_scheduler_catalog_provider.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <gnuradio-4.0/PluginLoader.hpp>
#include <gnuradio-4.0/Scheduler.hpp>

namespace gr4cp::catalog {

namespace {

bool is_alias_scheduler_id(const std::string& id) {
    return id.find('<') == std::string::npos;
}

void register_default_schedulers() {
    auto& registry = gr::globalSchedulerRegistry();
    (void)registry.insert<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::singleThreaded>>("=gr::scheduler::SimpleSingle");
    (void)registry.insert<gr::scheduler::Simple<gr::scheduler::ExecutionPolicy::multiThreaded>>("=gr::scheduler::SimpleMulti");
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

std::vector<domain::SchedulerDescriptor> Gr4SchedulerCatalogProvider::list() const {
    throw_on_failed_plugins(*loader_);
    register_default_schedulers();

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
