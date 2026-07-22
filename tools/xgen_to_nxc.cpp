#include "nanoxgen/curve_cache.h"
#include "nanoxgen/xgen.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace nanoxgen;

namespace {

float parse_time(const char *text) {
    std::size_t used = 0u;
    const float value = std::stof(text, &used);
    if (text[used] != '\0' || !std::isfinite(value)) {
        throw std::invalid_argument("invalid motion time");
    }
    return value;
}

auto identity(const XGenEvaluatedCurves &curves, std::size_t curve) {
    return std::tuple{
        curves.face_ids[curve], std::bit_cast<std::uint32_t>(curves.face_uvs[curve].x),
        std::bit_cast<std::uint32_t>(curves.face_uvs[curve].y),
        std::bit_cast<std::uint32_t>(curves.patch_uvs[curve].x),
        std::bit_cast<std::uint32_t>(curves.patch_uvs[curve].y)};
}

bool same_topology(const XGenEvaluatedCurves &a, const XGenEvaluatedCurves &b) {
    if (a.point_counts != b.point_counts || a.face_ids.size() != b.face_ids.size()) {
        return false;
    }
    for (std::size_t curve = 0u; curve < a.face_ids.size(); ++curve) {
        if (identity(a, curve) != identity(b, curve)) { return false; }
    }
    return true;
}

bool duplicate_identities(const XGenEvaluatedCurves &curves) {
    for (std::size_t curve = 1u; curve < curves.face_ids.size(); ++curve) {
        if (identity(curves, curve - 1u) == identity(curves, curve)) { return true; }
    }
    return false;
}

} // namespace

int main(int argc, char **argv) try {
    bool renderer_minimal = false;
    std::vector<std::pair<float, std::filesystem::path>> motion_paths;
    std::vector<std::filesystem::path> positional;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--renderer-minimal") {
            renderer_minimal = true;
        } else if (argument == "--motion") {
            if (index + 2 >= argc) {
                throw std::invalid_argument("--motion requires <time> <input.xgen>");
            }
            const float time = parse_time(argv[++index]);
            const std::filesystem::path path = argv[++index];
            motion_paths.emplace_back(time, path);
        } else {
            positional.emplace_back(argument);
        }
    }
    if (positional.size() != 2u) {
        std::cerr << "usage: nanoxgen_xgen_cache [--renderer-minimal] "
                     "[--motion <time> <motion.xgen>]... <base.xgen> <output.nxc>\n";
        return 2;
    }
    const std::uint64_t input_bytes = std::filesystem::file_size(positional[0]);
    const XGenEvaluatedCurves base = materialize_xgen_curves(
        load_xgen_document(positional[0]));
    if (!motion_paths.empty() && duplicate_identities(base)) {
        throw std::runtime_error("base curve identities are ambiguous for motion");
    }
    std::vector<float> motion_times;
    std::vector<Vec3> motion_points;
    for (const auto &[time, path] : motion_paths) {
        if (!motion_times.empty() && time <= motion_times.back()) {
            throw std::runtime_error("motion times must be strictly increasing");
        }
        const XGenEvaluatedCurves sample = materialize_xgen_curves(load_xgen_document(path));
        if (duplicate_identities(sample) || !same_topology(base, sample)) {
            throw std::runtime_error("motion sample topology or curve identities differ");
        }
        motion_times.push_back(time);
        motion_points.insert(motion_points.end(), sample.positions.begin(), sample.positions.end());
    }
    std::vector<PackedCurvePoint> points;
    points.reserve(base.positions.size());
    for (std::size_t point = 0u; point < base.positions.size(); ++point) {
        points.push_back({base.positions[point].x, base.positions[point].y,
                          base.positions[point].z, 0.5f * base.widths[point]});
    }
    const CurveCache cache = renderer_minimal
        ? build_curve_cache({base.point_counts, points, {}, {}, {}, {},
                             motion_times, motion_points})
        : build_curve_cache({base.point_counts, points, base.texcoords, base.patch_uvs,
                             base.face_uvs, base.face_ids, motion_times, motion_points});
    save_curve_cache(cache, positional[1]);
    const CurveCacheHeader header = cache.view().header();
    std::cout << std::setprecision(9)
              << "{\"input_bytes\":" << input_bytes
              << ",\"cache_bytes\":" << header.byte_size
              << ",\"curves\":" << header.strand_count
              << ",\"points\":" << header.point_count
              << ",\"motion_samples\":" << header.motion_sample_count
              << ",\"renderer_minimal\":" << (renderer_minimal ? "true" : "false")
              << ",\"autodesk_runtime\":false"
              << ",\"content_hash\":\"0x" << std::hex << header.content_hash
              << std::dec << "\"}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
