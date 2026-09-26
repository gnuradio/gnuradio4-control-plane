#pragma once

#include <memory>
#include <vector>

#include "gr4cp/catalog/block_catalog_provider.hpp"

namespace gr {
class PluginLoader;
}

namespace gr4cp::catalog {

class Gr4BlockCatalogProvider final : public BlockCatalogProvider {
public:
    explicit Gr4BlockCatalogProvider(std::shared_ptr<gr::PluginLoader> loader);

    std::vector<domain::BlockDescriptor> list() const override;

private:
    std::shared_ptr<gr::PluginLoader> loader_;
};

}  // namespace gr4cp::catalog
