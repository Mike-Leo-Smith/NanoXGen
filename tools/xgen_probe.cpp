#if __has_include(<xgen/src/xgsculptcore/api/XgSplineAPI.h>)
#include <xgen/src/xgsculptcore/api/XgSplineAPI.h>
#elif __has_include(<XGen/XgSplineAPI.h>)
#include <XGen/XgSplineAPI.h>
#else
#error "Autodesk XgSplineAPI.h was not found"
#endif

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <tuple>
#include <vector>

namespace {

std::uint32_t float_bits(float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct CanonicalCurveHash {
    unsigned int face_id{};
    float face_u{};
    float face_v{};
    float patch_u{};
    float patch_v{};
    std::uint64_t hash{};
};

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: nanoxgen_xgen_probe <xgmExportSplineDataInternal output>\n";
        return 2;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::cerr << "failed to open " << argv[1] << '\n';
        return 1;
    }
    std::stringstream bytes;
    bytes << input.rdbuf();
    XGenSplineAPI::XgFnSpline splines;
    if (!splines.load(bytes, bytes.str().size(), 0.0f)) {
        std::cerr << "XgFnSpline::load failed\n";
        return 1;
    }
    if (!splines.executeScript()) {
        std::cerr << "XgFnSpline::executeScript failed\n";
        return 1;
    }
    std::uint64_t batch_count = 0;
    std::uint64_t curve_count = 0;
    std::uint64_t vertex_count = 0;
    unsigned int min_vertices_per_curve = std::numeric_limits<unsigned int>::max();
    unsigned int max_vertices_per_curve = 0;
    float min_width = std::numeric_limits<float>::infinity();
    float max_width = -std::numeric_limits<float>::infinity();
    float min_position[3] = {
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    float max_position[3] = {
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    float min_patch_uv[2] = {
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity()};
    float max_patch_uv[2] = {
        -std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    std::vector<CanonicalCurveHash> curve_hashes;
    const auto hash_bytes = [](std::uint64_t &hash, const void *data, std::size_t size) {
        const auto *bytes = static_cast<const unsigned char *>(data);
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
    };
    for (auto it = splines.iterator(); !it.isDone(); it.next()) {
        ++batch_count;
        curve_count += it.primitiveCount();
        vertex_count += it.vertexCount();
        const unsigned int stride = it.primitiveInfoStride();
        const unsigned int *primitive_infos = it.primitiveInfos();
        const SgVec3f *positions = it.positions();
        const SgVec2f *texcoords = it.texcoords();
        const SgVec2f *patch_uvs = it.patchUVs();
        const SgVec2f *face_uvs = it.faceUV();
        const unsigned int *face_ids = it.faceId();
        const float *widths = it.width();
        if (stride < 2u || !primitive_infos || !positions || !texcoords || !patch_uvs ||
            !face_uvs || !face_ids || !widths) {
            std::cerr << "XGen returned an incomplete spline batch\n";
            return 1;
        }
        for (unsigned int primitive = 0; primitive < it.primitiveCount(); ++primitive) {
            const unsigned int offset = primitive_infos[primitive * stride];
            const unsigned int length = primitive_infos[primitive * stride + 1u];
            if (length == 0u || offset > it.vertexCount() || length > it.vertexCount() - offset) {
                std::cerr << "XGen returned an invalid primitive vertex range\n";
                return 1;
            }
            min_vertices_per_curve = std::min(min_vertices_per_curve, length);
            max_vertices_per_curve = std::max(max_vertices_per_curve, length);
            std::uint64_t curve_hash = 1469598103934665603ull;
            hash_bytes(curve_hash, &length, sizeof(length));
            hash_bytes(curve_hash, &face_ids[primitive], sizeof(face_ids[primitive]));
            hash_bytes(curve_hash, &face_uvs[primitive][0], sizeof(float));
            hash_bytes(curve_hash, &face_uvs[primitive][1], sizeof(float));
            for (unsigned int axis = 0; axis < 2u; ++axis) {
                const float value = patch_uvs[offset][axis];
                if (!std::isfinite(value)) {
                    std::cerr << "XGen returned a non-finite patch UV\n";
                    return 1;
                }
                min_patch_uv[axis] = std::min(min_patch_uv[axis], value);
                max_patch_uv[axis] = std::max(max_patch_uv[axis], value);
                hash_bytes(curve_hash, &value, sizeof(value));
            }
            float previous_v = -std::numeric_limits<float>::infinity();
            for (unsigned int vertex = offset; vertex < offset + length; ++vertex) {
                if (!std::isfinite(widths[vertex]) || widths[vertex] < 0.0f ||
                    !std::isfinite(texcoords[vertex][0]) || !std::isfinite(texcoords[vertex][1]) ||
                    texcoords[vertex][1] < previous_v) {
                    std::cerr << "XGen returned invalid varying spline data\n";
                    return 1;
                }
                previous_v = texcoords[vertex][1];
                min_width = std::min(min_width, widths[vertex]);
                max_width = std::max(max_width, widths[vertex]);
                hash_bytes(curve_hash, &widths[vertex], sizeof(widths[vertex]));
                hash_bytes(curve_hash, &texcoords[vertex][0], sizeof(texcoords[vertex][0]));
                hash_bytes(curve_hash, &texcoords[vertex][1], sizeof(texcoords[vertex][1]));
                for (unsigned int axis = 0; axis < 3u; ++axis) {
                    const float value = positions[vertex][axis];
                    if (!std::isfinite(value)) {
                        std::cerr << "XGen returned a non-finite position\n";
                        return 1;
                    }
                    min_position[axis] = std::min(min_position[axis], value);
                    max_position[axis] = std::max(max_position[axis], value);
                    hash_bytes(curve_hash, &value, sizeof(value));
                }
            }
            curve_hashes.push_back({face_ids[primitive], face_uvs[primitive][0],
                                    face_uvs[primitive][1], patch_uvs[offset][0],
                                    patch_uvs[offset][1], curve_hash});
        }
    }
    if (curve_count == 0u || vertex_count == 0u) {
        std::cerr << "XGen BLOB contains no spline geometry\n";
        return 1;
    }
    std::sort(curve_hashes.begin(), curve_hashes.end(), [](const auto &a, const auto &b) {
        return std::tuple{a.face_id, float_bits(a.face_u), float_bits(a.face_v),
                          float_bits(a.patch_u), float_bits(a.patch_v)} <
               std::tuple{b.face_id, float_bits(b.face_u), float_bits(b.face_v),
                          float_bits(b.patch_u), float_bits(b.patch_v)};
    });
    std::uint64_t canonical_hash = 1469598103934665603ull;
    for (const CanonicalCurveHash &curve : curve_hashes) {
        hash_bytes(canonical_hash, &curve.hash, sizeof(curve.hash));
    }
    std::cout << "{\n"
              << "  \"valid\": true,\n"
              << "  \"motion_samples\": " << splines.sampleCount() << ",\n"
              << "  \"batches\": " << batch_count << ",\n"
              << "  \"curves\": " << curve_count << ",\n"
              << "  \"vertices\": " << vertex_count << ",\n"
              << "  \"canonical_hash\": \"0x" << std::hex << canonical_hash << std::dec << "\",\n"
              << "  \"vertices_per_curve\": {\"min\": " << min_vertices_per_curve
              << ", \"max\": " << max_vertices_per_curve << "},\n"
              << "  \"width\": {\"min\": " << min_width << ", \"max\": " << max_width << "},\n"
              << "  \"position_bounds\": {\"min\": [" << min_position[0] << ", "
              << min_position[1] << ", " << min_position[2] << "], \"max\": ["
              << max_position[0] << ", " << max_position[1] << ", " << max_position[2] << "]},\n"
              << "  \"patch_uv_bounds\": {\"min\": [" << min_patch_uv[0] << ", "
              << min_patch_uv[1] << "], \"max\": [" << max_patch_uv[0] << ", "
              << max_patch_uv[1] << "]}\n"
              << "}\n";
    return 0;
}
