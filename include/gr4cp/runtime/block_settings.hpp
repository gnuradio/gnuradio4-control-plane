#pragma once

#include <stdexcept>
#include <string_view>

#include <gnuradio-4.0/Tag.hpp>

namespace gr4cp::runtime {

enum class BlockSettingsMode {
    Staged,
    Immediate,
};

inline constexpr std::string_view to_string(BlockSettingsMode mode) noexcept {
    switch (mode) {
    case BlockSettingsMode::Staged: return "staged_settings";
    case BlockSettingsMode::Immediate: return "settings";
    }
    return "settings";
}

class BlockNotFoundError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A block refused a settings patch: a key it does not accept, or a value it does not take. Nothing was staged.
class SettingsRefusedError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace gr4cp::runtime
