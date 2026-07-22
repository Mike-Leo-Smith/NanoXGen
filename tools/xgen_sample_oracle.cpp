#include <xgen/src/xggenerator/XgSamples.h>

#include "nanoxgen/xgen_expression.h"

#include <charconv>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::uint32_t parse(std::string_view text, const char *label) {
    std::uint32_t result{};
    const auto converted =
        std::from_chars(text.data(), text.data() + text.size(), result);
    if (converted.ec != std::errc{} ||
        converted.ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string{"invalid "} + label);
    }
    return result;
}

} // namespace

int main(int argc, char **argv) try {
    if (argc == 3 && std::string_view{argv[1]} == "--write-cpp") {
        std::ofstream output{argv[2], std::ios::binary | std::ios::trunc};
        if (!output) {
            throw std::runtime_error("cannot create C++ sample table");
        }
        output << "// Generated from the Maya 2027 public getSample API.\n"
                  "// Exact base doubles; device callers explicitly narrow to float.\n";
        for (std::uint32_t group = 0u; group < 16u; ++group) {
            output << "// group " << group << '\n';
            for (std::uint32_t count = 0u; count < 32768u; ++count) {
                double u{};
                double v{};
                getSample(count, 0u, group, u, v);
                output << "{0x" << std::hex << std::setw(16)
                       << std::setfill('0') << std::bit_cast<std::uint64_t>(u)
                       << "ull,0x" << std::setw(16)
                       << std::bit_cast<std::uint64_t>(v) << "ull},";
                if ((count & 1u) == 1u) { output << '\n'; }
            }
        }
        if (!output) {
            throw std::runtime_error("cannot write C++ sample table");
        }
        return 0;
    }
    if (argc == 5 && std::string_view{argv[1]} == "--group") {
        const std::uint32_t group = parse(argv[2], "group");
        const std::uint32_t description_id = parse(argv[3], "description ID");
        const std::uint32_t sample_count = parse(argv[4], "sample count");
        std::cout << std::setprecision(17);
        for (std::uint32_t count = 0u; count < sample_count; ++count) {
            double u{};
            double v{};
            getSample(count, description_id, group, u, v);
            std::cout << count << ' ' << u << ' ' << v << '\n';
        }
        return 0;
    }
    if (argc != 5) {
        throw std::runtime_error(
            "usage: nanoxgen_xgen_sample_oracle DESCRIPTION_ID PATCH_NAME "
            "FACE_ID SAMPLE_COUNT\n"
            "       nanoxgen_xgen_sample_oracle --group GROUP "
            "DESCRIPTION_ID SAMPLE_COUNT\n"
            "       nanoxgen_xgen_sample_oracle --write-cpp OUTPUT.inc");
    }
    const std::uint32_t description_id = parse(argv[1], "description ID");
    const std::string_view patch_name{argv[2]};
    const std::uint32_t face_id = parse(argv[3], "face ID");
    const std::uint32_t sample_count = parse(argv[4], "sample count");
    const std::uint32_t group = static_cast<std::uint32_t>(
        nanoxgen::xgen_face_seed(description_id, patch_name, face_id) * 1600.0);
    std::cout << std::setprecision(17);
    for (std::uint32_t count = 0u; count < sample_count; ++count) {
        double u{};
        double v{};
        getSample(count, description_id, group, u, v);
        std::cout << count << ' ' << u << ' ' << v << '\n';
    }
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
