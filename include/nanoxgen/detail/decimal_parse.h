#pragma once

#include <charconv>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>

namespace nanoxgen::detail {

struct DecimalParseResult {
    const char *ptr{};
    std::errc ec{};
};

// Apple libc++ shipped integer from_chars before its floating-point overloads.
// Keep the allocation-free standard implementation where it is available and
// use a locale-independent compatibility path for libc++ builds.
template <typename Float>
DecimalParseResult parse_decimal(
    const char *first, const char *last, Float &value) {
    static_assert(std::is_floating_point_v<Float>);
#if defined(_LIBCPP_VERSION)
    if (first == last || *first == '+' ||
        *first == ' ' || *first == '\t' || *first == '\n' ||
        *first == '\r' || *first == '\f' || *first == '\v') {
        return {first, std::errc::invalid_argument};
    }
    std::istringstream stream{std::string{first, last}};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    Float parsed{};
    stream >> parsed;
    if (stream.fail()) {
        return {first, std::errc::invalid_argument};
    }
    const std::streampos position =
        stream.rdbuf()->pubseekoff(0, std::ios_base::cur, std::ios_base::in);
    if (position == std::streampos{-1}) {
        return {first, std::errc::invalid_argument};
    }
    value = parsed;
    return {first + static_cast<std::size_t>(position), std::errc{}};
#else
    const auto result =
        std::from_chars(first, last, value, std::chars_format::general);
    return {result.ptr, result.ec};
#endif
}

} // namespace nanoxgen::detail
