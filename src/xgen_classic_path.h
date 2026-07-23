#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace nanoxgen::detail {

// Authored XGen packages commonly retain Windows separators after being
// relocated to Unix (and vice versa). Treat both characters as separators at
// the package boundary; do not rely on the host filesystem parser to
// reinterpret the foreign one.
inline std::filesystem::path classic_path(
    std::string_view value) {
    std::string normalized{value};
    for (char &character : normalized) {
        if (character == '/' || character == '\\') {
            character = std::filesystem::path::preferred_separator;
        }
    }
    return std::filesystem::path{std::move(normalized)};
}

inline std::string_view strip_classic_root_separators(
    std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == '/' || value.front() == '\\')) {
        value.remove_prefix(1u);
    }
    return value;
}

} // namespace nanoxgen::detail
