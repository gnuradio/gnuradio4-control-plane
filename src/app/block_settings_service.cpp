#include "gr4cp/app/block_settings_service.hpp"

#include <limits>
#include <string>
#include <type_traits>

#include "gr4cp/app/session_service.hpp"

namespace gr4cp::app {

BlockSettingsService::BlockSettingsService(storage::SessionRepository& repository,
                                           runtime::RuntimeManager& runtime_manager)
    : repository_(repository), runtime_manager_(runtime_manager) {}

BlockSettingsUpdateResult BlockSettingsService::update(const std::string& session_id,
                                                       const std::string& block_unique_name,
                                                       const Json& patch,
                                                       runtime::BlockSettingsMode mode) {
    const auto session = load_running_session_or_throw(session_id);
    const auto property_map = json_to_property_map(patch);

    try {
        runtime_manager_.set_block_settings(session, block_unique_name, property_map, mode);
    } catch (const runtime::BlockNotFoundError& error) {
        throw NotFoundError(error.what());
    } catch (const runtime::ReplyTimeoutError& error) {
        throw TimeoutError(error.what());
    } catch (const std::exception& error) {
        throw RuntimeError(error.what());
    }

    return BlockSettingsUpdateResult{
        .session_id = session.id,
        .block = block_unique_name,
        .applied_via = std::string(runtime::to_string(mode)),
        .accepted = true,
    };
}

BlockSettingsService::Json BlockSettingsService::get(const std::string& session_id,
                                                     const std::string& block_unique_name) {
    const auto stored = repository_.get(session_id);
    if (!stored) {
        throw NotFoundError("session not found: " + session_id);
    }
    const auto session = *stored;
    if (session.state != domain::SessionState::Running && !session.execution_id) {
        throw InvalidStateError("session has no terminal execution: " + session_id);
    }

    try {
        auto settings = runtime_manager_.get_block_settings(session, block_unique_name);
        return property_map_to_json(settings, settings.contains("__gr4cp_terminal"));
    } catch (const runtime::BlockNotFoundError& error) {
        throw NotFoundError(error.what());
    } catch (const runtime::ReplyTimeoutError& error) {
        throw TimeoutError(error.what());
    } catch (const std::exception& error) {
        throw RuntimeError(error.what());
    }
}

domain::Session BlockSettingsService::load_running_session_or_throw(const std::string& session_id) const {
    const auto session = repository_.get(session_id);
    if (!session.has_value()) {
        throw NotFoundError("session not found: " + session_id);
    }
    if (session->state != domain::SessionState::Running) {
        throw InvalidStateError("session not running: " + session_id);
    }
    return *session;
}

gr::property_map BlockSettingsService::json_to_property_map(const Json& value) {
    if (!value.is_object()) {
        throw ValidationError("settings body must be a JSON object");
    }

    gr::property_map result;
    for (const auto& [key, item] : value.items()) {
        result.insert_or_assign(std::pmr::string(key), json_to_pmt_value(item));
    }
    return result;
}

gr::pmt::Value BlockSettingsService::json_to_pmt_value(const Json& value) {
    if (value.is_null()) {
        return gr::pmt::Value{};
    }
    if (value.is_boolean()) {
        return value.get<bool>();
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        const auto raw = value.get<std::uint64_t>();
        if (raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw ValidationError("unsigned integer settings values must fit in int64");
        }
        return static_cast<std::int64_t>(raw);
    }
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_object()) {
        return json_to_property_map(value);
    }
    if (value.is_array()) {
        throw ValidationError("settings arrays are not supported");
    }
    throw ValidationError("unsupported JSON value in settings body");
}

BlockSettingsService::Json BlockSettingsService::property_map_to_json(const gr::property_map& value, bool exact) {
    auto result = Json::object();
    for (const auto& [key, item] : value) {
        result[std::string(key)] = pmt_value_to_json(item, exact);
    }
    return result;
}

BlockSettingsService::Json BlockSettingsService::pmt_value_to_json(const gr::pmt::Value& value, bool exact) {
    Json result;
    bool converted = false;

    gr::pmt::ValueVisitor([&result, &converted, exact](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::same_as<T, std::monostate>) {
            result = nullptr;
            converted = true;
        } else if constexpr (std::same_as<T, bool>) {
            result = item;
            converted = true;
        } else if constexpr ((std::integral<T> && !std::same_as<T, bool>) || std::same_as<T, std::uint64_t>) {
            if constexpr (std::same_as<T, std::uint64_t> || std::same_as<T, std::int64_t>) {
                if (exact) {
                    result = std::to_string(item);
                } else {
                    result = item;
                }
            } else {
                result = item;
            }
            converted = true;
        } else if constexpr (std::floating_point<T>) {
            result = item;
            converted = true;
        } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::pmr::string>) {
            result = std::string(item);
            converted = true;
        } else if constexpr (std::same_as<T, std::string_view>) {
            result = std::string(item);
            converted = true;
        } else if constexpr (std::same_as<T, gr::property_map>) {
            result    = property_map_to_json(item, exact);
            converted = true;
        }
    }).visit(value);

    if (!converted) {
        throw RuntimeError("runtime returned unsupported settings value");
    }

    return result;
}

}  // namespace gr4cp::app
