#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "gr4cp/runtime/block_settings.hpp"
#include "gr4cp/runtime/runtime_manager.hpp"
#include "gr4cp/storage/session_repository.hpp"

namespace gr4cp::app {

class TimeoutError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct BlockSettingsUpdateResult {
    std::string session_id;
    std::string block;
    std::string applied_via;
    bool accepted{true};
};

class BlockSettingsService {
public:
    using Json = nlohmann::json;

    BlockSettingsService(storage::SessionRepository& repository, runtime::RuntimeManager& runtime_manager);

    BlockSettingsUpdateResult update(const std::string& session_id,
                                     const std::string& block_unique_name,
                                     const Json& patch,
                                     runtime::BlockSettingsMode mode);
    Json get(const std::string& session_id, const std::string& block_unique_name);

private:
    domain::Session load_running_session_or_throw(const std::string& session_id) const;

    static gr::property_map json_to_property_map(const Json& value);
    static gr::pmt::Value json_to_pmt_value(const Json& value);
    static Json             property_map_to_json(const gr::property_map& value, bool exact = false);
    static Json             pmt_value_to_json(const gr::pmt::Value& value, bool exact = false);

    storage::SessionRepository& repository_;
    runtime::RuntimeManager& runtime_manager_;
};

}  // namespace gr4cp::app
