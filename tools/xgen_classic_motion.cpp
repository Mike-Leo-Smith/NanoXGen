#include "nanoxgen/context.h"
#include "nanoxgen/xgen_classic_collection.h"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Clock = std::chrono::steady_clock;

double parse_double(std::string_view text, const char *label) {
    std::size_t consumed{};
    const double value = std::stod(std::string{text}, &consumed);
    if (consumed != text.size() || !std::isfinite(value)) {
        throw std::invalid_argument(std::string{label} + " is invalid");
    }
    return value;
}

std::uint32_t parse_u32(std::string_view text, const char *label) {
    std::size_t consumed{};
    const unsigned long value = std::stoul(std::string{text}, &consumed);
    if (consumed != text.size() ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string{label} + " is invalid");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint64_t mix(std::uint64_t hash, std::uint32_t value) {
    hash ^= value;
    return hash * 1099511628211ull;
}

} // namespace

int main(int argc, char **argv) try {
    if (argc < 10 || ((argc - 8) % 2) != 0) {
        throw std::invalid_argument(
            "usage: nanoxgen_xgen_classic_motion COLLECTION.xgen PATCHES.abc "
            "DESCRIPTIONS_ROOT FRAME FPS THREADS EFFECT_COUNT "
            "LOOKUP PLACEMENT [LOOKUP PLACEMENT]...");
    }
    const std::filesystem::path collection_path = argv[1];
    const std::filesystem::path archive_path = argv[2];
    const std::filesystem::path descriptions_root = argv[3];
    nanoxgen::ClassicMotionSampling sampling{};
    sampling.frame = parse_double(argv[4], "frame");
    sampling.frames_per_second = parse_double(argv[5], "FPS");
    const std::uint32_t threads = parse_u32(argv[6], "threads");
    const std::uint32_t effects = parse_u32(argv[7], "effect count");
    for (int index = 8; index < argc; index += 2) {
        sampling.lookup_offsets.push_back(
            parse_double(argv[index], "lookup"));
        sampling.placements.push_back(static_cast<float>(
            parse_double(argv[index + 1], "placement")));
    }
    nanoxgen::NanoXGenContext context{
        threads == 0u ? nanoxgen::available_worker_count() : threads};
    nanoxgen::ClassicCollectionExecutionOptions options{};
    options.effect_count = effects;
    options.context = &context;
    const Clock::time_point begin = Clock::now();
    const nanoxgen::ClassicCollectionMotionExecutionPlan plan =
        nanoxgen::build_xgen_classic_collection_motion_execution_plan(
            collection_path, archive_path, descriptions_root,
            sampling, options);
    const Clock::time_point end = Clock::now();
    std::uint64_t strands{};
    std::uint64_t points{};
    std::uint64_t unique_deformations{};
    std::uint64_t checksum = 1469598103934665603ull;
    for (const auto &description : plan.descriptions) {
        for (std::size_t sample_index = 0u;
             sample_index < description.samples.size(); ++sample_index) {
            const auto &sample = description.samples[sample_index];
            if (sample.deformation_source_index == sample_index) {
                ++unique_deformations;
            }
            const auto &deformation =
                nanoxgen::resolve_xgen_classic_motion_deformation(
                    description, sample_index);
            strands += deformation.roots.roots.size();
            points += deformation.roots.roots.size() *
                description.runtime.fx_cv_count;
            for (const nanoxgen::RootSample root :
                 deformation.roots.roots) {
                checksum = mix(
                    checksum, std::bit_cast<std::uint32_t>(
                                  root.position.x));
                checksum = mix(
                    checksum, std::bit_cast<std::uint32_t>(
                                  root.position.y));
                checksum = mix(
                    checksum, std::bit_cast<std::uint32_t>(
                                  root.position.z));
            }
        }
    }
    const double elapsed =
        std::chrono::duration<double, std::milli>(end - begin).count();
    std::cout << std::setprecision(9)
              << "{\"description_count\":" << plan.descriptions.size()
              << ",\"motion_samples\":" << sampling.lookup_offsets.size()
              << ",\"unique_deformations\":" << unique_deformations
              << ",\"sample_strands\":" << strands
              << ",\"sample_points\":" << points
              << ",\"context_workers\":" << plan.context_worker_count
              << ",\"host_motion_plan_ms\":" << elapsed
              << ",\"checksum\":" << checksum
              << ",\"fallback_count\":0}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
