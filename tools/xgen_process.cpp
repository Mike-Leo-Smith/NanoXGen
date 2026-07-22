#include "nanoxgen/xgen.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace nanoxgen;

namespace {

float parse_float(const char *text, const char *name) {
    std::size_t used = 0u;
    const float value = std::stof(text, &used);
    if (text[used] != '\0' || !std::isfinite(value)) {
        throw std::invalid_argument(std::string{"invalid "} + name);
    }
    return value;
}

std::uint64_t parse_u64(const char *text, const char *name) {
    if (text[0] == '-') {
        throw std::invalid_argument(std::string{"invalid "} + name);
    }
    std::size_t used = 0u;
    const unsigned long long value = std::stoull(text, &used);
    if (text[used] != '\0') {
        throw std::invalid_argument(std::string{"invalid "} + name);
    }
    return static_cast<std::uint64_t>(value);
}

} // namespace

int main(int argc, char **argv) try {
    if (argc < 3) {
        std::cerr << "usage: nanoxgen_xgen_process <input.xgen> <output.xgen> "
                     "[--translate x y z] [--length-scale value] [--width-scale value] "
                     "[--rebuild] [--group-bytes value] [--deflate-level 0..9]\n";
        return 2;
    }
    XGenProcessParams process{};
    XGenBuildOptions build{};
    bool rebuild = false;
    for (int index = 3; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--translate") {
            if (index + 3 >= argc) {
                throw std::invalid_argument("--translate requires x y z");
            }
            process.translation = {
                parse_float(argv[++index], "translation x"),
                parse_float(argv[++index], "translation y"),
                parse_float(argv[++index], "translation z")};
        } else if (argument == "--length-scale") {
            if (++index >= argc) { throw std::invalid_argument("missing length scale"); }
            process.length_scale = parse_float(argv[index], "length scale");
        } else if (argument == "--width-scale") {
            if (++index >= argc) { throw std::invalid_argument("missing width scale"); }
            process.width_scale = parse_float(argv[index], "width scale");
        } else if (argument == "--group-bytes") {
            if (++index >= argc) { throw std::invalid_argument("missing group byte target"); }
            build.target_group_bytes = parse_u64(argv[index], "group byte target");
            rebuild = true;
        } else if (argument == "--deflate-level") {
            if (++index >= argc) { throw std::invalid_argument("missing deflate level"); }
            const std::uint64_t level = parse_u64(argv[index], "deflate level");
            if (level > 9u) { throw std::invalid_argument("deflate level must be 0..9"); }
            build.group_deflate_level = static_cast<std::uint32_t>(level);
            rebuild = true;
        } else if (argument == "--rebuild") {
            rebuild = true;
        } else {
            throw std::invalid_argument("unknown option: " + argument);
        }
    }
    XGenDocument document = load_xgen_document(argv[1]);
    const XGenEvaluatedCurves summary = materialize_xgen_curves(document);
    const std::size_t curve_count = summary.point_counts.size();
    const std::size_t point_count = summary.positions.size();
    if (rebuild) {
        XGenEvaluatedCurves curves = materialize_xgen_curves(document);
        process_xgen_curves(curves, process);
        document = build_xgen_document(curves, build);
    } else {
        process_xgen_document(document, process);
    }
    save_xgen_document(document, argv[2]);
    std::cout << "{\"curves\":" << curve_count
              << ",\"points\":" << point_count
              << ",\"rebuilt\":" << (rebuild ? "true" : "false")
              << ",\"autodesk_runtime\":false}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
