#include "nanoxgen/curve_cache.h"

#if __has_include(<xgen/src/xgsculptcore/api/XgSplineAPI.h>)
#include <xgen/src/xgsculptcore/api/XgSplineAPI.h>
#elif __has_include(<XGen/XgSplineAPI.h>)
#include <XGen/XgSplineAPI.h>
#else
#error "Autodesk XgSplineAPI.h was not found"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace nanoxgen;

namespace {

std::uint32_t float_bits(float value) noexcept {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct CanonicalKey {
    std::uint32_t face_id{};
    std::uint32_t face_u{};
    std::uint32_t face_v{};
    std::uint32_t patch_u{};
    std::uint32_t patch_v{};

    [[nodiscard]] auto tuple() const noexcept {
        return std::tuple{face_id, face_u, face_v, patch_u, patch_v};
    }
};

struct OfficialCurve {
    CanonicalKey key{};
    Vec2 patch_uv{};
    Vec2 face_uv{};
    std::vector<PackedCurvePoint> points;
    std::vector<Vec2> texcoords;
};

struct OfficialCurves {
    std::uint64_t blob_bytes{};
    std::vector<std::uint32_t> point_counts;
    std::vector<PackedCurvePoint> points;
    std::vector<Vec2> texcoords;
    std::vector<Vec2> patch_uvs;
    std::vector<Vec2> face_uvs;
    std::vector<std::uint32_t> face_ids;
    std::vector<CanonicalKey> keys;
};

OfficialCurves load_official(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("failed to open " + path.string()); }
    std::stringstream bytes;
    bytes << input.rdbuf();
    const std::string storage = bytes.str();
    std::stringstream stream{storage};
    XGenSplineAPI::XgFnSpline splines;
    if (!splines.load(stream, storage.size(), 0.0f) || !splines.executeScript()) {
        throw std::runtime_error("XGen failed to evaluate " + path.string());
    }
    if (splines.sampleCount() != 1u) {
        throw std::runtime_error("each curve-cache input must contain one evaluated sample");
    }

    std::vector<OfficialCurve> curves;
    for (auto it = splines.iterator(); !it.isDone(); it.next()) {
        const unsigned int stride = it.primitiveInfoStride();
        const unsigned int *infos = it.primitiveInfos();
        const SgVec3f *positions = it.positions();
        const float *widths = it.width();
        const SgVec2f *texcoords = it.texcoords();
        const SgVec2f *patch_uvs = it.patchUVs();
        const SgVec2f *face_uvs = it.faceUV();
        const unsigned int *face_ids = it.faceId();
        const unsigned int primitive_count = it.primitiveCount();
        const unsigned int vertex_count = it.vertexCount();
        if (stride < 2u || !infos || !positions || !widths || !texcoords ||
            !patch_uvs || !face_uvs || !face_ids) {
            throw std::runtime_error("XGen returned incomplete public spline data");
        }
        for (unsigned int primitive = 0u; primitive < primitive_count; ++primitive) {
            const unsigned int offset = infos[primitive * stride];
            const unsigned int length = infos[primitive * stride + 1u];
            if (length < 2u || offset > vertex_count || length > vertex_count - offset) {
                throw std::runtime_error("XGen returned an invalid primitive range");
            }
            OfficialCurve curve{};
            curve.patch_uv = {patch_uvs[offset][0], patch_uvs[offset][1]};
            curve.face_uv = {face_uvs[primitive][0], face_uvs[primitive][1]};
            curve.key = {face_ids[primitive], float_bits(curve.face_uv.x),
                         float_bits(curve.face_uv.y), float_bits(curve.patch_uv.x),
                         float_bits(curve.patch_uv.y)};
            curve.points.reserve(length);
            curve.texcoords.reserve(length);
            for (unsigned int vertex = offset; vertex < offset + length; ++vertex) {
                curve.points.push_back({positions[vertex][0], positions[vertex][1],
                                        positions[vertex][2], 0.5f * widths[vertex]});
                curve.texcoords.push_back({texcoords[vertex][0], texcoords[vertex][1]});
            }
            curves.emplace_back(std::move(curve));
        }
    }
    std::stable_sort(curves.begin(), curves.end(), [](const OfficialCurve &a, const OfficialCurve &b) {
        return a.key.tuple() < b.key.tuple();
    });
    OfficialCurves result{};
    result.blob_bytes = storage.size();
    result.point_counts.reserve(curves.size());
    result.patch_uvs.reserve(curves.size());
    result.face_uvs.reserve(curves.size());
    result.face_ids.reserve(curves.size());
    result.keys.reserve(curves.size());
    for (OfficialCurve &curve : curves) {
        result.point_counts.push_back(static_cast<std::uint32_t>(curve.points.size()));
        result.patch_uvs.push_back(curve.patch_uv);
        result.face_uvs.push_back(curve.face_uv);
        result.face_ids.push_back(curve.key.face_id);
        result.keys.push_back(curve.key);
        result.points.insert(result.points.end(), curve.points.begin(), curve.points.end());
        result.texcoords.insert(result.texcoords.end(),
                                curve.texcoords.begin(), curve.texcoords.end());
    }
    return result;
}

float parse_time(const char *text) {
    std::size_t used = 0u;
    const float value = std::stof(text, &used);
    if (text[used] != '\0' || !std::isfinite(value)) {
        throw std::invalid_argument("invalid motion time");
    }
    return value;
}

bool same_topology_and_order(const OfficialCurves &a, const OfficialCurves &b) {
    if (a.point_counts != b.point_counts || a.keys.size() != b.keys.size()) { return false; }
    for (std::size_t i = 0u; i < a.keys.size(); ++i) {
        if (a.keys[i].tuple() != b.keys[i].tuple()) { return false; }
    }
    return true;
}

bool has_duplicate_keys(const OfficialCurves &curves) {
    for (std::size_t i = 1u; i < curves.keys.size(); ++i) {
        if (curves.keys[i - 1u].tuple() == curves.keys[i].tuple()) { return true; }
    }
    return false;
}

} // namespace

