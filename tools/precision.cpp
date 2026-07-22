#include "nanoxgen/asset.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nanoxgen;

namespace {

constexpr std::uint64_t kProbeMagic = 0x314e4f4953494345ull; // "ECISION1"

struct ProbeHeader {
    std::uint64_t magic{kProbeMagic};
    std::uint32_t strand_count{};
    std::uint32_t cvs_per_strand{};
    std::uint64_t point_count{};
};

Asset make_asset() {
    constexpr std::uint32_t nx = 12u;
    constexpr std::uint32_t nz = 10u;
    AssetBuildInput input{};
    for (std::uint32_t z = 0u; z <= nz; ++z) {
        for (std::uint32_t x = 0u; x <= nx; ++x) {
            const float u = static_cast<float>(x) / nx;
            const float v = static_cast<float>(z) / nz;
            const float px = -5.0f + 10.0f * u;
            const float pz = -4.0f + 8.0f * v;
            const float py = 0.23f * std::sin(1.37f * px) * std::cos(1.11f * pz) +
                             0.017f * px * pz;
            input.positions.push_back({px, py, pz});
            input.texcoords.push_back({u, v});
        }
    }
    const auto vertex = [](std::uint32_t x, std::uint32_t z) {
        return z * (nx + 1u) + x;
    };
    for (std::uint32_t z = 0u; z < nz; ++z) {
        for (std::uint32_t x = 0u; x < nx; ++x) {
            const std::uint32_t a = vertex(x, z);
            const std::uint32_t b = vertex(x + 1u, z);
            const std::uint32_t c = vertex(x + 1u, z + 1u);
            const std::uint32_t d = vertex(x, z + 1u);
            input.triangles.push_back({a, b, c});
            input.triangles.push_back({a, c, d});
        }
    }
    for (std::uint32_t guide_index = 0u; guide_index < 16u; ++guide_index) {
        const float u = static_cast<float>((guide_index * 7u) % 17u) / 16.0f;
        const float v = static_cast<float>((guide_index * 11u) % 19u) / 18.0f;
        const float x = -4.5f + 9.0f * u;
        const float z = -3.5f + 7.0f * v;
        const float y = 0.23f * std::sin(1.37f * x) * std::cos(1.11f * z) +
                        0.017f * x * z;
        GuideInput guide{};
        guide.root_normal = normalize({-0.1f * std::cos(x), 1.0f, 0.1f * std::sin(z)});
        guide.support_radius = 4.25f;
        for (std::uint32_t cv = 0u; cv < 10u; ++cv) {
            const float t = static_cast<float>(cv) / 9.0f;
            const float bend = 0.13f + 0.02f * guide_index;
            guide.cvs.push_back({
                x + bend * t * t + 0.025f * std::sin(4.0f * t + guide_index),
                y + (0.7f + 0.03f * guide_index) * t,
                z + 0.11f * t * t + 0.018f * std::cos(5.0f * t + guide_index),
            });
        }
        input.guides.emplace_back(std::move(guide));
    }
    return build_asset(input);
}

GenerationParams fixture_params() {
    GenerationParams params{};
    params.strand_count = 20000u;
    params.cvs_per_strand = 16u;
    params.seed = 0x91ab23cdu;
    params.guide_support_scale = 1.07f;
    params.guide_weight_power = 2.37f;
    params.normal_rejection_cos = -0.18f;
    params.length_scale = 0.83f;
    params.root_width = 0.027f;
    params.tip_width = 0.0017f;
    params.noise_amplitude = 0.043f;
    params.noise_frequency = 3.17f;
    params.noise_mask = 0.83f;
    params.noise_correlation = 0.35f;
    params.noise_preserve_length = 0.4f;
    return params;
}

GeneratedCurves generate_fixture(const Asset &asset) {
    return generate_cpu(asset, fixture_params(), {1u, 128u});
}

void write_fixture(const char *path) {
    const Asset asset = make_asset();
    const GeneratedCurves curves = generate_fixture(asset);
    const ProbeHeader header{
        kProbeMagic, curves.strand_count, curves.cvs_per_strand, curves.points.size()};
    std::ofstream output(path, std::ios::binary);
    if (!output) { throw std::runtime_error(std::string{"failed to open "} + path); }
    output.write(reinterpret_cast<const char *>(&header), sizeof(header));
    output.write(reinterpret_cast<const char *>(curves.points.data()),
                 static_cast<std::streamsize>(curves.points.size() * sizeof(Vec3)));
    output.write(reinterpret_cast<const char *>(curves.widths.data()),
                 static_cast<std::streamsize>(curves.widths.size() * sizeof(float)));
    output.write(reinterpret_cast<const char *>(curves.roots.data()),
                 static_cast<std::streamsize>(curves.roots.size() * sizeof(RootSample)));
    if (!output) { throw std::runtime_error("failed while writing precision fixture"); }
}

void benchmark_fixture(std::uint32_t repeats) {
    if (repeats == 0u) { throw std::invalid_argument("repeat count must be positive"); }
    const Asset asset = make_asset();
    generate_fixture(asset);
    std::vector<double> samples;
    samples.reserve(repeats);
    double checksum = 0.0;
    for (std::uint32_t repeat = 0u; repeat < repeats; ++repeat) {
        const auto begin = std::chrono::steady_clock::now();
        const GeneratedCurves curves = generate_fixture(asset);
        const auto end = std::chrono::steady_clock::now();
        samples.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
        checksum += curves.points[(repeat * 17u) % curves.points.size()].y;
    }
    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2u];
    const double cvs = static_cast<double>(fixture_params().strand_count) *
                       fixture_params().cvs_per_strand;
    std::cout << std::setprecision(9)
              << "{\"repeats\": " << repeats
              << ", \"median_ms\": " << median
              << ", \"mcvs_per_s\": " << cvs / median / 1000.0
              << ", \"checksum\": " << checksum << "}\n";
}

