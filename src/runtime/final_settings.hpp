#pragma once

#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "gr4cp/runtime/block_settings.hpp"

namespace gr4cp::runtime::detail {

// An ended block's values and lookup identity, without a handle keeping the block alive.
struct CapturedBlock {
    std::string unique_name;
    std::string block_name;
    std::string type_name;
    gr::property_map settings;
    std::optional<std::string> capture_error;

    std::string_view uniqueName() const { return unique_name; }
    std::string_view name() const { return block_name; }
    std::string_view typeName() const { return type_name; }
    std::optional<gr::pmt::Value> get(const std::string& key) const {
        const auto it = settings.find(key);
        return it == settings.end() ? std::nullopt : std::optional<gr::pmt::Value>(it->second);
    }
    gr::property_map get() const {
        if (capture_error.has_value()) {
            throw std::runtime_error(*capture_error);
        }
        return settings;
    }
};

// Called only after the runtime has joined its workers and stop hooks. Capture failures remain explicit read
// errors for their blocks; other blocks still retain all their values.
template <typename Block>
auto capture_settings(const std::map<std::string, Block, std::less<>>& blocks) {
    std::map<std::string, CapturedBlock, std::less<>> captured;
    for (const auto& [unique_name, block] : blocks) {
        CapturedBlock snapshot{unique_name, std::string(block.name()), std::string(block.typeName()), {}, {}};
        try {
            snapshot.settings = block.get();
        } catch (const std::exception& error) {
            snapshot.capture_error = std::format("final settings capture failed for block {}: {}", unique_name, error.what());
        }
        captured.emplace(unique_name, std::move(snapshot));
    }
    return captured;
}

}  // namespace gr4cp::runtime::detail
