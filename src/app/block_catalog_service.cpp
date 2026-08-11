#include "gr4cp/app/block_catalog_service.hpp"

#include <algorithm>
#include <unordered_set>

#include "gr4cp/app/session_service.hpp"

namespace gr4cp::app {

BlockCatalogService::BlockCatalogService(const catalog::BlockCatalogProvider& provider) : provider_(provider) {}

std::vector<domain::BlockDescriptor> BlockCatalogService::list() const {
    std::lock_guard lock(mutex_);
    return cached_catalog().browse_blocks;
}

domain::BlockDescriptor BlockCatalogService::get(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto& catalog = cached_catalog();
    const auto it = catalog.exact_blocks.find(id);
    if (it == catalog.exact_blocks.end()) {
        throw NotFoundError("block not found: " + id);
    }
    return it->second;
}

namespace {

std::string canonical_catalog_key(const domain::BlockDescriptor& block) {
    if (block.canonical_type.has_value() && !block.canonical_type->empty()) {
        return *block.canonical_type;
    }
    return block.id;
}

bool is_canonical_implementation_entry(const domain::BlockDescriptor& block) {
    return block.canonical_type.has_value() && !block.canonical_type->empty() && block.id == *block.canonical_type;
}

bool is_better_browse_entry(const domain::BlockDescriptor& candidate, const domain::BlockDescriptor& current) {
    const auto candidate_is_impl = is_canonical_implementation_entry(candidate);
    const auto current_is_impl = is_canonical_implementation_entry(current);
    if (candidate_is_impl != current_is_impl) {
        return !candidate_is_impl;
    }
    if (candidate.id.size() != current.id.size()) {
        return candidate.id.size() < current.id.size();
    }
    if (candidate.category != current.category) {
        return candidate.category < current.category;
    }
    if (candidate.name != current.name) {
        return candidate.name < current.name;
    }
    return candidate.id < current.id;
}

}  // namespace

const BlockCatalogService::CachedCatalog& BlockCatalogService::cached_catalog() const {
    if (!cached_catalog_.has_value()) {
        auto blocks = provider_.list();
        std::sort(blocks.begin(), blocks.end(), [](const domain::BlockDescriptor& left, const domain::BlockDescriptor& right) {
            if (left.category != right.category) {
                return left.category < right.category;
            }
            if (left.name != right.name) {
                return left.name < right.name;
            }
            return left.id < right.id;
        });
        CachedCatalog catalog;
        std::unordered_set<std::string> browse_keys;
        for (const auto& block : blocks) {
            catalog.exact_blocks.insert_or_assign(block.id, block);
            const auto key = canonical_catalog_key(block);
            if (browse_keys.insert(key).second) {
                catalog.browse_blocks.push_back(block);
            } else {
                auto it = std::find_if(catalog.browse_blocks.begin(), catalog.browse_blocks.end(), [&](const auto& existing) {
                    return canonical_catalog_key(existing) == key;
                });
                if (it != catalog.browse_blocks.end() && is_better_browse_entry(block, *it)) {
                    *it = block;
                }
            }
        }
        std::sort(catalog.browse_blocks.begin(),
                  catalog.browse_blocks.end(),
                  [](const domain::BlockDescriptor& left, const domain::BlockDescriptor& right) {
                      if (left.category != right.category) {
                          return left.category < right.category;
                      }
                      if (left.name != right.name) {
                          return left.name < right.name;
                      }
                      return left.id < right.id;
                  });
        cached_catalog_ = std::move(catalog);
    }
    return *cached_catalog_;
}

}  // namespace gr4cp::app
