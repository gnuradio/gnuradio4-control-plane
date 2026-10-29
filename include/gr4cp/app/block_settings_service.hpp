#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "gr4cp/app/session_service.hpp"
#include "gr4cp/runtime/block_settings.hpp"
#include "gr4cp/runtime/runtime_manager.hpp"

namespace gr4cp::app {

struct BlockSettingsUpdateResult {
    std::string session_id;
    std::string block;
    std::string applied_via;
    bool accepted{true};
};

class BlockSettingsService {
public:
    using Json = nlohmann::json;

    // Reads each session through `sessions`. A run that ended by itself then reads as not running: an update is
    // refused, and a read answers with the settings the run left.
    BlockSettingsService(SessionService& sessions, runtime::RuntimeManager& runtime_manager);

    BlockSettingsUpdateResult update(const std::string& session_id,
                                     const std::string& block_unique_name,
                                     const Json& patch,
                                     runtime::BlockSettingsMode mode);
    Json get(const std::string& session_id, const std::string& block_unique_name);

private:
    domain::Session load_running_session_or_throw(const std::string& session_id);

    static gr::property_map json_to_property_map(const Json& value);
    static gr::pmt::Value json_to_pmt_value(const Json& value);
    static Json property_map_to_json(const gr::property_map& value);
    static Json pmt_value_to_json(const gr::pmt::Value& value);

    SessionService& sessions_;
    runtime::RuntimeManager& runtime_manager_;
};

}  // namespace gr4cp::app
