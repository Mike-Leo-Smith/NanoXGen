#pragma once

#include <cctype>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace nanoxgen::detail {

inline bool is_classic_path_separator(char character) noexcept {
    return character == '/' || character == '\\';
}

// Authored XGen packages commonly retain Windows separators after being
// relocated to Unix (and vice versa). Treat both characters as separators at
// the package boundary; do not rely on the host filesystem parser to
// reinterpret the foreign one.
inline std::filesystem::path classic_path(
    std::string_view value) {
    std::string normalized{value};
    for (char &character : normalized) {
        if (is_classic_path_separator(character)) {
            character = std::filesystem::path::preferred_separator;
        }
    }
    return std::filesystem::path{std::move(normalized)};
}

inline std::string_view strip_classic_root_separators(
    std::string_view value) noexcept {
    while (!value.empty() && is_classic_path_separator(value.front())) {
        value.remove_prefix(1u);
    }
    return value;
}

inline bool classic_ascii_iequals(
    std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) { return false; }
    for (std::size_t index = 0u; index < lhs.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(lhs[index])) !=
            std::tolower(static_cast<unsigned char>(rhs[index]))) {
            return false;
        }
    }
    return true;
}

inline bool classic_windows_absolute(std::string_view value) noexcept {
    return value.size() >= 3u &&
        std::isalpha(static_cast<unsigned char>(value[0])) &&
        value[1] == ':' && is_classic_path_separator(value[2]);
}

inline bool classic_windows_drive_relative(std::string_view value) noexcept {
    return value.size() >= 2u &&
        std::isalpha(static_cast<unsigned char>(value[0])) &&
        value[1] == ':' && !classic_windows_absolute(value);
}

inline bool classic_unc_absolute(std::string_view value) noexcept {
    return value.size() >= 2u &&
        is_classic_path_separator(value[0]) &&
        is_classic_path_separator(value[1]);
}

inline bool classic_windows_root_relative(std::string_view value) noexcept {
#if defined(_WIN32)
    return !value.empty() && is_classic_path_separator(value.front()) &&
        !classic_unc_absolute(value);
#else
    return !value.empty() && value.front() == '\\' &&
        !classic_unc_absolute(value);
#endif
}

inline bool classic_absolute(std::string_view value) noexcept {
    return (!value.empty() && is_classic_path_separator(value.front())) ||
        classic_windows_absolute(value);
}

inline bool classic_safe_component(std::string_view value) noexcept {
    return !value.empty() && value != "." && value != ".." &&
        value.find('\0') == std::string_view::npos &&
        value.find_first_of("/\\") == std::string_view::npos;
}

inline bool classic_extension_equals(
    const std::filesystem::path &path, std::string_view extension) {
    return classic_ascii_iequals(path.extension().string(), extension);
}

inline std::optional<std::string_view>
classic_suffix_after_component(
    std::string_view value, std::string_view component) noexcept {
    if (component.empty()) { return std::nullopt; }
#if defined(_WIN32)
    constexpr bool host_case_insensitive = true;
#else
    constexpr bool host_case_insensitive = false;
#endif
    const bool case_insensitive = host_case_insensitive ||
        classic_windows_absolute(value) || classic_unc_absolute(value);
    std::optional<std::string_view> result;
    for (std::size_t begin = 0u; begin < value.size();) {
        while (begin < value.size() &&
               is_classic_path_separator(value[begin])) {
            ++begin;
        }
        const std::size_t end = value.find_first_of("/\\", begin);
        const std::size_t component_end =
            end == std::string_view::npos ? value.size() : end;
        const std::string_view candidate =
            value.substr(begin, component_end - begin);
        if ((case_insensitive &&
             classic_ascii_iequals(candidate, component)) ||
            (!case_insensitive && candidate == component)) {
            std::size_t suffix = component_end;
            while (suffix < value.size() &&
                   is_classic_path_separator(value[suffix])) {
                ++suffix;
            }
            result = value.substr(suffix);
        }
        if (end == std::string_view::npos) { break; }
        begin = end + 1u;
    }
    return result;
}

// Resolve data paths embedded in Classic expressions and module attributes.
// ${DESC} is always relative to the concrete description directory. When a
// relocated package retains a stale absolute path, rebase the suffix following
// the matching description directory component, but only when that candidate
// exists. This handles Windows-authored paths on Unix without interpreting
// "E:" as a Unix relative directory and leaves intentional external paths
// untouched when they are still valid.
inline std::filesystem::path resolve_classic_description_path(
    std::string_view value,
    const std::filesystem::path &description_directory) {
    constexpr std::string_view desc_token{"${DESC}"};
    if (value.starts_with(desc_token)) {
        value.remove_prefix(desc_token.size());
        return (description_directory /
                classic_path(strip_classic_root_separators(value)))
            .lexically_normal();
    }
    if (classic_windows_drive_relative(value)) {
        throw std::invalid_argument(
            "Classic sidecar path is drive-relative and ambiguous: " +
            std::string{value});
    }

    const std::filesystem::path authored =
        classic_path(value).lexically_normal();
    std::error_code error;
    if (!classic_absolute(value)) {
        const std::filesystem::path relative_to_description =
            (description_directory / authored).lexically_normal();
        return relative_to_description;
    }
#if defined(_WIN32)
    const bool foreign_windows_path =
        classic_windows_root_relative(value);
#else
    const bool foreign_windows_path =
        classic_windows_absolute(value) || classic_unc_absolute(value) ||
        classic_windows_root_relative(value);
#endif
    if (!foreign_windows_path &&
        std::filesystem::exists(authored, error) && !error) {
        return authored;
    }

    const std::string description_name =
        description_directory.filename().string();
    if (const auto suffix =
            classic_suffix_after_component(value, description_name)) {
        const std::filesystem::path relocated =
            (description_directory / classic_path(*suffix))
                .lexically_normal();
        error.clear();
        if (std::filesystem::exists(relocated, error) && !error) {
            return relocated;
        }
    }
#if !defined(_WIN32)
    if (foreign_windows_path) {
        throw std::invalid_argument(
            "Classic sidecar path uses an unresolved foreign Windows root: " +
            std::string{value});
    }
#endif
    if (classic_windows_root_relative(value)) {
        throw std::invalid_argument(
            "Classic sidecar path is root-relative and ambiguous: " +
            std::string{value});
    }
    return authored;
}

} // namespace nanoxgen::detail