GeneratedCurves read_fixture(const char *path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error(std::string{"failed to open "} + path); }
    ProbeHeader header{};
    input.read(reinterpret_cast<char *>(&header), sizeof(header));
    if (header.magic != kProbeMagic || header.strand_count == 0u ||
        header.cvs_per_strand < 2u ||
        header.point_count != static_cast<std::uint64_t>(header.strand_count) *
                              header.cvs_per_strand) {
        throw std::runtime_error("invalid precision fixture header");
    }
    GeneratedCurves curves{};
    curves.strand_count = header.strand_count;
    curves.cvs_per_strand = header.cvs_per_strand;
    curves.points.resize(header.point_count);
    curves.widths.resize(header.point_count);
    curves.roots.resize(header.strand_count);
    input.read(reinterpret_cast<char *>(curves.points.data()),
               static_cast<std::streamsize>(curves.points.size() * sizeof(Vec3)));
    input.read(reinterpret_cast<char *>(curves.widths.data()),
               static_cast<std::streamsize>(curves.widths.size() * sizeof(float)));
    input.read(reinterpret_cast<char *>(curves.roots.data()),
               static_cast<std::streamsize>(curves.roots.size() * sizeof(RootSample)));
    if (!input || input.peek() != std::ifstream::traits_type::eof()) {
        throw std::runtime_error("invalid precision fixture payload size");
    }
    return curves;
}

