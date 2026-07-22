#include "nanoxgen/curve_cache.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;

namespace {

struct Summary {
    double minimum{};
    double median{};
    double p90{};
};

Summary summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const std::size_t p90 = std::min(samples.size() - 1u,
        static_cast<std::size_t>(std::ceil(samples.size() * 0.9)) - 1u);
    return {samples.front(), samples[samples.size() / 2u], samples[p90]};
}

} // namespace

int main(int argc, char **argv) try {
    std::uint32_t repeats = 7u;
    std::vector<std::filesystem::path> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--repeats") {
            if (++i >= argc) { throw std::invalid_argument("missing repeat count"); }
            repeats = static_cast<std::uint32_t>(std::stoul(argv[i]));
        } else {
            paths.emplace_back(argument);
        }
    }
    if (repeats == 0u || paths.empty()) {
        throw std::invalid_argument("provide cache paths and a positive repeat count");
    }
    std::cout << std::setprecision(9) << "{\n  \"repeats\": " << repeats
              << ",\n  \"cases\": [\n";
    double checksum = 0.0;
    for (std::size_t file = 0u; file < paths.size(); ++file) {
        (void)load_curve_cache(paths[file]);
        std::vector<double> samples;
        samples.reserve(repeats);
        CurveCacheHeader header{};
        for (std::uint32_t repeat = 0u; repeat < repeats; ++repeat) {
            const auto begin = std::chrono::steady_clock::now();
            const CurveCache cache = load_curve_cache(paths[file]);
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
            const CurveCacheView view = cache.view();
            header = view.header();
            checksum += view.points()[(repeat * 17u) % header.point_count].radius;
        }
        const Summary timing = summarize(std::move(samples));
        std::cout << "    {\"file\": \"" << paths[file].filename().string()
                  << "\", \"bytes\": " << header.byte_size
                  << ", \"curves\": " << header.strand_count
                  << ", \"points\": " << header.point_count
                  << ", \"read_validate_ms\": {\"min\": " << timing.minimum
                  << ", \"median\": " << timing.median
                  << ", \"p90\": " << timing.p90 << "}}"
                  << (file + 1u == paths.size() ? "\n" : ",\n");
    }
    std::cout << "  ],\n  \"checksum\": " << checksum << "\n}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