int main(int argc, char **argv) try {
    bool renderer_minimal = false;
    std::vector<std::pair<float, std::filesystem::path>> motion_paths;
    std::vector<std::filesystem::path> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--renderer-minimal") {
            renderer_minimal = true;
        } else if (argument == "--motion") {
            if (i + 2 >= argc) {
                throw std::invalid_argument("--motion requires <time> <input.xgen>");
            }
            const float time = parse_time(argv[++i]);
            motion_paths.emplace_back(time, argv[++i]);
        } else {
            positional.emplace_back(argument);
        }
    }
    if (positional.size() != 2u) {
        std::cerr << "usage: nanoxgen_xgen_cache [--renderer-minimal] "
                     "[--motion <time> <motion.xgen>]... <base.xgen> <output.nxc>\n";
        return 2;
    }
    const OfficialCurves base = load_official(positional[0]);
    if (!motion_paths.empty() && has_duplicate_keys(base)) {
        throw std::runtime_error(
            "base has duplicate face/UV curve identities; motion matching is ambiguous");
    }
    std::vector<float> motion_times;
    std::vector<Vec3> motion_points;
    motion_times.reserve(motion_paths.size());
    motion_points.reserve(base.points.size() * motion_paths.size());
    for (const auto &[time, path] : motion_paths) {
        const OfficialCurves sample = load_official(path);
        if (has_duplicate_keys(sample)) {
            throw std::runtime_error(
                "motion sample has duplicate face/UV curve identities; matching is ambiguous");
        }
        if (!same_topology_and_order(base, sample)) {
            throw std::runtime_error(
                "motion sample topology/order differs from base; refusing misaligned motion");
        }
        motion_times.push_back(time);
        for (const PackedCurvePoint point : sample.points) {
            motion_points.push_back({point.x, point.y, point.z});
        }
    }

    const CurveCache cache = renderer_minimal
        ? build_curve_cache({base.point_counts, base.points, {}, {}, {}, {},
                             motion_times, motion_points})
        : build_curve_cache({base.point_counts, base.points, base.texcoords, base.patch_uvs,
                             base.face_uvs, base.face_ids, motion_times, motion_points});
    save_curve_cache(cache, positional[1]);
    const CurveCache reloaded = load_curve_cache(positional[1]);
    const CurveCacheView view = reloaded.view();
    const CurveCacheHeader header = view.header();
    if (std::memcmp(view.point_counts(), base.point_counts.data(),
                    base.point_counts.size() * sizeof(std::uint32_t)) != 0 ||
        std::memcmp(view.points(), base.points.data(),
                    base.points.size() * sizeof(PackedCurvePoint)) != 0 ||
        (!motion_times.empty() &&
         (std::memcmp(view.motion_times(), motion_times.data(),
                      motion_times.size() * sizeof(float)) != 0 ||
          std::memcmp(view.motion_points(), motion_points.data(),
                      motion_points.size() * sizeof(Vec3)) != 0))) {
        throw std::runtime_error("cache round trip changed official curve geometry or motion");
    }
    if (!renderer_minimal &&
        (std::memcmp(view.texcoords(), base.texcoords.data(),
                     base.texcoords.size() * sizeof(Vec2)) != 0 ||
         std::memcmp(view.patch_uvs(), base.patch_uvs.data(),
                     base.patch_uvs.size() * sizeof(Vec2)) != 0 ||
         std::memcmp(view.face_uvs(), base.face_uvs.data(),
                     base.face_uvs.size() * sizeof(Vec2)) != 0 ||
         std::memcmp(view.face_ids(), base.face_ids.data(),
                     base.face_ids.size() * sizeof(std::uint32_t)) != 0)) {
        throw std::runtime_error("cache round trip changed official spline attributes");
    }
    std::cout << std::setprecision(9)
              << "{\"input_bytes\": " << base.blob_bytes
              << ", \"cache_bytes\": " << header.byte_size
              << ", \"curves\": " << header.strand_count
              << ", \"points\": " << header.point_count
              << ", \"motion_samples\": " << header.motion_sample_count
              << ", \"renderer_minimal\": " << (renderer_minimal ? "true" : "false")
              << ", \"bit_exact_round_trip\": true"
              << ", \"content_hash\": \"0x" << std::hex << header.content_hash
              << std::dec << "\"}\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
}