std::uint32_t float_bits(float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::uint32_t ordered_bits(float value) {
    const std::uint32_t bits = float_bits(value);
    return (bits & 0x80000000u) != 0u ? ~bits : bits | 0x80000000u;
}

struct ErrorStats {
    std::uint64_t values{};
    std::uint64_t bit_exact{};
    std::uint64_t outside_strict_tolerance{};
    std::uint64_t above_relaxed_continuous_tolerance{};
    float max_abs{};
    float max_relative{};
    std::uint32_t max_ulp{};
    std::uint64_t non_finite_mismatches{};
    long double squared_error{};

    void add(float reference, float candidate) {
        if (!std::isfinite(reference) || !std::isfinite(candidate)) {
            ++values;
            bit_exact += float_bits(reference) == float_bits(candidate);
            non_finite_mismatches += float_bits(reference) != float_bits(candidate);
            return;
        }
        const float absolute = std::abs(reference - candidate);
        const float denominator = std::max(std::abs(reference), 1.0e-20f);
        const float relative = absolute / denominator;
        const std::uint32_t a = ordered_bits(reference);
        const std::uint32_t b = ordered_bits(candidate);
        const std::uint32_t ulp = a > b ? a - b : b - a;
        ++values;
        bit_exact += float_bits(reference) == float_bits(candidate);
        outside_strict_tolerance += absolute > 5.0e-7f && ulp > 4u;
        above_relaxed_continuous_tolerance += absolute > 4.0e-6f;
        max_abs = std::max(max_abs, absolute);
        max_relative = std::max(max_relative, relative);
        max_ulp = std::max(max_ulp, ulp);
        squared_error += static_cast<long double>(absolute) * absolute;
    }
};

double rms_error(const ErrorStats &stats) {
    return stats.values == 0u ? 0.0 :
        std::sqrt(static_cast<double>(stats.squared_error / stats.values));
}

void print_stats(const char *name, const ErrorStats &stats, bool comma) {
    std::cout << "  \"" << name << "\": {\"values\": " << stats.values
              << ", \"bit_exact\": " << stats.bit_exact
              << ", \"outside_5e-7_or_4ulp\": " << stats.outside_strict_tolerance
              << ", \"above_4e-6\": " << stats.above_relaxed_continuous_tolerance
              << ", \"max_abs\": " << stats.max_abs
              << ", \"rms\": " << rms_error(stats)
              << ", \"max_relative\": " << stats.max_relative
              << ", \"max_ulp\": " << stats.max_ulp
              << ", \"non_finite_mismatches\": " << stats.non_finite_mismatches
              << "}"
              << (comma ? ",\n" : "\n");
}

int compare_fixtures(
    const char *reference_path, const char *candidate_path, bool check_relaxed_budget) {
    const GeneratedCurves reference = read_fixture(reference_path);
    const GeneratedCurves candidate = read_fixture(candidate_path);
    if (reference.strand_count != candidate.strand_count ||
        reference.cvs_per_strand != candidate.cvs_per_strand) {
        throw std::runtime_error("precision fixture topology mismatch");
    }
    ErrorStats positions{};
    ErrorStats widths{};
    ErrorStats root_attributes{};
    std::uint64_t root_index_mismatches = 0u;
    for (std::size_t i = 0u; i < reference.points.size(); ++i) {
        positions.add(reference.points[i].x, candidate.points[i].x);
        positions.add(reference.points[i].y, candidate.points[i].y);
        positions.add(reference.points[i].z, candidate.points[i].z);
        widths.add(reference.widths[i], candidate.widths[i]);
    }
    for (std::size_t i = 0u; i < reference.roots.size(); ++i) {
        const RootSample &a = reference.roots[i];
        const RootSample &b = candidate.roots[i];
        const float av[] = {a.position.x, a.position.y, a.position.z,
                            a.normal.x, a.normal.y, a.normal.z,
                            a.uv.x, a.uv.y, a.barycentric.x, a.barycentric.y};
        const float bv[] = {b.position.x, b.position.y, b.position.z,
                            b.normal.x, b.normal.y, b.normal.z,
                            b.uv.x, b.uv.y, b.barycentric.x, b.barycentric.y};
        for (std::size_t value = 0u; value < std::size(av); ++value) {
            root_attributes.add(av[value], bv[value]);
        }
        root_index_mismatches += a.triangle_index != b.triangle_index;
    }
    std::cout << std::setprecision(9) << "{\n";
    print_stats("positions", positions, true);
    print_stats("widths", widths, true);
    print_stats("root_attributes", root_attributes, true);
    // This is a deliberately scale-specific production gate for the fixture
    // above (roughly ten scene units across), not a claim of strict parity.
    // Preserve-length has the same 1e-4 activation threshold as XGen; fast
    // arithmetic can place a few near-threshold strands on the other side of
    // that branch. Bound those discontinuities by count and magnitude instead
    // of widening the continuous error budget for every point. Strict XGen
    // differential tests continue to use 5e-7 absolute OR 4 ULP.
    const bool relaxed_budget_pass =
        positions.non_finite_mismatches == 0u &&
        widths.non_finite_mismatches == 0u &&
        root_attributes.non_finite_mismatches == 0u &&
        root_index_mismatches == 0u &&
        positions.max_abs <= 1.1e-4f && rms_error(positions) <= 5.0e-7 &&
        positions.above_relaxed_continuous_tolerance <= 100u &&
        widths.max_abs <= 5.0e-9f &&
        root_attributes.max_abs <= 2.0e-6f;
    std::cout << "  \"root_index_mismatches\": " << root_index_mismatches
              << ",\n  \"relaxed_budget_pass\": "
              << (relaxed_budget_pass ? "true" : "false") << "\n}\n";
    return check_relaxed_budget && !relaxed_budget_pass ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) try {
    if (argc == 3 && std::string{argv[1]} == "write") {
        write_fixture(argv[2]);
        return 0;
    }
    if (argc == 4 && std::string{argv[1]} == "compare") {
        return compare_fixtures(argv[2], argv[3], false);
    }
    if (argc == 4 && std::string{argv[1]} == "check-relaxed") {
        return compare_fixtures(argv[2], argv[3], true);
    }
    if ((argc == 2 || argc == 3) && std::string{argv[1]} == "benchmark") {
        const std::uint32_t repeats = argc == 3
            ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 11u;
        benchmark_fixture(repeats);
        return 0;
    }
    std::cerr << "usage: nanoxgen_precision write <output.bin>\n"
                 "       nanoxgen_precision compare <reference.bin> <candidate.bin>\n"
                 "       nanoxgen_precision check-relaxed <reference.bin> <candidate.bin>\n"
                 "       nanoxgen_precision benchmark [repeats]\n";
    return 2;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
