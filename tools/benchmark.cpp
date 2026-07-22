#include "nanoxgen/asset.h"
#include "nanoxgen/curve_payload.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace nanoxgen;

namespace {

Asset make_asset() {
    AssetBuildInput input{};
    input.positions = {{-5.0f, 0.0f, -5.0f}, {5.0f, 0.0f, -5.0f},
                       {5.0f, 0.0f, 5.0f}, {-5.0f, 0.0f, 5.0f}};
    input.normals.assign(4u, {0.0f, 1.0f, 0.0f});
    input.texcoords = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    input.triangles = {{0u, 1u, 2u}, {0u, 2u, 3u}};
    for (const Vec3 root : std::vector<Vec3>{{-4.0f, 0.0f, -4.0f}, {4.0f, 0.0f, -4.0f},
                                             {4.0f, 0.0f, 4.0f}, {-4.0f, 0.0f, 4.0f}}) {
        GuideInput guide{};
        guide.root_normal = {0.0f, 1.0f, 0.0f};
        guide.support_radius = 12.0f;
        for (std::uint32_t cv = 0u; cv < 8u; ++cv) {
            const float t = static_cast<float>(cv) / 7.0f;
            guide.cvs.push_back(root + Vec3{0.15f * t * t, 1.0f * t, 0.08f * t * t});
        }
        input.guides.push_back(std::move(guide));
    }
    return build_asset(input);
}

std::vector<LinearCurveSeed> make_linear_seeds(std::uint32_t count) {
    std::vector<LinearCurveSeed> seeds;
    seeds.reserve(count);
    for (std::uint32_t i = 0u; i < count; ++i) {
        const float u = static_cast<float>((i * 2654435761u) >> 8u) / 16777216.0f;
        const float v = static_cast<float>((i * 2246822519u) >> 8u) / 16777216.0f;
        const Vec3 root{-5.0f + 10.0f * u, 0.0f, -5.0f + 10.0f * v};
        seeds.push_back({root, root + Vec3{0.0f, 1.0f, 0.0f}, {u, v}, 0.02f});
    }
    return seeds;
}

struct Timing {
    double minimum_ms{};
    double median_ms{};
    double p90_ms{};
    double checksum{};
};

template<typename Function>
Timing measure(std::uint32_t repeats, Function &&function) {
    function();
    std::vector<double> samples;
    samples.reserve(repeats);
    double checksum = 0.0;
    for (std::uint32_t repeat = 0u; repeat < repeats; ++repeat) {
        const auto begin = std::chrono::steady_clock::now();
        const GeneratedCurves curves = function();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        checksum += curves.points[repeat % curves.points.size()].y;
        checksum += curves.widths[(repeat * 17u) % curves.widths.size()];
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t p90 = std::min(samples.size() - 1u,
        static_cast<std::size_t>(std::ceil(samples.size() * 0.9)) - 1u);
    return {samples.front(), samples[samples.size() / 2u], samples[p90], checksum};
}

template<typename Function>
Timing measure_payload(std::uint32_t repeats, Function &&function) {
    function();
    std::vector<double> samples;
    samples.reserve(repeats);
    double checksum = 0.0;
    for (std::uint32_t repeat = 0u; repeat < repeats; ++repeat) {
        const auto begin = std::chrono::steady_clock::now();
        const std::vector<CurveBatch> batches = function();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        const CurveBatch &batch = batches[repeat % batches.size()];
        checksum += batch.points[(repeat * 17u) % batch.points.size()].radius;
        checksum += batch.root_uvs[(repeat * 31u) % batch.root_uvs.size()].x;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t p90 = std::min(samples.size() - 1u,
        static_cast<std::size_t>(std::ceil(samples.size() * 0.9)) - 1u);
    return {samples.front(), samples[samples.size() / 2u], samples[p90], checksum};
}

template<typename Function>
Timing measure_packed(std::uint32_t repeats, Function &&function) {
    function();
    std::vector<double> samples;
    samples.reserve(repeats);
    double checksum = 0.0;
    for (std::uint32_t repeat = 0u; repeat < repeats; ++repeat) {
        const auto begin = std::chrono::steady_clock::now();
        const PackedGeneratedCurves curves = function();
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        checksum += curves.points[repeat % curves.points.size()].y;
        checksum += curves.points[(repeat * 17u) % curves.points.size()].radius;
        checksum += curves.root_uvs[(repeat * 31u) % curves.root_uvs.size()].x;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t p90 = std::min(samples.size() - 1u,
        static_cast<std::size_t>(std::ceil(samples.size() * 0.9)) - 1u);
    return {samples.front(), samples[samples.size() / 2u], samples[p90], checksum};
}

} // namespace

int main(int argc, char **argv) try {
    std::uint32_t repeats = 7u;
    std::vector<std::uint32_t> counts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--repeats") {
            if (++i >= argc) { throw std::invalid_argument("missing repeat count"); }
            repeats = static_cast<std::uint32_t>(std::stoul(argv[i]));
        } else {
            counts.push_back(static_cast<std::uint32_t>(std::stoul(arg)));
        }
    }
    if (counts.empty()) { counts = {10000u, 100000u}; }
    if (repeats == 0u) { throw std::invalid_argument("repeat count must be positive"); }

    const Asset asset = make_asset();
    std::cout << std::setprecision(9)
              << "{\n  \"logical_cpus\": " << std::max(1u, std::thread::hardware_concurrency())
              << ",\n  \"repeats\": " << repeats << ",\n  \"cases\": [\n";
    for (std::size_t case_index = 0u; case_index < counts.size(); ++case_index) {
        const std::uint32_t count = counts[case_index];
        GenerationParams native_params{};
        native_params.strand_count = count;
        native_params.cvs_per_strand = 12u;
        native_params.seed = 23u;
        native_params.root_width = 0.02f;
        native_params.tip_width = 0.02f;
        native_params.noise_amplitude = 0.0f;
        const Timing native = measure(repeats, [&] { return generate_cpu(asset, native_params); });
        const Timing direct_packed = measure_packed(repeats, [&] {
            return generate_packed_cpu(asset, native_params);
        });
        const GeneratedCurves generated = generate_cpu(asset, native_params);
        const Timing renderer_pack = measure_payload(repeats, [&] {
            return build_curve_batches(view_generated_curves(generated));
        });

        const std::vector<LinearCurveSeed> seeds = make_linear_seeds(count);
        LinearModifierReferenceParams reference_params{};
        reference_params.cvs_per_strand = 12u;
        const Timing reference = measure(repeats, [&] {
            return generate_linear_modifier_reference_cpu(seeds, reference_params);
        });
        const double cvs = static_cast<double>(count) * 12.0;
        std::cout << "    {\"strands\": " << count << ", \"cvs\": "
                  << static_cast<std::uint64_t>(count) * 12u
                  << ", \"native_ms\": {\"min\": " << native.minimum_ms
                  << ", \"median\": " << native.median_ms
                  << ", \"p90\": " << native.p90_ms
                  << "}, \"native_mcvs_per_s\": " << cvs / native.median_ms / 1000.0
                  << ", \"direct_renderer_output_ms\": {\"min\": "
                  << direct_packed.minimum_ms
                  << ", \"median\": " << direct_packed.median_ms
                  << ", \"p90\": " << direct_packed.p90_ms
                  << "}, \"direct_renderer_output_mcvs_per_s\": "
                  << cvs / direct_packed.median_ms / 1000.0
                  << ", \"renderer_pack_ms\": {\"min\": " << renderer_pack.minimum_ms
                  << ", \"median\": " << renderer_pack.median_ms
                  << ", \"p90\": " << renderer_pack.p90_ms
                  << "}, \"generate_plus_pack_ms\": "
                  << native.median_ms + renderer_pack.median_ms
                  << ", \"linear_modifier_reference_ms\": {\"min\": " << reference.minimum_ms
                  << ", \"median\": " << reference.median_ms
                  << ", \"p90\": " << reference.p90_ms
                  << "}, \"linear_modifier_reference_mcvs_per_s\": "
                  << cvs / reference.median_ms / 1000.0
                  << ", \"checksum\": "
                  << native.checksum + direct_packed.checksum + renderer_pack.checksum +
                     reference.checksum << "}";
        std::cout << (case_index + 1u == counts.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
