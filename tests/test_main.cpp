#include "nanoxgen/asset.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace nanoxgen;

namespace {

void require(bool condition, const char *message) {
    if (!condition) { throw std::runtime_error(message); }
}

AssetBuildInput fixture() {
    AssetBuildInput input{};
    input.positions = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    input.normals.assign(3u, {0.0f, 1.0f, 0.0f});
    input.texcoords = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    input.triangles = {{0u, 1u, 2u}};
    GuideInput guide{};
    guide.root_normal = {0.0f, 1.0f, 0.0f};
    guide.support_radius = 10.0f;
    guide.cvs = {{0.25f, 0.0f, 0.25f}, {0.25f, 0.5f, 0.25f}, {0.50f, 1.0f, 0.25f}};
    input.guides.push_back(guide);
    return input;
}

void test_blob_round_trip() {
    const Asset asset = build_asset(fixture());
    require(validate_asset(asset.bytes()).empty(), "fresh asset must validate");
    require(asset.view().header().triangle_count == 1u, "triangle count");
    require(asset.view().header().guide_count == 1u, "guide count");

    std::vector<std::byte> corrupt(asset.bytes().begin(), asset.bytes().end());
    corrupt.back() ^= std::byte{1};
    require(validate_asset(corrupt) == "content hash mismatch", "corruption must be detected");
}

void test_determinism_and_roots() {
    const Asset asset = build_asset(fixture());
    GenerationParams params{};
    params.strand_count = 128u;
    params.cvs_per_strand = 7u;
    params.seed = 42u;
    params.noise_amplitude = 0.0f;
    const GeneratedCurves a = generate_cpu(asset, params);
    const GeneratedCurves b = generate_cpu(asset, params);
    require(a.points.size() == b.points.size(), "point count");
    require(std::memcmp(a.points.data(), b.points.data(), a.points.size() * sizeof(Vec3)) == 0,
            "generation must be bitwise deterministic");
    for (std::uint32_t strand = 0; strand < params.strand_count; ++strand) {
        const RootSample root = a.roots[strand];
        require(root.position.x >= 0.0f && root.position.z >= 0.0f && root.position.x + root.position.z <= 1.00001f,
                "root must lie inside the triangle");
        const Vec3 first = a.points[static_cast<std::size_t>(strand) * params.cvs_per_strand];
        require(length_squared(first - root.position) < 1.0e-10f, "first CV must equal the root");
    }
}

void test_guide_interpolation() {
    const Asset asset = build_asset(fixture());
    GenerationParams params{};
    params.strand_count = 1u;
    params.cvs_per_strand = 3u;
    params.seed = 7u;
    const GeneratedCurves curves = generate_cpu(asset, params);
    const Vec3 root = curves.roots.front().position;
    const Vec3 middle = curves.points[1];
    const Vec3 tip = curves.points[2];
    require(std::abs(middle.y - 0.5f) < 1.0e-6f, "guide midpoint height");
    require(std::abs(tip.y - 1.0f) < 1.0e-6f, "guide tip height");
    require(std::abs((tip.x - root.x) - 0.25f) < 1.0e-6f, "root-relative guide bend");
}

void test_cpu_persistent_work_queue() {
    const Asset asset = build_asset(fixture());
    GenerationParams params{};
    params.strand_count = 257u;
    params.cvs_per_strand = 11u;
    params.seed = 91u;
    params.noise_amplitude = 0.03f;

    const GeneratedCurves serial = generate_cpu(asset, params, CpuGenerationOptions{1u, 17u});
    const GeneratedCurves parallel = generate_cpu(asset, params, CpuGenerationOptions{4u, 17u});
    require(std::memcmp(serial.roots.data(), parallel.roots.data(),
                        serial.roots.size() * sizeof(RootSample)) == 0,
            "persistent CPU scheduling must cover every root exactly once");
    require(std::memcmp(serial.points.data(), parallel.points.data(),
                        serial.points.size() * sizeof(Vec3)) == 0,
            "persistent CPU scheduling must preserve deterministic points");
    require(std::memcmp(serial.widths.data(), parallel.widths.data(),
                        serial.widths.size() * sizeof(float)) == 0,
            "persistent CPU scheduling must preserve deterministic widths");
}

void test_area_weighted_sampling() {
    AssetBuildInput input{};
    input.positions = {
        {0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
        {10.0f, 0.0f, 0.0f}, {11.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 1.0f}};
    input.normals.assign(input.positions.size(), {0.0f, 1.0f, 0.0f});
    input.triangles = {{0u, 1u, 2u}, {3u, 4u, 5u}}; // areas 1.0 and 0.5
    GuideInput guide{};
    guide.root_normal = {0.0f, 1.0f, 0.0f};
    guide.support_radius = 100.0f;
    guide.cvs = {{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    input.guides.push_back(guide);
    const Asset asset = build_asset(input);

    GenerationParams params{};
    params.strand_count = 30000u;
    params.cvs_per_strand = 2u;
    params.seed = 123u;
    const GeneratedCurves curves = generate_cpu(asset, params);
    std::uint32_t first_triangle = 0u;
    for (const RootSample root : curves.roots) { first_triangle += root.triangle_index == 0u ? 1u : 0u; }
    const float frequency = static_cast<float>(first_triangle) / static_cast<float>(params.strand_count);
    require(frequency > 0.64f && frequency < 0.69f, "root sampling must track rest triangle area");
}

void test_linear_compatibility_generation() {
    const std::vector<LinearCurveSeed> seeds = {
        {{1.0f, 2.0f, 3.0f}, {2.0f, 4.0f, 2.0f}, {0.25f, 0.75f}, 0.02f},
        {{-1.0f, 0.0f, 1.0f}, {-1.0f, 2.0f, 1.0f}, {0.5f, 0.5f}, 0.04f},
    };
    LinearGenerationParams params{};
    params.cvs_per_strand = 5u;
    params.length_scale = 0.75f;
    params.width_taper = 0.8f;
    params.width_taper_start = 0.25f;
    const GeneratedCurves curves = generate_linear_cpu(seeds, params, {2u, 1u});
    require(curves.strand_count == 2u && curves.cvs_per_strand == 5u,
            "linear compatibility topology");
    require(length_squared(curves.points[0] - seeds[0].root) == 0.0f,
            "linear compatibility root");
    const Vec3 expected_tip = seeds[0].root + (seeds[0].tip - seeds[0].root) * 0.75f;
    require(length_squared(curves.points[4] - expected_tip) == 0.0f,
            "linear compatibility cut length");
    require(curves.widths[0] == 0.02f &&
            std::abs(curves.widths[4] - 0.004f) < 1.0e-8f,
            "linear compatibility width taper");
}

} // namespace

int main() try {
    test_blob_round_trip();
    test_determinism_and_roots();
    test_guide_interpolation();
    test_cpu_persistent_work_queue();
    test_area_weighted_sampling();
    test_linear_compatibility_generation();
    std::cout << "all NanoXGen tests passed\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "test failure: " << e.what() << '\n';
    return 1;
}
