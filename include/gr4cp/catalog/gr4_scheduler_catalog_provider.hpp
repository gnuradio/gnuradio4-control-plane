#pragma once

#include <memory>
#include <vector>

#include "gr4cp/catalog/scheduler_catalog_provider.hpp"

namespace gr {
class PluginLoader;
}

namespace gr4cp::catalog {

class Gr4SchedulerCatalogProvider final : public SchedulerCatalogProvider {
public:
    explicit Gr4SchedulerCatalogProvider(std::shared_ptr<gr::PluginLoader> loader);

    std::vector<domain::SchedulerDescriptor> list() const override;

private:
    std::shared_ptr<gr::PluginLoader> loader_;
};

}  // namespace gr4cp::catalog
