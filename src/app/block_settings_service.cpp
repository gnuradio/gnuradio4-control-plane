#include "gr4cp/app/block_settings_service.hpp"

#include <complex>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "gr4cp/app/session_service.hpp"

namespace gr4cp::app {

namespace {

template <typename T>
struct is_tensor : std::false_type {};

template <typename T>
struct is_tensor<gr::Tensor<T>> : std::true_type {};

template <typename T>
struct is_complex : std::false_type {};

template <typename T>
struct is_complex<std::complex<T>> : std::true_type {};

// A complex value reads and writes as an object of its two parts.
constexpr std::string_view kRealPart = "real";
constexpr std::string_view kImaginaryPart = "imag";

bool is_complex_object(const nlohmann::json& value) {
    return value.is_object() && value.size() == 2 && value.contains(kRealPart) && value.contains(kImaginaryPart) &&
           value[kRealPart].is_number() && value[kImaginaryPart].is_number();
}

// Converts a scalar, a complex number or a string to JSON. An empty value becomes null. A 64-bit integer becomes its
// decimal string, which a client that parses every JSON number as a double reads exactly.
template <typename T>
nlohmann::json scalar_to_json(const T& item) {
    if constexpr (std::same_as<T, std::monostate>) {
        return nullptr;
    } else if constexpr (std::same_as<T, std::int64_t> || std::same_as<T, std::uint64_t>) {
        return std::to_string(item);
    } else if constexpr (is_complex<T>::value) {
        return nlohmann::json{{kRealPart, item.real()}, {kImaginaryPart, item.imag()}};
    } else if constexpr (std::same_as<T, std::string> || std::same_as<T, std::pmr::string> ||
                         std::same_as<T, std::string_view>) {
        return std::string(item);
    } else {
        return item;
    }
}

}  // namespace

BlockSettingsService::BlockSettingsService(SessionService& sessions, runtime::RuntimeManager& runtime_manager)
    : sessions_(sessions), runtime_manager_(runtime_manager) {}

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
    } catch (const runtime::SessionNotRunningError& error) {
        throw InvalidStateError(error.what());
    } catch (const runtime::SettingsRefusedError& error) {
        throw ValidationError(error.what());
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
    const auto session = sessions_.get(session_id);

    try {
        return property_map_to_json(runtime_manager_.get_block_settings(session, block_unique_name));
    } catch (const runtime::BlockNotFoundError& error) {
        throw NotFoundError(error.what());
    } catch (const runtime::SessionNotRunningError& error) {
        throw InvalidStateError(error.what());
    } catch (const std::exception& error) {
        throw RuntimeError(error.what());
    }
}

domain::Session BlockSettingsService::load_running_session_or_throw(const std::string& session_id) {
    auto session = sessions_.get(session_id);
    if (session.state != domain::SessionState::Running) {
        throw InvalidStateError("session not running: " + session_id);
    }
    return session;
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
    if (is_complex_object(value)) {
        return std::complex<double>(value[kRealPart].get<double>(), value[kImaginaryPart].get<double>());
    }
    if (value.is_object()) {
        return json_to_property_map(value);
    }
    if (value.is_array()) {
        // the block converts each element to its setting's element type, and refuses an element that does not convert
        gr::Tensor<gr::pmt::Value> elements;
        elements.reserve(value.size());
        for (const auto& element : value) {
            elements.push_back(json_to_pmt_value(element));
        }
        return elements;
    }
    throw ValidationError("unsupported JSON value in settings body");
}

BlockSettingsService::Json BlockSettingsService::property_map_to_json(const gr::property_map& value) {
    auto result = Json::object();
    for (const auto& [key, item] : value) {
        result[std::string(key)] = pmt_value_to_json(item);
    }
    return result;
}

BlockSettingsService::Json BlockSettingsService::pmt_value_to_json(const gr::pmt::Value& value) {
    Json result;
    gr::pmt::ValueVisitor([&result](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (is_tensor<T>::value) {
            result = Json::array();
            for (const auto& element : item) {
                if constexpr (std::same_as<typename T::value_type, gr::pmt::Value>) {
                    result.push_back(pmt_value_to_json(element));
                } else {
                    result.push_back(scalar_to_json(static_cast<typename T::value_type>(element)));
                }
            }
        } else if constexpr (std::same_as<T, gr::property_map>) {
            result = property_map_to_json(item);
        } else {
            result = scalar_to_json(item);
        }
    }).visit(value);
    return result;
}

}  // namespace gr4cp::app
